#include "archon/crypto.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "monocypher.h"
#include "archon/protocol.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <Security/SecRandom.h>
#elif defined(ESP_PLATFORM)
#include <esp_random.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#endif

namespace arc {

namespace {
void audio_nonce(uint64_t seq, uint8_t nonce[24]) {
    std::memset(nonce, 0, 24);
    for (int i = 0; i < 8; ++i) nonce[i] = uint8_t(seq >> (8 * i));
    std::memcpy(nonce + 8, "ARC1S", 5);
}
void audio_header(uint64_t seq, uint32_t ts, uint8_t codec, uint8_t h[16]) {
    h[0] = 1;  // version
    h[1] = codec;
    h[2] = h[3] = 0;
    for (int i = 0; i < 8; ++i) h[4 + i] = uint8_t(seq >> (56 - 8 * i));  // big-endian like the rest of the wire
    for (int i = 0; i < 4; ++i) h[12 + i] = uint8_t(ts >> (24 - 8 * i));
}
}  // namespace

Bytes seal_audio(const Key32& key, uint64_t seq, uint32_t timestamp, uint8_t codec, const uint8_t* payload, size_t n) {
    if (n > kMaxAudioPayload) n = 0;
    Bytes out(kAudioHeader + 16 + n);
    audio_header(seq, timestamp, codec, out.data());
    uint8_t nonce[24];
    audio_nonce(seq, nonce);
    crypto_aead_lock(out.data() + kAudioHeader + 16, out.data() + kAudioHeader, key.data(), nonce, out.data(),
                     kAudioHeader, payload, n);
    return out;
}

bool open_audio(const Key32& key, const uint8_t* d, size_t n, uint64_t& seq, uint32_t& timestamp, uint8_t& codec,
                Bytes& payload) {
    if (n < kAudioHeader + 16 || n > kAudioHeader + 16 + kMaxAudioPayload || d[0] != 1 || d[2] || d[3]) return false;
    uint64_t s = 0;
    for (int i = 0; i < 8; ++i) s = (s << 8) | d[4 + i];
    uint32_t t = 0;
    for (int i = 0; i < 4; ++i) t = (t << 8) | d[12 + i];
    uint8_t nonce[24];
    audio_nonce(s, nonce);
    payload.assign(n - kAudioHeader - 16, 0);
    if (crypto_aead_unlock(payload.data(), d + kAudioHeader, key.data(), nonce, d, kAudioHeader,
                           d + kAudioHeader + 16, payload.size()) != 0) {
        payload.clear();
        return false;
    }
    seq = s;
    timestamp = t;
    codec = d[1];
    return true;
}

void random_bytes(uint8_t* out, size_t n) {
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        std::abort();
#elif defined(__APPLE__)
    if (SecRandomCopyBytes(kSecRandomDefault, n, out) != errSecSuccess) std::abort();
#elif defined(ESP_PLATFORM)
    esp_fill_random(out, n);
#else
    size_t got = 0;
#if defined(SYS_getrandom)
    while (got < n) {
        long r = syscall(SYS_getrandom, out + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;  // fall back to /dev/urandom
        }
        got += size_t(r);
    }
#endif
    if (got < n) {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) std::abort();
        while (got < n) {
            ssize_t r = read(fd, out + got, n - got);
            if (r <= 0) {
                if (r < 0 && errno == EINTR) continue;
                std::abort();
            }
            got += size_t(r);
        }
        close(fd);
    }
#endif
}

std::string generate_pairing_code() {
    // Rejection sampling keeps the distribution uniform over 10^6 values.
    const uint32_t limit = 4294000000u;  // largest multiple of 10^6 below 2^32
    uint32_t v;
    do {
        uint8_t b[4];
        random_bytes(b, 4);
        v = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
    } while (v >= limit);
    char buf[8];
    std::snprintf(buf, sizeof buf, "%06u", unsigned(v % 1000000u));
    return buf;
}

bool is_valid_pairing_code(std::string_view code) {
    if (code.size() != 6) return false;
    for (char c : code)
        if (c < '0' || c > '9') return false;
    return true;
}

bool ct_equal(const Key32& a, const Key32& b) { return crypto_verify32(a.data(), b.data()) == 0; }

void wipe(void* p, size_t n) { crypto_wipe(p, n); }

Key32 kdf(ByteView key, std::initializer_list<ByteView> parts) {
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, 32, key.n ? key.p : nullptr, key.n);
    for (const ByteView& v : parts) crypto_blake2b_update(&ctx, v.p, v.n);
    Key32 out;
    crypto_blake2b_final(&ctx, out.data());
    return out;
}

