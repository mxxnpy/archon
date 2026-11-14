// "Listen on the phone" codec path, end to end: a 440 Hz tone is encoded by
// OpusStreamer, sealed and sent over UDP to the core's StreamReceiver, then
// decoded with libopus (like the phone's platform decoder would).

#include <opus.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include "../../core/tests/test.hpp"
#include "archon/crypto.hpp"
#include "archon/stream.hpp"
#include "opus_streamer.hpp"

using namespace std::chrono_literals;

namespace {

class Decode : public arc::StreamListener {
public:
    std::mutex mu;
    std::vector<float> left;  // decoded left channel
    int frames = 0, lost = 0;
    OpusDecoder* dec = nullptr;
    Decode() {
        int e = 0;
        dec = opus_decoder_create(48000, 2, &e);
    }
    ~Decode() override { opus_decoder_destroy(dec); }
    void on_audio(const uint8_t* p, size_t n, uint64_t, bool was_lost) override {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<float> pcm(960 * 2);
        // A lost frame decodes as concealment (null packet), like the apps do.
        const int got = opus_decode_float(dec, was_lost ? nullptr : p, was_lost ? 0 : opus_int32(n), pcm.data(), 960, 0);
        if (was_lost) ++lost;
        if (got <= 0) return;
        ++frames;
        for (int i = 0; i < got; ++i) left.push_back(pcm[size_t(i) * 2]);
    }
};

}  // namespace

TEST(stream_opus_tone_survives_encode_send_receive_decode) {
    Decode d;
    REQUIRE(d.dec != nullptr);
    arc::StreamReceiver rx(d);
    std::string err;
    const uint16_t port = rx.open(&err);
    REQUIRE(port != 0);
    const arc::Key32 key = arc::random_array<32>();
    rx.start(key, "127.0.0.1");

    agent::OpusStreamer tx("127.0.0.1", port, key);
    REQUIRE(tx.init(&err));
    // 1 s of a 440 Hz tone at -6 dBFS, pushed in uneven chunks (like a capture thread).
    const int total = 48000;
    std::vector<float> pcm(size_t(total) * 2);
    for (int i = 0; i < total; ++i) {
        const float v = 0.5f * float(std::sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0));
        pcm[size_t(i) * 2] = pcm[size_t(i) * 2 + 1] = v;
    }
    for (int at = 0; at < total;) {
        const int n = std::min(441, total - at);  // 9.19 ms chunks: frames straddle pushes
        tx.push(pcm.data() + size_t(at) * 2, size_t(n));
        at += n;
        std::this_thread::sleep_for(1ms);
    }
    CHECK(tx.sent() == 50);  // 1 s / 20 ms
    const auto until = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < until) {
        {
            std::lock_guard<std::mutex> lk(d.mu);
            if (d.frames >= 50) break;
        }
        std::this_thread::sleep_for(10ms);
    }
    std::lock_guard<std::mutex> lk(d.mu);
    CHECK(d.frames == 50);
    CHECK(d.lost == 0);
    // Skip the codec's start-up (first 100 ms), then measure level and pitch.
    const size_t from = 4800;
    REQUIRE(d.left.size() > from + 24000);
    double sum = 0;
    int crossings = 0;
    for (size_t i = from; i < from + 24000; ++i) {  // 0.5 s
        sum += double(d.left[i]) * d.left[i];
        if (i > from && (d.left[i - 1] < 0) != (d.left[i] < 0)) ++crossings;
    }
    const double rms = std::sqrt(sum / 24000);
    std::printf("  decoded rms %.3f (sine 0.5 -> 0.354), zero crossings in 0.5 s: %d (440 Hz -> 440)\n", rms, crossings);
    CHECK(rms > 0.30 && rms < 0.40);
    CHECK(crossings > 430 && crossings < 450);
}

TEST(stream_silence_keeps_the_clock_running) {
    Decode d;
    arc::StreamReceiver rx(d);
    std::string err;
    const uint16_t port = rx.open(&err);
    REQUIRE(port != 0);
    const arc::Key32 key = arc::random_array<32>();
    rx.start(key, "127.0.0.1");
    agent::OpusStreamer tx("127.0.0.1", port, key);
    REQUIRE(tx.init(&err));
    tx.push_silence(960 * 10);  // nothing playing on the PC for 200 ms
    CHECK(tx.sent() == 10);
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < until) {
        {
            std::lock_guard<std::mutex> lk(d.mu);
            if (d.frames >= 10) break;
        }
        std::this_thread::sleep_for(10ms);
    }
    std::lock_guard<std::mutex> lk(d.mu);
    CHECK(d.frames == 10);
    float peak = 0;
    for (float v : d.left) peak = std::max(peak, std::fabs(v));
    CHECK(peak < 0.001f);
}

TEST(stream_other_key_is_never_played) {
    Decode d;
    arc::StreamReceiver rx(d);
    std::string err;
    const uint16_t port = rx.open(&err);
    REQUIRE(port != 0);
    rx.start(arc::random_array<32>(), "127.0.0.1");
    agent::OpusStreamer tx("127.0.0.1", port, arc::random_array<32>());  // someone else's stream
    REQUIRE(tx.init(&err));
    tx.push_silence(960 * 5);
    std::this_thread::sleep_for(200ms);
    std::lock_guard<std::mutex> lk(d.mu);
    CHECK(d.frames == 0);
    CHECK(rx.rejected() == 5);
}
