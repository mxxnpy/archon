#include "archon/protocol.hpp"

#include <algorithm>
#include <cstring>

namespace arc {

static const uint8_t kMagic[4] = {'A', 'R', 'C', 'N'};

const char* error_code_name(ErrorCode c) {
    switch (c) {
        case ErrorCode::None: return "none";
        case ErrorCode::Protocol: return "protocol_error";
        case ErrorCode::UnsupportedVersion: return "unsupported_version";
        case ErrorCode::UnknownController: return "unknown_controller";
        case ErrorCode::WrongCode: return "wrong_code";
        case ErrorCode::PairingLocked: return "pairing_locked";
        case ErrorCode::PairingRejected: return "pairing_rejected";
        case ErrorCode::Busy: return "busy";
        case ErrorCode::AuthFailed: return "auth_failed";
        case ErrorCode::Timeout: return "timeout";
        case ErrorCode::Revoked: return "revoked";
        case ErrorCode::ConnectFailed: return "connect_failed";
        case ErrorCode::ConnectionLost: return "connection_lost";
        case ErrorCode::NotRecognized: return "not_recognized";
        case ErrorCode::WrongAgent: return "wrong_agent";
    }
    return "unknown";
}

std::string clean_device_name(std::string_view name) {
    const std::string utf8 = sanitize_utf8(name);
    std::string out;
    size_t i = 0;
    while (i < utf8.size()) {
        const uint8_t c = uint8_t(utf8[i]);
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        uint32_t cp = c < 0x80 ? c : c & (0x3F >> (len - 1));
        for (size_t k = 1; k < len && i + k < utf8.size(); ++k) cp = (cp << 6) | (uint8_t(utf8[i + k]) & 0x3F);
        const bool control = cp < 0x20 || (cp >= 0x7F && cp < 0xA0);
        const bool bidi = cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) ||
                          (cp >= 0x2066 && cp <= 0x2069) || cp == 0x061C;
        if (!control && !bidi) {
            if (out.size() + len > kMaxDeviceName) break;  // never split a code point
            out.append(utf8, i, len);
        }
        i += len;
    }
    return out;
}

static uint16_t clamp_volume(uint16_t v) { return std::min<uint16_t>(v, kMaxVolume); }

Bytes encode_hello(const Hello& m) {
    Writer w;
    w.u8(uint8_t(MsgType::Hello)).raw(kMagic, 4).u16(m.version).u8(uint8_t(m.mode));
    w.arr(m.controller_id).str(m.name, kMaxDeviceName);
    if (m.mode == Mode::Resume) w.arr(m.nonce);
    return w.take();
}

bool decode_hello(Reader& r, Hello& m) {
    const uint8_t* magic = r.take(4);
    if (!magic || std::memcmp(magic, kMagic, 4) != 0) return false;
    m.version = r.u16();
    uint8_t mode = r.u8();
    if (!r.ok()) return false;
    if (m.version != kProtocolVersion) {
        // Body layout of other versions is unknown; caller reports version error.
        m.mode = Mode::Pair;
        return true;
    }
    if (mode != uint8_t(Mode::Pair) && mode != uint8_t(Mode::Resume)) return false;
    m.mode = Mode(mode);
    m.controller_id = r.arr<16>();
    m.name = clean_device_name(r.str(kMaxDeviceName));
    if (m.mode == Mode::Resume) m.nonce = r.arr<32>();
    return r.at_end();
}

Bytes encode_hello_ack(const HelloAck& m) {
    Writer w;
    w.u8(uint8_t(MsgType::HelloAck)).u16(m.version).arr(m.agent_id).str(m.name, kMaxDeviceName);
    w.u8(uint8_t(m.mode));
    if (m.mode == Mode::Pair) {
        w.arr(m.sid).arr(m.y);
    } else {
        w.arr(m.nonce).arr(m.tag);
    }
    return w.take();
}

bool decode_hello_ack(Reader& r, HelloAck& m) {
    m.version = r.u16();
    m.agent_id = r.arr<16>();
    m.name = clean_device_name(r.str(kMaxDeviceName));
    uint8_t mode = r.u8();
    if (!r.ok()) return false;
    if (mode == uint8_t(Mode::Pair)) {
        m.mode = Mode::Pair;
        m.sid = r.arr<16>();
        m.y = r.arr<32>();
    } else if (mode == uint8_t(Mode::Resume)) {
        m.mode = Mode::Resume;
        m.nonce = r.arr<32>();
        m.tag = r.arr<32>();
    } else {
        return false;
    }
    return r.at_end();
}

Bytes encode_pair_response(const Key32& y, const Key32& tag) {
    Writer w;
    w.u8(uint8_t(MsgType::PairResponse)).arr(y).arr(tag);
    return w.take();
}

Bytes encode_pair_confirm(const Key32& tag) {
    Writer w;
    w.u8(uint8_t(MsgType::PairConfirm)).arr(tag);
    return w.take();
}

