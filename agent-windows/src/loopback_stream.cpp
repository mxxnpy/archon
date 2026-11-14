#include "loopback_stream.hpp"

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <chrono>
#include <vector>

#include "archon/crypto.hpp"
#include "opus_streamer.hpp"
#include "win_util.hpp"

using Microsoft::WRL::ComPtr;

namespace agent {

LoopbackStreams::~LoopbackStreams() {
    std::map<uint64_t, std::unique_ptr<Stream>> all;
    {
        std::lock_guard<std::mutex> lk(mu_);
        all.swap(streams_);
    }
    for (auto& [id, st] : all) {
        st->stop = true;
        if (st->thread.joinable()) st->thread.join();
    }
}

bool LoopbackStreams::start(uint64_t id, const std::string& endpoint_id, const std::string& dest_host,
                            uint16_t dest_port, const arc::Key32& key, std::string* err) {
    if (endpoint_id.empty() || dest_port == 0) {
        if (err) *err = "no output to capture";
        return false;
    }
    stop(id);  // a restart replaces the previous stream of this phone
    auto st = std::make_unique<Stream>();
    Stream* raw = st.get();
    raw->key = key;  // not a thread argument: std::thread would keep an unwipeable copy
    raw->thread = std::thread(&LoopbackStreams::run, this, id, raw, to_wide(endpoint_id), dest_host, dest_port);
    std::lock_guard<std::mutex> lk(mu_);
    streams_[id] = std::move(st);
    return true;  // capture errors are reported later through Server::stream_failed
}

void LoopbackStreams::stop(uint64_t id) {
    std::unique_ptr<Stream> st;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = streams_.find(id);
        if (it == streams_.end()) return;
        st = std::move(it->second);
        streams_.erase(it);
    }
    st->stop = true;
    if (st->thread.joinable()) st->thread.join();
}

void LoopbackStreams::capture(Stream* st, const std::wstring& endpoint, const std::string& host, uint16_t port,
                              arc::Key32& key, bool& failed) {
    {
        OpusStreamer enc(host, port, key);
        arc::wipe(key.data(), key.size());
        std::string err;
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> cap;
        // Ask Windows for exactly what Opus wants: 48 kHz, stereo, float.
        WAVEFORMATEXTENSIBLE fmt{};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = 2;
        fmt.Format.nSamplesPerSec = 48000;
        fmt.Format.wBitsPerSample = 32;
        fmt.Format.nBlockAlign = 8;
        fmt.Format.nAvgBytesPerSec = 48000 * 8;
        fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 32;
        fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        const bool ok = enc.init(&err) &&
                        SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
                        SUCCEEDED(en->GetDevice(endpoint.c_str(), &dev)) &&
                        SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) &&
                        SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000 /* 200 ms */, 0,
                                                     &fmt.Format, nullptr)) &&
                        SUCCEEDED(client->GetService(IID_PPV_ARGS(&cap))) && SUCCEEDED(client->Start());
        if (!ok) {
            log_line("listen on the phone: capture could not start" + (err.empty() ? "" : ": " + err));
            failed = true;
        }
        using Clock = std::chrono::steady_clock;
        Clock::time_point fed = Clock::now();  // audio (or silence) is accounted up to here
        while (!failed && !st->stop) {
            Sleep(10);
            UINT32 packet = 0;
            bool got = false;
            HRESULT hr = S_OK;
            while (SUCCEEDED(hr = cap->GetNextPacketSize(&packet)) && packet) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD bflags = 0;
                if (FAILED(hr = cap->GetBuffer(&data, &frames, &bflags, nullptr, nullptr))) break;
                if (bflags & AUDCLNT_BUFFERFLAGS_SILENT) enc.push_silence(frames);
                else enc.push(reinterpret_cast<const float*>(data), frames);
                cap->ReleaseBuffer(frames);
                got = true;
            }
            if (FAILED(hr)) {  // AUDCLNT_E_DEVICE_INVALIDATED: the output went away
                log_line("listen on the phone: the captured output went away");
                failed = true;
                break;
            }
            const Clock::time_point now = Clock::now();
            if (got) {
                fed = now;
            } else if (now - fed >= std::chrono::milliseconds(20)) {
                // Nothing is playing (Windows sends no loopback data then): send
                // silence so the phone's playback clock keeps running smoothly.
                // After a long stall (sleep, a suspended VM) only the last
                // 200 ms count: no burst of old silence, no huge allocation.
                if (now - fed > std::chrono::milliseconds(200)) fed = now - std::chrono::milliseconds(200);
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - fed).count();
                const size_t frames = size_t(ms / 20) * size_t(OpusStreamer::kFrame);
                enc.push_silence(frames);
                fed += std::chrono::milliseconds((ms / 20) * 20);
            }
        }
        if (client) client->Stop();
    }
}

void LoopbackStreams::run(uint64_t id, Stream* st, std::wstring endpoint, std::string host, uint16_t port) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool failed = false;
    try {
        capture(st, endpoint, host, port, st->key, failed);
    } catch (const std::exception& e) {  // e.g. bad_alloc: end this stream, not the agent
        log_line(std::string("listen on the phone: capture stopped: ") + e.what());
        failed = true;
    }
    arc::wipe(st->key.data(), st->key.size());
    CoUninitialize();
    arc::Server* server = server_.load();
    if (failed && !st->stop && server) server->stream_failed(id, arc::ErrorCode::ConnectionLost);
}

}  // namespace agent
