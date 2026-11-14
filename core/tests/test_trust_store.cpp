#include <cstdio>
#include <filesystem>
#include <fstream>

#include "archon/crypto.hpp"
#include "archon/trust_store.hpp"
#include "test.hpp"

using namespace arc;
namespace fs = std::filesystem;

static std::string temp_file(const char* name) {
    fs::path p = fs::temp_directory_path() / ("archon-test-" + to_hex(random_array<4>()) + "-" + name);
    return p.u8string();
}

// Toy "protection": XOR with a byte plus a checksum, to exercise the hooks.
static bool protect(const Bytes& in, Bytes& out) {
    out = in;
    uint8_t sum = 0;
    for (auto& b : out) {
        sum = uint8_t(sum + b);
        b ^= 0x5A;
    }
    out.push_back(sum);
    return true;
}
static bool unprotect(const Bytes& in, Bytes& out) {
    if (in.empty()) return false;
    out.assign(in.begin(), in.end() - 1);
    uint8_t sum = 0;
    for (auto& b : out) {
        b ^= 0x5A;
        sum = uint8_t(sum + b);
    }
    return sum == in.back();
}

TEST(trust_store_persists_and_reloads) {
    const std::string path = temp_file("trust.bin");
    PairedController a;
    a.id = random_array<16>();
    a.key = random_array<32>();
    a.name = "Meu iPhone";
    a.paired_at = 1700000000;
    {
        FileTrustStore s(path, protect, unprotect);
        REQUIRE(s.load(nullptr));
        CHECK(s.list().empty());
        CHECK(s.put(a));
    }
    {
        FileTrustStore s(path, protect, unprotect);
        REQUIRE(s.load(nullptr));
        auto f = s.find(a.id);
        REQUIRE(f.has_value());
        CHECK(f->key == a.key && f->name == a.name && f->paired_at == a.paired_at);
        CHECK(s.remove(a.id));
        CHECK(!s.remove(a.id));
    }
    {
        FileTrustStore s(path, protect, unprotect);
        REQUIRE(s.load(nullptr));
        CHECK(s.list().empty());
    }
    fs::remove(path);
}

TEST(trust_store_rejects_tampered_file) {
    const std::string path = temp_file("tamper.bin");
    {
        FileTrustStore s(path, protect, unprotect);
        s.load(nullptr);
        PairedController a;
        a.id = random_array<16>();
        s.put(a);
    }
    {
        std::fstream f(fs::u8path(path), std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(6);
        f.put(char(0x77));
    }
    FileTrustStore s(path, protect, unprotect);
    std::string err;
    CHECK(!s.load(&err));
    CHECK(!err.empty());
    fs::remove(fs::u8path(path));
}
