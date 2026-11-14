#pragma once
// Wire messages of the Archon protocol, version 1 (see docs/protocol.md).

#include <optional>
#include <string>
#include <vector>

#include "archon/bytes.hpp"

namespace arc {

constexpr uint16_t kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 47321;
constexpr size_t kMaxFrame = 65536;
constexpr uint16_t kMaxVolume = 10000;
constexpr const char* kServiceType = "_archon._tcp";

// Field limits (bytes).
constexpr size_t kMaxDeviceName = 64;
constexpr size_t kMaxErrorText = 256;
constexpr size_t kMaxId = 512;
constexpr size_t kMaxDisplayName = 256;
constexpr size_t kMaxIconKey = 128;
constexpr size_t kMaxIconBytes = 48 * 1024;

enum class MsgType : uint8_t {
    Hello = 0x01,
    HelloAck = 0x02,
    PairResponse = 0x03,
    PairConfirm = 0x04,
    ResumeConfirm = 0x05,
    Error = 0x0F,

    GetState = 0x20,
    SetEndpointVolume = 0x21,
    SetEndpointMute = 0x22,
    SetSourceVolume = 0x23,
    SetSourceMute = 0x24,
    GetIcon = 0x25,
    Ping = 0x26,
    SetMeters = 0x27,
    SetSourceRoute = 0x28,  // 1.2
    PanicMute = 0x29,       // 1.2

