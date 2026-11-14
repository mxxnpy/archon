#pragma once
// u32 length-prefixed framing over a byte stream (docs/protocol.md §2).

#include "archon/bytes.hpp"

namespace arc {

Bytes make_frame(const Bytes& payload);
void append_frame(Bytes& out, const Bytes& payload);

class FrameAssembler {
public:
    enum class Result { NeedMore, Frame, Error };

    void feed(const uint8_t* p, size_t n);
    // Extracts the next complete frame payload into `out`.
    Result next(Bytes& out);
    void reset();

private:
    Bytes buf_;
    size_t pos_ = 0;
};

}  // namespace arc
