#include "archon/trust_store.hpp"

#include <cstdio>
#include <cstring>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <filesystem>
#include <fstream>
#include <system_error>

#include "archon/crypto.hpp"

namespace arc {

namespace fs = std::filesystem;

// Writes and flushes to the device before returning, so a crash after the
// rename can never leave an empty or partial store.
static bool write_durably(const fs::path& p, const Bytes& blob) {
#if defined(_WIN32)
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wbc") != 0) f = nullptr;  // 'c' = commit on fflush
#else
    FILE* f = std::fopen(p.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = std::fwrite(blob.data(), 1, blob.size(), f) == blob.size() && std::fflush(f) == 0;
#if !defined(_WIN32)
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

static fs::path to_path(const std::string& utf8) {
#if defined(_WIN32)
    return fs::u8path(utf8);
#else
    return fs::path(utf8);
#endif
}

Bytes serialize_trust(const std::vector<PairedController>& items) {
    Writer w;
    w.raw(reinterpret_cast<const uint8_t*>("ARTS"), 4).u8(1).u16(uint16_t(items.size()));
    for (const PairedController& c : items) {
        w.arr(c.id).arr(c.key).str(c.name, kMaxDeviceName);
        w.u64(uint64_t(c.paired_at)).u64(uint64_t(c.last_seen));
    }
    return w.take();
}

bool deserialize_trust(const uint8_t* p, size_t n, std::vector<PairedController>& out) {
    Reader r(p, n);
    const uint8_t* magic = r.take(4);
    if (!magic || std::memcmp(magic, "ARTS", 4) != 0 || r.u8() != 1) return false;
    const uint16_t count = r.u16();
    out.clear();
    for (uint16_t i = 0; i < count && r.ok(); ++i) {
        PairedController c;
        c.id = r.arr<16>();
        c.key = r.arr<32>();
        c.name = r.str(kMaxDeviceName);
        c.paired_at = int64_t(r.u64());
        c.last_seen = int64_t(r.u64());
        out.push_back(std::move(c));
    }
    return r.at_end();
}

FileTrustStore::FileTrustStore(std::string utf8_path, Transform protect, Transform unprotect)
    : path_(std::move(utf8_path)), protect_(std::move(protect)), unprotect_(std::move(unprotect)) {}

bool FileTrustStore::load(std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    items_.clear();
    const fs::path p = to_path(path_);
    std::error_code ec;
    if (!fs::exists(p, ec)) return true;
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open " + path_;
        return false;
    }
    Bytes blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Bytes plain;
    if (unprotect_) {
        if (!unprotect_(blob, plain)) {
            if (err) *err = "cannot decrypt " + path_;
            return false;
        }
    } else {
        plain = blob;
    }
    std::vector<PairedController> list;
    const bool ok = deserialize_trust(plain.data(), plain.size(), list);
    wipe(plain.data(), plain.size());
    if (!ok) {
        if (err) *err = "corrupt " + path_;
        return false;
    }
    for (auto& c : list) items_[c.id] = c;
    return true;
}

bool FileTrustStore::save_locked() {
    std::vector<PairedController> list;
    for (auto& [id, c] : items_) list.push_back(c);
    Bytes plain = serialize_trust(list);
    Bytes blob;
    if (protect_) {
        const bool ok = protect_(plain, blob);
        wipe(plain.data(), plain.size());
        if (!ok) return false;
    } else {
        blob = std::move(plain);
    }
    const fs::path p = to_path(path_);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    if (!write_durably(tmp, blob)) return false;
    // Atomic replace: readers see either the old or the new file.
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<PairedController> FileTrustStore::find(const Id16& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = items_.find(id);
    if (it == items_.end()) return std::nullopt;
    return it->second;
}

bool FileTrustStore::put(const PairedController& c) {
    std::lock_guard<std::mutex> lk(mu_);
    auto prev = items_.find(c.id);
    std::optional<PairedController> old;
    if (prev != items_.end()) old = prev->second;
    items_[c.id] = c;
    if (save_locked()) return true;
    if (old)
        items_[c.id] = *old;
    else
        items_.erase(c.id);
    return false;
}

bool FileTrustStore::remove(const Id16& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = items_.find(id);
    if (it == items_.end()) return false;
    PairedController old = it->second;
    items_.erase(it);
    if (save_locked()) return true;
    items_[id] = old;
    return false;
}

std::vector<PairedController> FileTrustStore::list() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PairedController> out;
    for (auto& [id, c] : items_) out.push_back(c);
    return out;
}

}  // namespace arc
