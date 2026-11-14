#include "opus_streamer.hpp"

#include <opus.h>

#include <algorithm>

#include "archon/crypto.hpp"
#include "archon/protocol.hpp"

namespace agent {

OpusStreamer::OpusStreamer(std::string host, uint16_t port, const arc::Key32& key)
    : host_(std::move(host)), port_(port), key_(key) {}

OpusStreamer::~OpusStreamer() {
    if (enc_) opus_encoder_destroy(enc_);
    if (sock_ != arc::net::kInvalidSocket) arc::net::close_socket(sock_);
    arc::wipe(key_.data(), key_.size());
}

bool OpusStreamer::init(std::string* err) {
    int e = 0;
    // Restricted low-delay: no speech/music switching delay, ~26.5 ms algorithmic latency.
    enc_ = opus_encoder_create(kRate, kChannels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &e);
    if (e != OPUS_OK || !enc_) {
        if (err) *err = std::string("opus encoder: ") + opus_strerror(e);
        enc_ = nullptr;
        return false;
    }
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(128000));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(8));
    // Restricted low-delay is CELT-only (in-band FEC is a SILK feature): a lost
    // frame is concealed by the phone's decoder (PLC), reported by the receiver.
    sock_ = arc::net::bind_udp(0, nullptr, err);
    return sock_ != arc::net::kInvalidSocket;
}

void OpusStreamer::push(const float* interleaved, size_t frames) {
    pending_.insert(pending_.end(), interleaved, interleaved + frames * kChannels);
    flush_frames();
}

void OpusStreamer::push_silence(size_t frames) {
    pending_.insert(pending_.end(), frames * kChannels, 0.0f);
    flush_frames();
}

void OpusStreamer::flush_frames() {
    const size_t per_frame = size_t(kFrame) * kChannels;
    size_t at = 0;
    unsigned char packet[arc::kMaxAudioPayload];
    while (enc_ && pending_.size() - at >= per_frame) {
        const opus_int32 n = opus_encode_float(enc_, pending_.data() + at, kFrame, packet, int(sizeof packet));
        at += per_frame;
        if (n <= 0) continue;  // encoder error: skip the frame (the phone conceals it)
        const arc::Bytes d = arc::seal_audio(key_, seq_, timestamp_, arc::kStreamCodecOpus, packet, size_t(n));
        arc::net::send_udp(sock_, host_, port_, d.data(), d.size());
        ++seq_;
        timestamp_ += kFrame;
    }
    pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(at));
}

}  // namespace agent
