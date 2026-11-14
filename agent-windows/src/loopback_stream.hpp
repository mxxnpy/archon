#pragma once
// "Listen on the phone" on Windows: WASAPI loopback capture of an output
// device (its final mix, even while that output is muted), converted by
// Windows to 48 kHz stereo float, encoded with Opus and sent to the phone.

#include <windows.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "archon/server.hpp"

namespace agent {

class LoopbackStreams final : public arc::StreamService {
public:
    LoopbackStreams() = default;
    ~LoopbackStreams() override;
    // The server to tell when a stream ends on its own (output gone).
    // nullptr before the server goes away.
    void attach(arc::Server* server) { server_ = server; }

    bool start(uint64_t id, const std::string& endpoint_id, const std::string& dest_host, uint16_t dest_port,
               const arc::Key32& key, std::string* err) override;
    void stop(uint64_t id) override;

private:
    struct Stream {
        std::thread thread;
        std::atomic<bool> stop{false};
        arc::Key32 key{};  // read once by the capture thread, then wiped
    };
    void run(uint64_t id, Stream* st, std::wstring endpoint, std::string host, uint16_t port);
    void capture(Stream* st, const std::wstring& endpoint, const std::string& host, uint16_t port,
                 arc::Key32& key, bool& failed);

    std::atomic<arc::Server*> server_{nullptr};
    std::mutex mu_;
    std::map<uint64_t, std::unique_ptr<Stream>> streams_;
};

}  // namespace agent
