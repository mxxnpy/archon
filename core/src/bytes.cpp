#include "archon/bytes.hpp"

namespace arc {

Writer& Writer::u8(uint8_t v) {
    buf_.push_back(v);
    return *this;
}

Writer& Writer::u16(uint16_t v) {
    buf_.push_back(static_cast<uint8_t>(v >> 8));
    buf_.push_back(static_cast<uint8_t>(v));
    return *this;
}

Writer& Writer::u32(uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) buf_.push_back(static_cast<uint8_t>(v >> s));
    return *this;
}

Writer& Writer::u64(uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) buf_.push_back(static_cast<uint8_t>(v >> s));
    return *this;
}

Writer& Writer::raw(const uint8_t* p, size_t n) {
    buf_.insert(buf_.end(), p, p + n);
    return *this;
}

Writer& Writer::str(std::string_view s, size_t max_len) {
    if (max_len > 65535) max_len = 65535;
    size_t n = s.size();
    if (n > max_len) {
        n = max_len;
        // Back off to a UTF-8 code point boundary (never split a sequence).
        while (n > 0 && n < s.size() && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
    }
    u16(static_cast<uint16_t>(n));
    return raw(reinterpret_cast<const uint8_t*>(s.data()), n);
}

const uint8_t* Reader::take(size_t n) {
    if (!ok_ || n > n_ - pos_) {
        ok_ = false;
        return nullptr;
    }
    const uint8_t* r = p_ + pos_;
    pos_ += n;
    return r;
}

uint8_t Reader::u8() {
    const uint8_t* p = take(1);
    return p ? p[0] : 0;
}

uint16_t Reader::u16() {
    const uint8_t* p = take(2);
    return p ? static_cast<uint16_t>((p[0] << 8) | p[1]) : 0;
}

uint32_t Reader::u32() {
    const uint8_t* p = take(4);
    if (!p) return 0;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t Reader::u64() {
    const uint8_t* p = take(8);
    if (!p) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

std::string Reader::str(size_t max_len) {
    uint16_t n = u16();
    if (!ok_) return {};
    if (n > max_len) {
        ok_ = false;
        return {};
    }
    const uint8_t* p = take(n);
    if (!p) return {};
    return sanitize_utf8(std::string_view(reinterpret_cast<const char*>(p), n));
}

std::string sanitize_utf8(std::string_view in) {
    static const char kReplacement[] = "\xEF\xBF\xBD";
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    const size_t n = in.size();
    while (i < n) {
        const uint8_t c = static_cast<uint8_t>(in[i]);
        if (c == 0) {
            ++i;
            continue;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        size_t len = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        }
        bool valid = len != 0 && i + len <= n;
        for (size_t k = 1; valid && k < len; ++k) {
            const uint8_t cc = static_cast<uint8_t>(in[i + k]);
            if ((cc & 0xC0) != 0x80) valid = false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (valid) {
            // Reject overlong encodings, surrogates and out-of-range values.
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
                cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                valid = false;
        }
        if (valid) {
            out.append(in.data() + i, len);
            i += len;
        } else {
            out.append(kReplacement);
            ++i;
        }
    }
    return out;
}

std::string to_hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool from_hex(std::string_view hex, uint8_t* out, size_t n) {
    if (hex.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace arc
