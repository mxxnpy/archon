#pragma once
// Windows Core Audio (WASAPI) backend.
//
// Everything that touches COM runs on one worker thread (MTA). OS
// notifications only set "dirty" flags and wake the worker, which rebuilds the
// model and reports differences to the sink — never calling WASAPI from inside
// a notification (that can deadlock the audio service).

#include <windows.h>

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "archon/server.hpp"

namespace agent {

class WasapiBackend final : public arc::AudioBackend {
public:
    WasapiBackend();
    ~WasapiBackend() override;

    bool start(arc::AudioSink& sink, std::string* err) override;
    void stop() override;
    void set_endpoint_volume(const std::string& id, uint16_t volume) override;
    void set_endpoint_mute(const std::string& id, bool muted) override;
    void set_source_volume(const std::string& id, uint16_t volume) override;
    void set_source_mute(const std::string& id, bool muted) override;
    bool icon_png(const std::string& key, arc::Bytes& png) override;
    void set_metering(bool on) override;
    void set_source_route(const std::string& source_id, const std::string& endpoint_id) override;
    uint32_t features() const override;
    std::string os_name() const override { return "windows"; }

    // Called from COM notification threads.
    void mark_devices_dirty();
    void mark_sessions_dirty();

    struct State;  // COM objects, owned by the worker thread

private:
    void post(std::function<void()> fn);
    void run(std::promise<std::string>* started);
    void refresh_devices();
    void refresh_sessions();
    void publish();
    void sample_meters();

    arc::AudioSink* sink_ = nullptr;
    std::thread thread_;
    HANDLE wake_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> devices_dirty_{true};
    std::atomic<bool> sessions_dirty_{true};
    std::atomic<bool> metering_{false};
    std::atomic<bool> routing_ok_{false};
    bool meters_were_silent_ = true;

    std::mutex mu_;
    std::deque<std::function<void()>> tasks_;

    std::unique_ptr<State> st_;

    // Last state reported to the sink (worker thread only).
    std::map<std::string, arc::EndpointInfo> sent_endpoints_;
    std::map<std::string, arc::SourceInfo> sent_sources_;

    std::mutex icon_mu_;
    std::map<std::string, std::wstring> icon_paths_;  // key -> exe path
    std::map<std::string, arc::Bytes> icon_cache_;
};

}  // namespace agent
