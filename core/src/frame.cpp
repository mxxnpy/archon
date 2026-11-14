#include "archon/frame.hpp"

#include "archon/protocol.hpp"

namespace arc {

void append_frame(Bytes& out, const Bytes& payload) {
    const uint32_t n = uint32_t(payload.size());
    out.push_back(uint8_t(n >> 24));
    out.push_back(uint8_t(n >> 16));
    out.push_back(uint8_t(n >> 8));
    out.push_back(uint8_t(n));
    out.insert(out.end(), payload.begin(), payload.end());
}

Bytes make_frame(const Bytes& payload) {
    Bytes out;
    out.reserve(payload.size() + 4);
    append_frame(out, payload);
    return out;
}

void FrameAssembler::feed(const uint8_t* p, size_t n) {
    // Compact consumed bytes before growing so the buffer stays bounded.
    if (pos_ > 0 && (pos_ == buf_.size() || pos_ > 4096)) {
        buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(pos_));
        pos_ = 0;
    }
    buf_.insert(buf_.end(), p, p + n);
}

FrameAssembler::Result FrameAssembler::next(Bytes& out) {
    const size_t avail = buf_.size() - pos_;
    if (avail < 4) return Result::NeedMore;
    const uint8_t* h = buf_.data() + pos_;
    const uint32_t len = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
    if (len == 0 || len > kMaxFrame) return Result::Error;
    if (avail < 4 + size_t(len)) return Result::NeedMore;
    out.assign(h + 4, h + 4 + len);
    pos_ += 4 + len;
    return Result::Frame;
}

void FrameAssembler::reset() {
    buf_.clear();
    pos_ = 0;
}

}  // namespace arc
