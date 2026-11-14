#pragma once
// Pairing (CPace-style PAKE), key derivation and the secure channel.
// Every construction is specified in docs/protocol.md §3–§4.

#include <initializer_list>
#include <string>
#include <string_view>

#include "archon/bytes.hpp"

namespace arc {

// Cryptographically secure random bytes from the operating system.
void random_bytes(uint8_t* out, size_t n);
template <size_t N>
std::array<uint8_t, N> random_array() {
    std::array<uint8_t, N> a;
    random_bytes(a.data(), N);
    return a;
}

// Uniformly random 6-digit code ("000000".."999999").
std::string generate_pairing_code();
bool is_valid_pairing_code(std::string_view code);

bool ct_equal(const Key32& a, const Key32& b);
void wipe(void* p, size_t n);

struct ByteView {
    const uint8_t* p;
    size_t n;
    ByteView(const uint8_t* p_, size_t n_) : p(p_), n(n_) {}
    template <size_t N>
    ByteView(const std::array<uint8_t, N>& a) : p(a.data()), n(N) {}
    ByteView(std::string_view s) : p(reinterpret_cast<const uint8_t*>(s.data())), n(s.size()) {}
    ByteView(const char* s) : ByteView(std::string_view(s)) {}
};

// H(key, part1 || part2 || ...) — BLAKE2b, 32-byte output.
Key32 kdf(ByteView key, std::initializer_list<ByteView> parts);

// ---- Pairing ---------------------------------------------------------------

struct CpaceParty {
    Key32 secret{};
    Key32 y{};  // public share
    ~CpaceParty() { wipe(secret.data(), secret.size()); }
};

Key32 cpace_generator(std::string_view code, const Id16& sid, const Id16& agent_id,
                      const Id16& controller_id);
void cpace_start(const Key32& generator, CpaceParty& party);
// Computes K. Returns false if the peer share is degenerate (K == 0).
bool cpace_finish(const CpaceParty& party, const Key32& peer_y, Key32& k);

struct SessionKeys {
    Key32 tag_controller{};
    Key32 tag_agent{};
    Key32 key_c2a{};
    Key32 key_a2c{};
    Key32 pair_key{};  // pairing only
    ~SessionKeys();
};

// Device names (as cleaned by clean_device_name) are bound into both
// transcripts so a man-in-the-middle cannot relabel either side.
SessionKeys derive_pair_keys(const Key32& k, const Id16& sid, const Id16& agent_id,
                             const Id16& controller_id, const Key32& ys, const Key32& yc,
                             std::string_view controller_name = {}, std::string_view agent_name = {});
SessionKeys derive_resume_keys(const Key32& pair_key, const Id16& controller_id,
                               const Id16& agent_id, const Key32& nonce_c, const Key32& nonce_s,
                               std::string_view controller_name = {}, std::string_view agent_name = {});

// ---- Secure channel ----------------------------------------------------------

class SecureChannel {
public:
    SecureChannel() = default;
    ~SecureChannel();
    SecureChannel(const SecureChannel&) = delete;
    SecureChannel& operator=(const SecureChannel&) = delete;

    void init(const Key32& send_key, const Key32& recv_key);
    bool active() const { return active_; }
    // plain message -> frame payload (mac || ciphertext)
    Bytes seal(const Bytes& plain);
    // frame payload -> plain message. False on authentication failure.
    bool open(const uint8_t* payload, size_t n, Bytes& plain);

private:
    Key32 send_key_{};
    Key32 recv_key_{};
    uint64_t send_ctr_ = 0;
    uint64_t recv_ctr_ = 0;
    bool active_ = false;
};

}  // namespace arc
