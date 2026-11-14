#pragma once
// Socket client for controllers: runs ControllerSession on its own thread,
// keeps the link alive (PING), reconnects with backoff and coalesces rapid
// volume commands (slider drags) so only the latest value is sent.

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

#include "archon/controller_session.hpp"
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
