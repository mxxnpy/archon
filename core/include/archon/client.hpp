#pragma once
// Socket client for controllers: runs ControllerSession on its own thread,
// keeps the link alive (PING), reconnects with backoff and coalesces rapid
// volume commands (slider drags) so only the latest value is sent.

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "archon/controller_session.hpp"
#include "archon/stream.hpp"
#include "archon/net.hpp"

namespace arc {

enum class ClientStatus {
    Idle = 0,
    Connecting = 1,
    AwaitingCode = 2,   // pairing: user must type the code shown on the PC
    Verifying = 3,      // pairing: code sent, waiting for the PC
    Connected = 4,
    Reconnecting = 5,
    Failed = 6,         // terminal; see the error code
};

const char* client_status_name(ClientStatus s);

// Callbacks run on the client thread; apps must marshal to their UI thread.
class ClientListener {
public:
    virtual ~ClientListener() = default;
    virtual void on_status(ClientStatus status, ErrorCode error, const std::string& message) = 0;
    virtual void on_event(const ControllerEvent& event) = 0;
    // 1.3: one Opus packet of the PC's audio, in order (network thread).
    // lost = true, empty packet: that frame never arrived (conceal it).
    virtual void on_audio(const uint8_t* packet, size_t n, uint64_t seq, bool lost) {
        (void)packet;
        (void)n;
        (void)seq;
        (void)lost;
    }
};

struct ClientTiming {
    int connect_timeout_ms = 5000;
    int handshake_timeout_ms = 15000;
    int code_timeout_ms = 130000;
    int ping_interval_ms = 5000;
    int idle_timeout_ms = 15000;
    int backoff_min_ms = 500;
    int backoff_max_ms = 10000;
    int command_interval_ms = 16;  // ≤ 60 command batches per second
};

class Client {
public:
    Client(const Id16& controller_id, std::string controller_name, ClientListener& listener,
           ClientTiming timing = {});
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Each call stops any previous connection first.
    void pair(const std::string& host, uint16_t port);
    void resume(const std::string& host, uint16_t port, const PairingRecord& record);
    // Tries the addresses in turn (e.g. freshly discovered + last known). On
    // Connected, the status message is "host|port" of the address that worked.
    void resume(const std::vector<std::pair<std::string, uint16_t>>& addresses, const PairingRecord& record);
    void disconnect();

    void submit_code(const std::string& code);

    void set_endpoint_volume(const std::string& id, uint16_t volume);
    void set_endpoint_mute(const std::string& id, bool muted);
    void set_source_volume(const std::string& id, uint16_t volume);
    void set_source_mute(const std::string& id, bool muted);
    // 1.2: send an app's audio to an output (empty endpoint = system default).
    void set_source_route(const std::string& source_id, const std::string& endpoint_id);
    // 1.2: mute everything / undo. Unlike other commands it is not dropped
    // while offline: the last request is sent once, as soon as the secure
    // channel is up (a shortcut calls resume() then panic_mute()).
    void panic_mute(bool on);
    // 1.3: listen to the PC on this phone. Needs a live link; returns false
    // otherwise or if no UDP port could be opened. flags: kStreamFlagMutePc.
    bool stream_start(uint8_t flags, const std::string& endpoint_id = {});
    void stream_stop();
    void request_icon(const std::string& icon_key);
    void refresh();
    // Subscribe to live level meters (kept across reconnects).
    void set_meters(bool on);

private:
    struct Cmd {
        MsgType type;
        std::string id;
        Bytes msg;
    };
    void start(std::vector<std::pair<std::string, uint16_t>> addrs, Mode mode, const PairingRecord* record);
    void enqueue(MsgType type, const std::string& id, Bytes msg, bool coalesce);
    void run();
    // Runs one connection. Returns true if the caller should retry.
    bool run_connection(bool& ever_ready);
    void status(ClientStatus s, ErrorCode e = ErrorCode::None, const std::string& m = {});
    bool wait_interruptible(int ms);

    Id16 controller_id_;
    std::string controller_name_;
    ClientListener& listener_;
    ClientTiming timing_;
    net::Waker waker_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> meters_{false};
    std::atomic<bool> live_{false};  // secure channel up: commands are accepted only then
    std::optional<bool> pending_panic_;  // guarded by mu_: panic requested before the link was up
    std::chrono::steady_clock::time_point pending_panic_at_{};  // guarded by mu_

    // 1.3: audio receiver (opened on first stream_start, closed in the destructor).
    struct AudioRelay : StreamListener {
        ClientListener* target = nullptr;
        void on_audio(const uint8_t* p, size_t n, uint64_t seq, bool lost) override {
            if (target) target->on_audio(p, n, seq, lost);
        }
    };
    AudioRelay relay_;
    std::unique_ptr<StreamReceiver> receiver_;
    std::mutex receiver_mu_;

    // Connection parameters (owned by the worker while it runs).
    std::vector<std::pair<std::string, uint16_t>> addrs_;
    size_t attempt_ = 0;
    std::vector<ErrorCode> refused_;  // per address: unverified refusal since the last success (None = not refused)
    Mode mode_ = Mode::Pair;
    PairingRecord record_;

    std::mutex mu_;
    std::deque<Cmd> cmds_;
    std::string pending_code_;
};

}  // namespace arc