    StateReset = 0x40,
    Endpoint = 0x41,
    EndpointRemoved = 0x42,
    Source = 0x43,
    SourceRemoved = 0x44,
    StateSynced = 0x45,
    Icon = 0x46,
    Pong = 0x47,
    Meters = 0x48,
    AgentInfo = 0x49,  // 1.2
};

enum class Mode : uint8_t { Pair = 1, Resume = 2 };

enum class ErrorCode : uint16_t {
    None = 0,
    Protocol = 1,
    UnsupportedVersion = 2,
    UnknownController = 3,
    WrongCode = 4,
    PairingLocked = 5,
    PairingRejected = 6,
    Busy = 7,
    AuthFailed = 8,
    Timeout = 9,
    Revoked = 10,
    // Local-only codes (never on the wire).
    ConnectFailed = 100,
    ConnectionLost = 101,
    // The PC refused this phone's pairing in a *plaintext* (unauthenticated)
    // handshake error (wire 3/8/10). Could be a real deleted pairing or a
    // spoofer on the LAN: never delete the pairing automatically; ask the user.
    NotRecognized = 102,
    // The peer at this address is not the paired PC (wrong id or bad proof):
    // another device, a stale IP or a spoofer. Try another address.
    WrongAgent = 103,
};

const char* error_code_name(ErrorCode c);

// Device names travel in HELLO/HELLO_ACK and are bound into the handshake
// transcripts, so both sides must normalize them identically: UTF-8
// sanitized, control and bidi-override characters removed, ≤ 64 bytes.
std::string clean_device_name(std::string_view name);

// ---- Audio model ----------------------------------------------------------

struct EndpointInfo {
    std::string id;
    std::string name;
    uint16_t volume = 0;
    bool muted = false;
    bool is_default = false;        // default output (outputs only)
    uint16_t meter = 0;  // level-meter slot assigned by the server (0 = none)
    bool input = false;             // microphone / capture device (1.2)
    bool is_default_input = false;  // the default microphone (1.2)
    bool operator==(const EndpointInfo& o) const {
        return id == o.id && name == o.name && volume == o.volume && muted == o.muted &&
               is_default == o.is_default && meter == o.meter && input == o.input &&
               is_default_input == o.is_default_input;
    }
    bool operator!=(const EndpointInfo& o) const { return !(*this == o); }
};

struct SourceInfo {
    std::string id;
    std::string endpoint_id;
    std::string name;
    std::string icon_key;
    uint32_t pid = 0;
    uint16_t volume = 0;
    bool muted = false;
    bool playing = false;
    bool system_sounds = false;
    uint16_t meter = 0;  // level-meter slot assigned by the server (0 = none)
    std::string route;   // output this app is pinned to; empty = follows the default (1.2)
    bool operator==(const SourceInfo& o) const {
        return id == o.id && endpoint_id == o.endpoint_id && name == o.name &&
               icon_key == o.icon_key && pid == o.pid && volume == o.volume && muted == o.muted &&
               playing == o.playing && system_sounds == o.system_sounds && meter == o.meter &&
               route == o.route;
    }
    bool operator!=(const SourceInfo& o) const { return !(*this == o); }
};

// ---- Handshake messages -----------------------------------------------------

struct Hello {
    uint16_t version = kProtocolVersion;
    Mode mode = Mode::Pair;
    Id16 controller_id{};
    std::string name;
    Key32 nonce{};  // resume only
};

struct HelloAck {
    uint16_t version = kProtocolVersion;
    Id16 agent_id{};
    std::string name;
    Mode mode = Mode::Pair;
    Id16 sid{};      // pair
    Key32 y{};       // pair: Ys
    Key32 nonce{};   // resume: nonce_s
    Key32 tag{};     // resume: tag_s
};

struct ErrorMsg {
    ErrorCode code = ErrorCode::Protocol;
    std::string message;
};

Bytes encode_hello(const Hello& m);
Bytes encode_hello_ack(const HelloAck& m);
Bytes encode_pair_response(const Key32& y, const Key32& tag);
Bytes encode_pair_confirm(const Key32& tag);
Bytes encode_resume_confirm(const Key32& tag);
Bytes encode_error(const ErrorMsg& m);

// Decoders take the message *body* (after the type byte) and return false on
// any malformation, including trailing bytes.
bool decode_hello(Reader& r, Hello& m);
bool decode_hello_ack(Reader& r, HelloAck& m);
bool decode_two_keys(Reader& r, Key32& a, Key32& b);
bool decode_key(Reader& r, Key32& k);
bool decode_error(Reader& r, ErrorMsg& m);

// ---- Application messages ---------------------------------------------------

Bytes encode_simple(MsgType t);  // GetState, StateReset, StateSynced
Bytes encode_set_volume(MsgType t, const std::string& id, uint16_t volume);
Bytes encode_set_mute(MsgType t, const std::string& id, bool muted);
Bytes encode_id(MsgType t, const std::string& id);  // GetIcon, *Removed
Bytes encode_seq(MsgType t, uint32_t seq);          // Ping, Pong
Bytes encode_endpoint(const EndpointInfo& e);
Bytes encode_source(const SourceInfo& s);
Bytes encode_icon(const std::string& key, const uint8_t* png, size_t n);
Bytes encode_flag(MsgType t, bool on);  // SetMeters, PanicMute

// Feature bits announced in AGENT_INFO (1.2).
enum AgentFeature : uint32_t {
    kFeatureInputs = 1u << 0,
    kFeatureRouting = 1u << 1,
    kFeaturePanic = 1u << 2,
};
constexpr size_t kMaxOsName = 32;
struct AgentInfo {
    uint32_t features = 0;
    bool panic = false;
    std::string os;
    bool operator==(const AgentInfo& o) const { return features == o.features && panic == o.panic && os == o.os; }
    bool operator!=(const AgentInfo& o) const { return !(*this == o); }
};
Bytes encode_agent_info(const AgentInfo& a);
Bytes encode_set_route(const std::string& source_id, const std::string& endpoint_id);

struct MeterLevel {
    uint16_t slot = 0;
    uint8_t level = 0;  // peak, 0..255
};
constexpr size_t kMaxMetersPerMessage = 255;
Bytes encode_meters(const std::vector<MeterLevel>& levels);

bool decode_set_volume(Reader& r, std::string& id, uint16_t& volume);
bool decode_set_mute(Reader& r, std::string& id, bool& muted);
bool decode_id(Reader& r, std::string& id, size_t max_len);
bool decode_seq(Reader& r, uint32_t& seq);
bool decode_endpoint(Reader& r, EndpointInfo& e);
bool decode_source(Reader& r, SourceInfo& s);
bool decode_icon(Reader& r, std::string& key, Bytes& png);
bool decode_flag(Reader& r, bool& on);
bool decode_meters(Reader& r, std::vector<MeterLevel>& levels);
bool decode_agent_info(Reader& r, AgentInfo& a);
bool decode_set_route(Reader& r, std::string& source_id, std::string& endpoint_id);

// ---- Pairing record (stored by controllers) ----------------------------------

struct PairingRecord {
    Id16 agent_id{};
    std::string agent_name;
    Key32 pair_key{};
};

Bytes serialize_pairing(const PairingRecord& p);
std::optional<PairingRecord> deserialize_pairing(const uint8_t* p, size_t n);

}  // namespace arc
