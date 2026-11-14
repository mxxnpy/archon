#include "portable.hpp"

#include <cstdlib>
#include <cstring>
#include <sstream>

#include "archon/crypto.hpp"
#include "monocypher.h"

namespace agent {

namespace {
constexpr char kMagic[4] = {'A', 'R', 'K', '1'};
constexpr size_t kNonce = 24, kMac = 16, kHeader = 4 + kNonce + kMac;
const char kAd[] = "ARC1-trust-store";
}  // namespace

AgentConfig parse_config(const std::string& text) {
    AgentConfig cfg;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("id=", 0) == 0) cfg.have_id = arc::from_hex(line.substr(3), cfg.agent_id.data(), 16);
        else if (line.rfind("lockouts=", 0) == 0) cfg.lockouts = std::atoi(line.c_str() + 9);
        else if (line.rfind("locked_until=", 0) == 0) cfg.locked_until = std::atoll(line.c_str() + 13);
        else if (line.rfind("port=", 0) == 0) {
            const int p = std::atoi(line.c_str() + 5);
            if (p > 0 && p < 65536) cfg.port = uint16_t(p);
        }
    }
    if (cfg.lockouts < 0) cfg.lockouts = 0;
    if (cfg.locked_until < 0) cfg.locked_until = 0;
    return cfg;
}

std::string serialize_config(const AgentConfig& cfg) {
    std::string s = "# Archon agent configuration\n";
    s += "id=" + arc::to_hex(cfg.agent_id) + "\n";
    s += "port=" + std::to_string(cfg.port) + "\n";
    s += "lockouts=" + std::to_string(cfg.lockouts) + "\n";
    s += "locked_until=" + std::to_string(cfg.locked_until) + "\n";
    return s;
}

bool seal_blob(const arc::Key32& key, const arc::Bytes& plain, arc::Bytes& out) {
    const auto nonce = arc::random_array<kNonce>();
    out.assign(kHeader + plain.size(), 0);
    std::memcpy(out.data(), kMagic, 4);
    std::memcpy(out.data() + 4, nonce.data(), kNonce);
    crypto_aead_lock(out.data() + kHeader, out.data() + 4 + kNonce, key.data(), nonce.data(),
                     reinterpret_cast<const uint8_t*>(kAd), sizeof kAd - 1, plain.data(), plain.size());
    return true;
}

bool open_blob(const arc::Key32& key, const arc::Bytes& sealed, arc::Bytes& plain) {
    if (sealed.size() < kHeader || std::memcmp(sealed.data(), kMagic, 4) != 0) return false;
    plain.assign(sealed.size() - kHeader, 0);
    const int rc = crypto_aead_unlock(plain.data(), sealed.data() + 4 + kNonce, key.data(), sealed.data() + 4,
                                      reinterpret_cast<const uint8_t*>(kAd), sizeof kAd - 1,
                                      sealed.data() + kHeader, plain.size());
    if (rc != 0) {
        arc::wipe(plain.data(), plain.size());
        plain.clear();
        return false;
    }
    return true;
}

}  // namespace agent
