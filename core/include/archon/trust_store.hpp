#pragma once
// File-backed list of paired controllers. The blob can be wrapped by an OS
// protection facility (DPAPI on Windows, Keychain-held key elsewhere).

#include <functional>
#include <map>
#include <mutex>

#include "archon/server.hpp"

namespace arc {

Bytes serialize_trust(const std::vector<PairedController>& items);
bool deserialize_trust(const uint8_t* p, size_t n, std::vector<PairedController>& out);

class FileTrustStore final : public TrustStore {
public:
    // Transform returns false on failure. Empty transforms store plaintext.
    using Transform = std::function<bool(const Bytes& in, Bytes& out)>;

    explicit FileTrustStore(std::string utf8_path, Transform protect = {}, Transform unprotect = {});

    // Missing file = empty store. False if the file exists but is unreadable or
    // corrupt (the caller decides whether to start fresh).
    bool load(std::string* err);

    std::optional<PairedController> find(const Id16& id) override;
    bool put(const PairedController& c) override;
    bool remove(const Id16& id) override;
    std::vector<PairedController> list() override;

private:
    bool save_locked();

    std::string path_;
    Transform protect_;
    Transform unprotect_;
    std::mutex mu_;
    std::map<Id16, PairedController> items_;
};

}  // namespace arc
