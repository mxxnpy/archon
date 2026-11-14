#pragma once
// Platform-neutral pieces of the macOS agent, unit tested on every OS:
// the agent.cfg format and the sealed trust-store blob.

#include <cstdint>
#include <string>

#include "archon/bytes.hpp"

namespace agent {

struct AgentConfig {
    arc::Id16 agent_id{};
    bool have_id = false;
    uint16_t port = 47321;
    int lockouts = 0;
    int64_t locked_until = 0;
};

// Same keys as the Windows agent: id, port, lockouts, locked_until.
// Unknown lines are ignored; an invalid id leaves have_id false.
AgentConfig parse_config(const std::string& text);
std::string serialize_config(const AgentConfig& cfg);

// Trust-store wrapping with a key kept in the Keychain:
// "ARK1" | nonce (24) | mac (16) | XChaCha20-Poly1305 ciphertext.
bool seal_blob(const arc::Key32& key, const arc::Bytes& plain, arc::Bytes& out);
bool open_blob(const arc::Key32& key, const arc::Bytes& sealed, arc::Bytes& plain);

}  // namespace agent
