#pragma once
// "Listen on the phone" sender: 48 kHz stereo float PCM in, Opus frames out,
// each sealed with the stream key and sent as one UDP datagram (protocol 1.3).
// Not thread-safe: one capture thread owns one streamer.

#include <cstdint>
#include <string>
#include <vector>

#include "archon/bytes.hpp"
#include "archon/net.hpp"

struct OpusEncoder;

namespace agent {

class OpusStreamer {
public:
    static constexpr int kRate = 48000;
    static constexpr int kChannels = 2;
    static constexpr int kFrame = 960;  // 20 ms at 48 kHz

    OpusStreamer(std::string host, uint16_t port, const arc::Key32& key);
    ~OpusStreamer();
    OpusStreamer(const OpusStreamer&) = delete;
    OpusStreamer& operator=(const OpusStreamer&) = delete;

    bool init(std::string* err);
    // Interleaved stereo float, 48 kHz. Every complete 20 ms frame is encoded and sent.
    void push(const float* interleaved, size_t frames);
    // Silence of `frames` length (nothing is playing on the PC): keeps the
    // phone's playback clock running without gaps.
    void push_silence(size_t frames);

    uint64_t sent() const { return seq_; }

private:
    void flush_frames();

    std::string host_;
    uint16_t port_;
    arc::Key32 key_;
    OpusEncoder* enc_ = nullptr;
    arc::net::socket_t sock_ = arc::net::kInvalidSocket;
    std::vector<float> pending_;  // interleaved samples waiting for a full frame
    uint64_t seq_ = 0;
    uint32_t timestamp_ = 0;      // in samples per channel
};

}  // namespace agent