Bytes encode_resume_confirm(const Key32& tag) {
    Writer w;
    w.u8(uint8_t(MsgType::ResumeConfirm)).arr(tag);
    return w.take();
}

bool decode_two_keys(Reader& r, Key32& a, Key32& b) {
    a = r.arr<32>();
    b = r.arr<32>();
    return r.at_end();
}

bool decode_key(Reader& r, Key32& k) {
    k = r.arr<32>();
    return r.at_end();
}

Bytes encode_error(const ErrorMsg& m) {
    Writer w;
    w.u8(uint8_t(MsgType::Error)).u16(uint16_t(m.code)).str(m.message, kMaxErrorText);
    return w.take();
}

bool decode_error(Reader& r, ErrorMsg& m) {
    m.code = ErrorCode(r.u16());
    m.message = r.str(kMaxErrorText);
    return r.at_end();
}

Bytes encode_simple(MsgType t) { return Bytes{uint8_t(t)}; }

Bytes encode_set_volume(MsgType t, const std::string& id, uint16_t volume) {
    Writer w;
    w.u8(uint8_t(t)).str(id, kMaxId).u16(clamp_volume(volume));
    return w.take();
}

Bytes encode_set_mute(MsgType t, const std::string& id, bool muted) {
    Writer w;
    w.u8(uint8_t(t)).str(id, kMaxId).u8(muted ? 1 : 0);
    return w.take();
}

Bytes encode_id(MsgType t, const std::string& id) {
    Writer w;
    w.u8(uint8_t(t)).str(id, t == MsgType::GetIcon ? kMaxIconKey : kMaxId);
    return w.take();
}

Bytes encode_seq(MsgType t, uint32_t seq) {
    Writer w;
    w.u8(uint8_t(t)).u32(seq);
    return w.take();
}

Bytes encode_endpoint(const EndpointInfo& e) {
    Writer w;
    uint8_t flags = uint8_t((e.is_default ? 1 : 0) | (e.muted ? 2 : 0) | (e.input ? 4 : 0) |
                            (e.is_default_input ? 8 : 0));
    w.u8(uint8_t(MsgType::Endpoint)).str(e.id, kMaxId).str(e.name, kMaxDisplayName).u8(flags);
    w.u16(clamp_volume(e.volume)).u16(e.meter);
    return w.take();
}

Bytes encode_source(const SourceInfo& s) {
    Writer w;
    uint8_t flags = (s.muted ? 1 : 0) | (s.playing ? 2 : 0) | (s.system_sounds ? 4 : 0);
    w.u8(uint8_t(MsgType::Source)).str(s.id, kMaxId).str(s.endpoint_id, kMaxId);
    w.str(s.name, kMaxDisplayName).str(s.icon_key, kMaxIconKey).u32(s.pid).u8(flags);
    w.u16(clamp_volume(s.volume)).u16(s.meter);
    w.str(s.route, kMaxId);  // 1.2
    return w.take();
}

Bytes encode_agent_info(const AgentInfo& a) {
    Writer w;
    w.u8(uint8_t(MsgType::AgentInfo)).u32(a.features).u8(a.panic ? 1 : 0).str(a.os, kMaxOsName);
    return w.take();
}

Bytes encode_set_route(const std::string& source_id, const std::string& endpoint_id) {
    Writer w;
    w.u8(uint8_t(MsgType::SetSourceRoute)).str(source_id, kMaxId).str(endpoint_id, kMaxId);
    return w.take();
}

bool decode_agent_info(Reader& r, AgentInfo& a) {
    a.features = r.u32();
    a.panic = r.u8() != 0;
    a.os = r.str(kMaxOsName);
    return r.ok();  // tolerant: later versions may append fields
}

Bytes encode_stream_start(const StreamStart& s) {
    Writer w;
    w.u8(uint8_t(MsgType::StreamStart)).u16(s.udp_port).u8(s.flags).str(s.endpoint_id, kMaxId);
    return w.take();
}

bool decode_stream_start(Reader& r, StreamStart& s) {
    s.udp_port = r.u16();
    s.flags = r.u8();
    s.endpoint_id = r.str(kMaxId);
    return r.ok();  // tolerant: later versions may append options
}

Bytes encode_stream_state(const StreamState& s) {
    Writer w;
    w.u8(uint8_t(MsgType::StreamState)).u8(s.active ? 1 : 0).u8(s.codec).u32(s.sample_rate).u8(s.channels);
    w.u16(s.frame_ms).raw(s.key.data(), s.key.size()).u16(s.error);
    return w.take();
}

bool decode_stream_state(Reader& r, StreamState& s) {
    s.active = r.u8() != 0;
    s.codec = r.u8();
    s.sample_rate = r.u32();
    s.channels = r.u8();
    s.frame_ms = r.u16();
    const uint8_t* k = r.take(32);
    if (!k) return false;
    std::memcpy(s.key.data(), k, 32);
    s.error = r.u16();
    return r.ok();
}

