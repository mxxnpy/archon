#include "archon/stream.hpp"

#include <vector>

#include "archon/crypto.hpp"
#include "archon/protocol.hpp"

namespace arc {

StreamReceiver::StreamReceiver(StreamListener& listener) : listener_(listener) {}

StreamReceiver::~StreamReceiver() { close(); }

uint16_t StreamReceiver::open(std::string* err) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        armed_ = true;
    }
    if (sock_ != net::kInvalidSocket) return port_;
    sock_ = net::bind_udp(0, &port_, err);
    if (sock_ == net::kInvalidSocket) return 0;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
    return port_;
}

void StreamReceiver::start(const Key32& key, const std::string& sender_host) {
    std::lock_guard<std::mutex> lk(mu_);
    key_ = key;
    sender_ = net::canonical_ip(sender_host);
    active_ = true;
    reorder_.reset();
    replay_.swap(early_);  // datagrams that arrived before the key: try them now
    early_.clear();
    waker_.wake();
}

void StreamReceiver::stop() {
    std::lock_guard<std::mutex> lk(mu_);
    active_ = false;
    armed_ = false;
    wipe(key_.data(), key_.size());
    reorder_.reset();
    early_.clear();
    replay_.clear();
}

void StreamReceiver::close() {
    stop();
    if (thread_.joinable()) {
        stop_ = true;
        waker_.wake();
        thread_.join();
    }
    if (sock_ != net::kInvalidSocket) {
        net::close_socket(sock_);
        sock_ = net::kInvalidSocket;
    }
    port_ = 0;
}

namespace {
struct Out {
    uint64_t seq;
    Bytes packet;
    bool lost;
};
}  // namespace

template <class Ready>
void StreamReceiver::accept_locked(const std::string& from, const uint8_t* d, size_t n, Ready& out) {
    const std::string src = net::canonical_ip(from);
    uint64_t seq = 0;
    uint32_t ts = 0;
    uint8_t codec = 0;
    Bytes packet;
    // Only the PC we streamed from, and only with this stream's key.
    if (src != sender_ || !open_audio(key_, d, n, seq, ts, codec, packet) || codec != kStreamCodecOpus) {
        ++rejected_;
        return;
    }
    ++received_;
    reorder_.push(seq, std::move(packet), [&out](uint64_t s, const Bytes& p, bool lost) { out.push_back({s, p, lost}); });
}

void StreamReceiver::run() {
    std::vector<uint8_t> buf(2048);
    while (!stop_) {
        std::vector<net::PollItem> items{{waker_.fd(), true, false}, {sock_, true, false}};
        net::poll(items, 500);
        if (items[0].readable) waker_.drain();
        std::vector<Out> ready;
        {
            // Datagrams kept from before the key arrived.
            std::lock_guard<std::mutex> lk(mu_);
            std::vector<Datagram> replay;
            replay.swap(replay_);
            if (active_)
                for (const Datagram& g : replay) accept_locked(g.from, g.data.data(), g.data.size(), ready);
        }
        for (const Out& o : ready) listener_.on_audio(o.packet.data(), o.packet.size(), o.seq, o.lost);
        if (!items[1].readable) continue;
        for (;;) {  // drain everything that arrived
            std::string from;
            const long n = net::recv_udp(sock_, buf.data(), buf.size(), &from);
            if (n <= 0) break;
            ready.clear();
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!active_) {
                    if (!armed_) {  // nobody is listening: nothing is kept
                        ++rejected_;
                        continue;
                    }
                    // No key yet: keep a few (the key is on its way over TCP).
                    if (early_.size() >= kMaxEarly) early_.erase(early_.begin());
                    early_.push_back(Datagram{from, Bytes(buf.data(), buf.data() + n)});
                    continue;
                }
                // The key may have just arrived while this loop was draining: the
                // datagrams kept before it go first, or the newer one would start
                // the sequence and they would be dropped as late.
                if (!replay_.empty()) {
                    std::vector<Datagram> replay;
                    replay.swap(replay_);
                    for (const Datagram& g : replay) accept_locked(g.from, g.data.data(), g.data.size(), ready);
                }
                accept_locked(from, buf.data(), size_t(n), ready);
            }
            // Outside the lock: the app may call stop() from its callback.
            for (const Out& o : ready) listener_.on_audio(o.packet.data(), o.packet.size(), o.seq, o.lost);
        }
    }
}

}  // namespace arc
