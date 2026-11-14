#pragma once
// Controller-side protocol state machine with no I/O ("sans-IO"): feed it
// received frame payloads, send the payloads it returns. The socket client,
// the tests and a future ESP32 firmware all drive this same class.

#include <memory>
#include <vector>

#include "archon/crypto.hpp"
#include "archon/protocol.hpp"

namespace arc {

struct ControllerEvent {
    enum class Kind {
        CodeRequired,    // pairing: ask the user for the code shown on the PC
        Paired,          // pairing succeeded: persist `pairing`
        Ready,           // secure channel established (pair or resume)
        StateReset,
        Endpoint,
        EndpointRemoved,
        Source,
        SourceRemoved,
        StateSynced,
        Icon,
        Pong,
        Meters,          // levels for meter slots (see EndpointInfo/SourceInfo::meter)
        Error,           // agent reported an error; connection will close
    };
    ControllerEvent(Kind k = Kind::Error) : kind(k) {}  // NOLINT: implicit for brace-init
    Kind kind;
    EndpointInfo endpoint;
    SourceInfo source;
    std::string id;  // removed id / icon key
    Bytes data;      // icon PNG
    uint32_t seq = 0;
    ErrorCode error = ErrorCode::None;
    std::string message;
    PairingRecord pairing;
    std::vector<MeterLevel> meters;
};

class ControllerSession {
public:
    enum class State { Idle, AwaitHelloAck, AwaitCode, AwaitPairConfirm, Ready, Closed };

    ControllerSession(const Id16& controller_id, std::string controller_name);

    // Return the first frame payload to send.
    Bytes begin_pair();
    Bytes begin_resume(const PairingRecord& record);

    // Pairing: after CodeRequired. Returns the payload to send, or empty if the
    // code is malformed or the session is not waiting for one.
    Bytes submit_code(const std::string& code);

    struct Output {
        std::vector<Bytes> send;
        std::vector<ControllerEvent> events;
        bool close = false;           // tear down the connection
        ErrorCode close_reason = ErrorCode::None;
    };
    void on_frame(const uint8_t* p, size_t n, Output& out);

    // Application messages (encrypted). Empty if not Ready.
    Bytes app(const Bytes& plain_message);

    State state() const { return state_; }
    const Id16& agent_id() const { return agent_id_; }
    const std::string& agent_name() const { return agent_name_; }

private:
    void fail(Output& out, ErrorCode reason);
    void handle_plain(const uint8_t* p, size_t n, Output& out);
    void handle_app(const Bytes& msg, Output& out);

    Id16 controller_id_;
    std::string controller_name_;
    State state_ = State::Idle;
    Mode mode_ = Mode::Pair;

    Id16 agent_id_{};
    std::string agent_name_;
    Id16 expected_agent_{};
    Key32 pair_key_{};
    Key32 nonce_c_{};

    // Pairing scratch
    Id16 sid_{};
    Key32 ys_{};
    std::unique_ptr<SessionKeys> pending_;

    SecureChannel channel_;
};

}  // namespace arc