bool decode_set_route(Reader& r, std::string& source_id, std::string& endpoint_id) {
    source_id = r.str(kMaxId);
    endpoint_id = r.str(kMaxId);
    return r.at_end();
}

Bytes encode_icon(const std::string& key, const uint8_t* png, size_t n) {
    Writer w;
    if (n > kMaxIconBytes) n = 0;  // never emit an oversize frame; empty = no icon
    w.u8(uint8_t(MsgType::Icon)).str(key, kMaxIconKey).u32(uint32_t(n)).raw(png, n);
    return w.take();
}

bool decode_set_volume(Reader& r, std::string& id, uint16_t& volume) {
    id = r.str(kMaxId);
    volume = clamp_volume(r.u16());
    return r.at_end();
}

bool decode_set_mute(Reader& r, std::string& id, bool& muted) {
    id = r.str(kMaxId);
    muted = r.u8() != 0;
    return r.at_end();
}

bool decode_id(Reader& r, std::string& id, size_t max_len) {
    id = r.str(max_len);
    return r.at_end();
}

bool decode_seq(Reader& r, uint32_t& seq) {
    seq = r.u32();
    return r.at_end();
}

// Endpoint/Source decoders tolerate trailing bytes: later minor revisions may
// append fields, and receivers ignore what they do not know (protocol §6).
bool decode_endpoint(Reader& r, EndpointInfo& e) {
    e.id = r.str(kMaxId);
    e.name = r.str(kMaxDisplayName);
    uint8_t flags = r.u8();
    e.volume = clamp_volume(r.u16());
    e.is_default = flags & 1;
    e.muted = flags & 2;
    e.input = flags & 4;             // 1.2
    e.is_default_input = flags & 8;  // 1.2
    e.meter = r.remaining() >= 2 ? r.u16() : 0;  // added in 1.1; absent from older agents
    return r.ok();
}

bool decode_source(Reader& r, SourceInfo& s) {
    s.id = r.str(kMaxId);
    s.endpoint_id = r.str(kMaxId);
    s.name = r.str(kMaxDisplayName);
    s.icon_key = r.str(kMaxIconKey);
    s.pid = r.u32();
    uint8_t flags = r.u8();
    s.volume = clamp_volume(r.u16());
    s.muted = flags & 1;
    s.playing = flags & 2;
    s.system_sounds = flags & 4;
    s.meter = r.remaining() >= 2 ? r.u16() : 0;
    s.route = r.remaining() > 0 ? r.str(kMaxId) : std::string();  // 1.2
    return r.ok();
}

bool decode_icon(Reader& r, std::string& key, Bytes& png) {
    key = r.str(kMaxIconKey);
    uint32_t n = r.u32();
    if (!r.ok() || n > kMaxIconBytes) return false;
    const uint8_t* p = r.take(n);
    if (!p) return false;
    png.assign(p, p + n);
    return r.at_end();
}

Bytes encode_flag(MsgType t, bool on) {
    Writer w;
    w.u8(uint8_t(t)).u8(on ? 1 : 0);
    return w.take();
}

bool decode_flag(Reader& r, bool& on) {
    on = r.u8() != 0;
    return r.at_end();
}

Bytes encode_meters(const std::vector<MeterLevel>& levels) {
    Writer w;
    const size_t n = std::min(levels.size(), kMaxMetersPerMessage);
    w.u8(uint8_t(MsgType::Meters)).u8(uint8_t(n));
    for (size_t i = 0; i < n; ++i) w.u16(levels[i].slot).u8(levels[i].level);
    return w.take();
}

bool decode_meters(Reader& r, std::vector<MeterLevel>& levels) {
    const uint8_t n = r.u8();
    levels.clear();
    levels.reserve(n);
    for (uint8_t i = 0; i < n && r.ok(); ++i) {
        MeterLevel m;
        m.slot = r.u16();
        m.level = r.u8();
        levels.push_back(m);
    }
    return r.ok();
}

Bytes serialize_pairing(const PairingRecord& p) {
    Writer w;
    w.raw(reinterpret_cast<const uint8_t*>("ARPR"), 4).u8(1).arr(p.agent_id).arr(p.pair_key);
    w.str(p.agent_name, kMaxDeviceName);
    return w.take();
}

std::optional<PairingRecord> deserialize_pairing(const uint8_t* data, size_t n) {
    Reader r(data, n);
    const uint8_t* magic = r.take(4);
    if (!magic || std::memcmp(magic, "ARPR", 4) != 0) return std::nullopt;
    if (r.u8() != 1) return std::nullopt;
    PairingRecord p;
    p.agent_id = r.arr<16>();
    p.pair_key = r.arr<32>();
    p.agent_name = r.str(kMaxDeviceName);
    if (!r.at_end()) return std::nullopt;
    return p;
}

}  // namespace arc
