#include "archon/stream.hpp"

#include <vector>

#include "archon/crypto.hpp"
#include "archon/protocol.hpp"

namespace arc {

StreamReceiver::StreamReceiver(StreamListener& listener) : listener_(listener) {}

StreamReceiver::~StreamReceiver() { close(); }

uint16_t StreamReceiver::open(std::string* err) {
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
    sender_ = net::unmap_ipv4(sender_host);
    const size_t pct = sender_.find('%');
    if (pct != std::string::npos) sender_.resize(pct);
    active_ = true;
    reorder_.reset();
}

void StreamReceiver::stop() {
    std::lock_guard<std::mutex> lk(mu_);
    active_ = false;
    wipe(key_.data(), key_.size());
    reorder_.reset();
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

void StreamReceiver::run() {
    std::vector<uint8_t> buf(2048);
    while (!stop_) {
        std::vector<net::PollItem> items{{waker_.fd(), true, false}, {sock_, true, false}};
        net::poll(items, 500);
        if (items[0].readable) waker_.drain();
        if (!items[1].readable) continue;
        for (;;) {  // drain everything that arrived
            std::string from;
            const long n = net::recv_udp(sock_, buf.data(), buf.size(), &from);
            if (n <= 0) break;
            struct Out {
                uint64_t seq;
                Bytes packet;
                bool lost;
            };
            std::vector<Out> ready;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!active_) continue;
                std::string src = from;
                const size_t pct = src.find('%');
                if (pct != std::string::npos) src.resize(pct);
                uint64_t seq = 0;
                uint32_t ts = 0;
                uint8_t codec = 0;
                Bytes packet;
                // Only the PC we streamed from, and only with this stream's key.
                if (src != sender_ || !open_audio(key_, buf.data(), size_t(n), seq, ts, codec, packet) ||
                    codec != kStreamCodecOpus) {
                    ++rejected_;
                    continue;
                }
                ++received_;
                reorder_.push(seq, std::move(packet),
                              [&ready](uint64_t s, const Bytes& p, bool lost) { ready.push_back({s, p, lost}); });
            }
            // Outside the lock: the app may call stop() from its callback.
            for (const Out& o : ready) listener_.on_audio(o.packet.data(), o.packet.size(), o.seq, o.lost);
        }
    }
}

}  // namespace arc
