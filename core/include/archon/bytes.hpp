#pragma once
// Big-endian binary writer/reader used by the wire protocol.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace arc {

using Bytes = std::vector<uint8_t>;
using Id16 = std::array<uint8_t, 16>;
using Key32 = std::array<uint8_t, 32>;

class Writer {
public:
    Writer& u8(uint8_t v);
    Writer& u16(uint16_t v);
    Writer& u32(uint32_t v);
    Writer& u64(uint64_t v);
    Writer& raw(const uint8_t* p, size_t n);
    template <size_t N>
    Writer& arr(const std::array<uint8_t, N>& a) { return raw(a.data(), N); }
    // Length-prefixed (u16) UTF-8 string. Truncated at max_len bytes on a
    // UTF-8 boundary so the writer can never emit an oversize field.
    Writer& str(std::string_view s, size_t max_len = 65535);

    const Bytes& data() const { return buf_; }
    Bytes take() { return std::move(buf_); }

private:
    Bytes buf_;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    template <size_t N>
    std::array<uint8_t, N> arr() {
        std::array<uint8_t, N> a{};
        const uint8_t* src = take(N);
        if (src) {
            for (size_t i = 0; i < N; ++i) a[i] = src[i];
        }
        return a;
    }
    // Reads a u16-prefixed string; fails if longer than max_len. Invalid UTF-8
    // sequences are replaced with U+FFFD and NUL bytes are removed, so the
    // result is always safe to hand to C strings, JNI and Swift.
    std::string str(size_t max_len);
    // Returns a pointer to n raw bytes inside the buffer, or nullptr.
    const uint8_t* take(size_t n);

    bool ok() const { return ok_; }
    bool at_end() const { return ok_ && pos_ == n_; }
    size_t remaining() const { return ok_ ? n_ - pos_ : 0; }
    void fail() { ok_ = false; }

private:
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// Replaces invalid UTF-8 with U+FFFD and strips NUL bytes.
std::string sanitize_utf8(std::string_view in);

std::string to_hex(const uint8_t* p, size_t n);
template <size_t N>
std::string to_hex(const std::array<uint8_t, N>& a) { return to_hex(a.data(), N); }
bool from_hex(std::string_view hex, uint8_t* out, size_t n);

}  // namespace arc
