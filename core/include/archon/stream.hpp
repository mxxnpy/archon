#pragma once
// 1.3: receiving the PC's audio on the phone. Opens a UDP port, accepts only
// datagrams from the PC's address that authenticate with the stream key,
// reorders a little and hands Opus packets to the app in sequence order (the
// app decodes with the platform decoder and plays them).

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "archon/bytes.hpp"
#include "archon/net.hpp"

namespace arc {

class StreamListener {
public:
    virtual ~StreamListener() = default;
    // One Opus packet in order. `lost` = true with an empty packet means the
    // frame never arrived (decode it as a gap: packet-loss concealment).
    virtual void on_audio(const uint8_t* packet, size_t n, uint64_t seq, bool lost) = 0;
};

// Reordering core, separate from the socket so it is unit-testable.
class Reorder {
public:
    static constexpr size_t kWindow = 3;      // frames held to fix reordering (3 x 20 ms)
    static constexpr uint64_t kResync = 100;  // a jump this big restarts the sequence

    // Feeds one authenticated packet; calls out(seq, packet, lost) in order.
    template <class Out>
    void push(uint64_t seq, Bytes packet, Out&& out);
    void reset() {
        started_ = false;
        held_.clear();
    }

private:
    bool started_ = false;
    uint64_t next_ = 0;
    std::map<uint64_t, Bytes> held_;
};

class StreamReceiver {
public:
    explicit StreamReceiver(StreamListener& listener);
    ~StreamReceiver();
    StreamReceiver(const StreamReceiver&) = delete;
    StreamReceiver& operator=(const StreamReceiver&) = delete;

    // Binds a UDP port (once) and starts the reader thread. Returns the port, 0 on error.
    uint16_t open(std::string* err);
    // Starts accepting datagrams from `sender_host` (numeric) sealed with `key`.
    void start(const Key32& key, const std::string& sender_host);
    // Stops accepting (keeps the port for the next start).
    void stop();
    void close();
    uint16_t port() const { return port_; }

    // Counters (diagnostics and tests).
    uint64_t received() const { return received_; }
    uint64_t rejected() const { return rejected_; }

private:
    void run();

    StreamListener& listener_;
    net::socket_t sock_ = net::kInvalidSocket;
    uint16_t port_ = 0;
    net::Waker waker_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;  // guards key_, sender_, active_, reorder_
    Key32 key_{};
    std::string sender_;
    bool active_ = false;
    Reorder reorder_;
    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> rejected_{0};
};

// ---- template ---------------------------------------------------------------------

template <class Out>
void Reorder::push(uint64_t seq, Bytes packet, Out&& out) {
    if (!started_ || seq >= next_ + kResync || seq + kResync <= next_) {
        // First packet, or the sender restarted far away: start over here.
        started_ = true;
        held_.clear();
        next_ = seq;
    }
    if (seq < next_ || held_.count(seq)) return;  // late or duplicate
    held_.emplace(seq, std::move(packet));
    for (;;) {
        auto it = held_.find(next_);
        if (it != held_.end()) {
            out(next_, it->second, false);
            held_.erase(it);
            ++next_;
            continue;
        }
        // Missing frame: wait while the window is small, then call it lost.
        if (held_.size() <= kWindow) break;
        out(next_, Bytes{}, true);
        ++next_;
    }
}

}  // namespace arc
