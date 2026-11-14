// Platform-neutral parts of the macOS agent (built into archon_tests on every OS).

#include <cmath>
#include <vector>

#include "../../core/tests/test.hpp"
#include "../src/portable.hpp"
#include "../src/tap_mix.hpp"
#include "archon/crypto.hpp"

using namespace agent;

TEST(mac_config_roundtrip_and_defaults) {
    AgentConfig c;
    c.agent_id = arc::random_array<16>();
    c.have_id = true;
    c.port = 50123;
    c.lockouts = 3;
    c.locked_until = 1800000000;
    const AgentConfig back = parse_config(serialize_config(c));
    CHECK(back.have_id);
    CHECK(back.agent_id == c.agent_id);
    CHECK(back.port == 50123);
    CHECK(back.lockouts == 3);
    CHECK(back.locked_until == 1800000000);

    const AgentConfig empty = parse_config("");
    CHECK(!empty.have_id);
    CHECK(empty.port == 47321);
}

TEST(mac_config_rejects_bad_values_and_reads_windows_files) {
    const AgentConfig bad = parse_config("id=zz\nport=70000\nlockouts=-4\nlocked_until=-1\n");
    CHECK(!bad.have_id);
    CHECK(bad.port == 47321);
    CHECK(bad.lockouts == 0);
    CHECK(bad.locked_until == 0);
    // CRLF line endings and unknown keys (a file written on Windows).
    const AgentConfig win =
        parse_config("# Archon agent configuration\r\nid=b118dbbf39665d441d93d9cd126e3480\r\nport=47321\r\nfoo=1\r\n");
    CHECK(win.have_id);
    CHECK(win.port == 47321);
}

TEST(mac_trust_blob_seals_and_detects_tampering) {
    const arc::Key32 key = arc::random_array<32>();
    const arc::Bytes plain = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    arc::Bytes sealed, back;
    REQUIRE(seal_blob(key, plain, sealed));
    CHECK(sealed.size() == 4 + 24 + 16 + plain.size());
    CHECK(open_blob(key, sealed, back));
    CHECK(back == plain);

    arc::Bytes tampered = sealed;
    tampered.back() ^= 1;
    CHECK(!open_blob(key, tampered, back));
    CHECK(back.empty());

    arc::Key32 other = key;
    other[0] ^= 1;
    CHECK(!open_blob(other, sealed, back));
    CHECK(!open_blob(key, arc::Bytes{'A', 'R', 'K'}, back));

    arc::Bytes empty_sealed;
    REQUIRE(seal_blob(key, {}, empty_sealed));
    CHECK(open_blob(key, empty_sealed, back));
    CHECK(back.empty());

    // Fresh nonce every time: sealing twice never gives the same bytes.
    arc::Bytes again;
    seal_blob(key, plain, again);
    CHECK(again != sealed);
}

TEST(mac_mix_levels_gain_and_meter_curve) {
    CHECK(mix::to_level(0.5f) == 5000);
    CHECK(mix::to_level(2.0f) == 10000);
    CHECK(std::fabs(mix::to_scalar(2500) - 0.25f) < 1e-6f);
    CHECK(mix::gain_for(10000, false) == 1.0f);
    CHECK(mix::gain_for(10000, true) == 0.0f);
    CHECK(std::fabs(mix::gain_for(5000, false) - 0.25f) < 1e-6f);  // about -12 dB
    CHECK(mix::meter_byte(0.0f) == 0);
    CHECK(mix::meter_byte(1.0f) == 255);
    CHECK(mix::meter_byte(0.25f) == 128);  // sqrt curve, like Windows
}

TEST(mac_mix_render_maps_channels_and_ramps) {
    // Interleaved stereo in -> interleaved 4-channel out (extra outputs silenced).
    const size_t frames = 5;
    std::vector<float> in(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        in[f * 2] = 0.5f;
        in[f * 2 + 1] = -0.5f;
    }
    std::vector<float> out(frames * 4, 9.0f);
    const mix::ConstChan ic[2] = {{in.data(), 2}, {in.data() + 1, 2}};
    const mix::Chan oc[4] = {{out.data(), 4}, {out.data() + 1, 4}, {out.data() + 2, 4}, {out.data() + 3, 4}};
    const float peak = mix::render(ic, 2, oc, 4, frames, 0.0f, 1.0f);
    CHECK(std::fabs(peak - 0.5f) < 1e-6f);
    CHECK(out[0] == 0.0f);                              // ramp starts at g0
    CHECK(std::fabs(out[(frames - 1) * 4] - 0.5f) < 1e-6f);  // and ends at g1
    CHECK(std::fabs(out[(frames - 1) * 4 + 1] + 0.5f) < 1e-6f);
    for (size_t f = 0; f < frames; ++f) {
        CHECK(out[f * 4 + 2] == 0.0f);
        CHECK(out[f * 4 + 3] == 0.0f);
    }
    // Monotonic ramp (no jump in the middle of the block).
    for (size_t f = 1; f < frames; ++f) CHECK(out[f * 4] >= out[(f - 1) * 4]);
}

TEST(mac_mix_render_mono_to_stereo_and_mute) {
    const size_t frames = 4;
    std::vector<float> mono = {0.2f, 0.4f, -0.8f, 0.1f};
    std::vector<float> l(frames), r(frames);  // non-interleaved outputs
    const mix::ConstChan ic[1] = {{mono.data(), 1}};
    const mix::Chan oc[2] = {{l.data(), 1}, {r.data(), 1}};
    float peak = mix::render(ic, 1, oc, 2, frames, 1.0f, 1.0f);
    CHECK(l == mono);
    CHECK(r == mono);
    CHECK(std::fabs(peak - 0.8f) < 1e-6f);
    peak = mix::render(ic, 1, oc, 2, frames, 0.0f, 0.0f);
    CHECK(peak == 0.0f);
    for (float v : l) CHECK(v == 0.0f);
    // No input at all: outputs silenced, never left with stale samples.
    std::vector<float> junk = {1, 1, 1, 1};
    const mix::Chan oj[1] = {{junk.data(), 1}};
    mix::render(nullptr, 0, oj, 1, frames, 1.0f, 1.0f);
    for (float v : junk) CHECK(v == 0.0f);
}

TEST(mac_mix_peak_and_approach) {
    std::vector<float> s = {0.1f, -0.9f, 0.3f};
    const mix::ConstChan ic[1] = {{s.data(), 1}};
    CHECK(std::fabs(mix::peak_of(ic, 1, 3) - 0.9f) < 1e-6f);
    CHECK(std::fabs(mix::peak_of(ic, 1, 3, 0.5f) - 0.45f) < 1e-6f);
    CHECK(mix::approach(0.0f, 1.0f) == 0.25f);
    CHECK(mix::approach(0.9f, 1.0f) == 1.0f);
    CHECK(mix::approach(1.0f, 0.0f) == 0.75f);
}