Key32 cpace_generator(std::string_view code, const Id16& sid, const Id16& agent_id,
                      const Id16& controller_id) {
    const uint8_t len[2] = {uint8_t(code.size() >> 8), uint8_t(code.size())};
    Key32 rep = kdf(ByteView(nullptr, 0), {"ARC1-CPace-generator", ByteView(len, 2), code, sid,
                                           agent_id, controller_id});
    Key32 g;
    crypto_elligator_map(g.data(), rep.data());
    wipe(rep.data(), rep.size());
    return g;
}

void cpace_start(const Key32& generator, CpaceParty& party) {
    random_bytes(party.secret.data(), party.secret.size());
    crypto_x25519(party.y.data(), party.secret.data(), generator.data());
}

bool cpace_finish(const CpaceParty& party, const Key32& peer_y, Key32& k) {
    crypto_x25519(k.data(), party.secret.data(), peer_y.data());
    uint8_t acc = 0;
    for (uint8_t b : k) acc |= b;
    return acc != 0;
}

SessionKeys::~SessionKeys() {
    wipe(tag_controller.data(), 32);
    wipe(tag_agent.data(), 32);
    wipe(key_c2a.data(), 32);
    wipe(key_a2c.data(), 32);
    wipe(pair_key.data(), 32);
}

namespace {
struct Lp {  // u16 big-endian length prefix for variable fields in transcripts
    uint8_t b[2];
    explicit Lp(std::string_view v) : b{uint8_t(v.size() >> 8), uint8_t(v.size())} {}
    ByteView view() const { return ByteView(b, 2); }
};
}  // namespace

SessionKeys derive_pair_keys(const Key32& k, const Id16& sid, const Id16& agent_id,
                             const Id16& controller_id, const Key32& ys, const Key32& yc,
                             std::string_view controller_name, std::string_view agent_name) {
    const Lp lc(controller_name), la(agent_name);
    // T = sid || agent_id || controller_id || Ys || Yc || lp(controller_name) || lp(agent_name)
    // (the braced list is built inside each kdf() call so it outlives the call)
    auto h = [&](const Key32& key, const char* label) {
        return kdf(key, {label, sid, agent_id, controller_id, ys, yc, lc.view(), controller_name, la.view(),
                         agent_name});
    };
    Key32 isk = h(k, "ARC1-ISK");
    SessionKeys s;
    s.tag_controller = h(isk, "ARC1-confirm-controller");
    s.tag_agent = h(isk, "ARC1-confirm-agent");
    s.pair_key = h(isk, "ARC1-pair-key");
    s.key_c2a = h(isk, "ARC1-key-c2a");
    s.key_a2c = h(isk, "ARC1-key-a2c");
    wipe(isk.data(), isk.size());
    return s;
}

SessionKeys derive_resume_keys(const Key32& pair_key, const Id16& controller_id,
                               const Id16& agent_id, const Key32& nonce_c, const Key32& nonce_s,
                               std::string_view controller_name, std::string_view agent_name) {
    const Lp lc(controller_name), la(agent_name);
    Key32 sk = kdf(pair_key, {"ARC1-resume", controller_id, agent_id, nonce_c, nonce_s, lc.view(),
                              controller_name, la.view(), agent_name});
    SessionKeys s;
    s.tag_agent = kdf(sk, {"ARC1-confirm-agent"});
    s.tag_controller = kdf(sk, {"ARC1-confirm-controller"});
    s.key_c2a = kdf(sk, {"ARC1-key-c2a"});
    s.key_a2c = kdf(sk, {"ARC1-key-a2c"});
    wipe(sk.data(), sk.size());
    return s;
}

static void make_nonce(uint64_t ctr, uint8_t nonce[24]) {
    for (int i = 0; i < 24; ++i) nonce[i] = 0;
    for (int i = 0; i < 8; ++i) nonce[i] = uint8_t(ctr >> (8 * i));
}

SecureChannel::~SecureChannel() {
    wipe(send_key_.data(), 32);
    wipe(recv_key_.data(), 32);
}

void SecureChannel::init(const Key32& send_key, const Key32& recv_key) {
    send_key_ = send_key;
    recv_key_ = recv_key;
    send_ctr_ = recv_ctr_ = 0;
    active_ = true;
}

Bytes SecureChannel::seal(const Bytes& plain) {
    Bytes out(16 + plain.size());
    uint8_t nonce[24];
    make_nonce(send_ctr_++, nonce);
    crypto_aead_lock(out.data() + 16, out.data(), send_key_.data(), nonce, nullptr, 0,
                     plain.data(), plain.size());
    return out;
}

bool SecureChannel::open(const uint8_t* payload, size_t n, Bytes& plain) {
    if (n < 16) return false;
    plain.resize(n - 16);
    uint8_t nonce[24];
    make_nonce(recv_ctr_, nonce);
    if (crypto_aead_unlock(plain.data(), payload, recv_key_.data(), nonce, nullptr, 0,
                           payload + 16, n - 16) != 0) {
        plain.clear();
        return false;
    }
    ++recv_ctr_;
    return true;
}

}  // namespace arc
