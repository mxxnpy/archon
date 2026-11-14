// Feeds random and mutated bytes into every decoder and into the controller
// state machine. Passing means: no crash, no hang, no out-of-bounds read
// (run under ASan in CI on Linux for the memory guarantee).

#include <random>

#include "archon/controller_session.hpp"
#include "archon/frame.hpp"
#include "test.hpp"

using namespace arc;

static Bytes random_bytes_of(std::mt19937& rng, size_t max_len) {
    std::uniform_int_distribution<size_t> len(0, max_len);
    std::uniform_int_distribution<int> byte(0, 255);
    Bytes b(len(rng));
    for (auto& x : b) x = uint8_t(byte(rng));
    return b;
}

static void decode_all(const Bytes& b) {
    {
        Reader r(b.data(), b.size());
        Hello h;
        decode_hello(r, h);
    }
    {
        Reader r(b.data(), b.size());
        HelloAck a;
        decode_hello_ack(r, a);
    }
    {
        Reader r(b.data(), b.size());
        EndpointInfo e;
        decode_endpoint(r, e);
    }
    {
        Reader r(b.data(), b.size());
        SourceInfo s;
        decode_source(r, s);
    }
    {
        Reader r(b.data(), b.size());
        std::string k;
        Bytes png;
        decode_icon(r, k, png);
    }
    {
        Reader r(b.data(), b.size());
        ErrorMsg e;
        decode_error(r, e);
    }
    {
        Reader r(b.data(), b.size());
        std::string id;
        uint16_t v;
        decode_set_volume(r, id, v);
    }
    deserialize_pairing(b.data(), b.size());
}

TEST(fuzz_decoders_random_input) {
    std::mt19937 rng(1234);
    for (int i = 0; i < 20000; ++i) decode_all(random_bytes_of(rng, 300));
    CHECK(true);
}

TEST(fuzz_decoders_mutated_valid_messages) {
    std::mt19937 rng(99);
    SourceInfo s;
    s.id = "endpoint|42";
    s.endpoint_id = "endpoint";
    s.name = "Navegador";
    s.icon_key = "abc";
    std::vector<Bytes> seeds = {encode_source(s), encode_endpoint({"e", "Fone", 5000, false, true}),
                                encode_error({ErrorCode::WrongCode, "x"}), serialize_pairing(PairingRecord{})};
    for (int i = 0; i < 20000; ++i) {
        Bytes b = seeds[size_t(i) % seeds.size()];
        std::uniform_int_distribution<size_t> pos(0, b.size() - 1);
        for (int m = 0; m < 3; ++m) b[pos(rng)] = uint8_t(rng());
        if (rng() % 4 == 0) b.resize(pos(rng));
        decode_all(Bytes(b.begin() + (b.empty() ? 0 : 1), b.end()));
        decode_all(b);
    }
    CHECK(true);
}

TEST(fuzz_controller_session_survives_garbage) {
    std::mt19937 rng(7);
    for (int i = 0; i < 3000; ++i) {
        ControllerSession cs(random_array<16>(), "fuzz");
        if (i % 2)
            cs.begin_pair();
        else
            cs.begin_resume(PairingRecord{});
        for (int k = 0; k < 5; ++k) {
            Bytes b = random_bytes_of(rng, 200);
            if (b.empty()) continue;
            if (k == 0 && rng() % 2) b[0] = uint8_t(MsgType::HelloAck);
            ControllerSession::Output o;
            cs.on_frame(b.data(), b.size(), o);
            if (o.close) break;
        }
    }
    CHECK(true);
}

TEST(fuzz_frame_assembler_random_stream) {
    std::mt19937 rng(5);
    for (int i = 0; i < 2000; ++i) {
        FrameAssembler a;
        Bytes out;
        for (int k = 0; k < 10; ++k) {
            Bytes b = random_bytes_of(rng, 64);
            a.feed(b.data(), b.size());
            int guard = 0;
            FrameAssembler::Result r;
            while ((r = a.next(out)) == FrameAssembler::Result::Frame && guard++ < 1000) {
                CHECK(!out.empty() && out.size() <= kMaxFrame);
            }
            if (r == FrameAssembler::Result::Error) break;
        }
    }
}
