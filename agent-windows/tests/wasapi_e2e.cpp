// Manual end-to-end check against the real Windows audio stack:
// WasapiBackend + Server + Client over loopback. Pairs, lists every endpoint
// and source, changes the "System sounds" volume and mute, verifies the change
// comes back through WASAPI notifications, fetches an app icon, then restores
// the original values. Needs an active output device.
//
// Run: archon_wasapi_e2e.exe   (exit code 0 = pass)

#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

#include "archon/client.hpp"
#include "archon/crypto.hpp"
#include "archon/server.hpp"
#include "../src/wasapi_backend.hpp"

using namespace arc;
using namespace std::chrono_literals;

namespace {

std::mutex mu;
std::string code;
std::map<std::string, EndpointInfo> eps;
std::map<std::string, SourceInfo> srcs;
std::map<std::string, Bytes> icons;
std::map<uint16_t, uint8_t> levels;
int meter_frames = 0;
ClientStatus status = ClientStatus::Idle;
int synced = 0;
int failures = 0;

struct Store : TrustStore {
    std::map<Id16, PairedController> m;
    std::mutex smu;
    std::optional<PairedController> find(const Id16& id) override {
        std::lock_guard<std::mutex> lk(smu);
        auto it = m.find(id);
        return it == m.end() ? std::nullopt : std::optional<PairedController>(it->second);
    }
    bool put(const PairedController& c) override {
        std::lock_guard<std::mutex> lk(smu);
        m[c.id] = c;
        return true;
    }
    bool remove(const Id16& id) override {
        std::lock_guard<std::mutex> lk(smu);
        return m.erase(id) > 0;
    }
    std::vector<PairedController> list() override { return {}; }
};

struct Obs : ServerObserver {
    void on_pair_request(uint64_t, const std::string&, const std::string& c) override {
        std::lock_guard<std::mutex> lk(mu);
        code = c;
    }
};

struct L : ClientListener {
    void on_status(ClientStatus s, ErrorCode, const std::string&) override {
        std::lock_guard<std::mutex> lk(mu);
        status = s;
    }
    void on_event(const ControllerEvent& e) override {
        std::lock_guard<std::mutex> lk(mu);
        using K = ControllerEvent::Kind;
        if (e.kind == K::Endpoint) eps[e.endpoint.id] = e.endpoint;
        if (e.kind == K::EndpointRemoved) eps.erase(e.id);
        if (e.kind == K::Source) srcs[e.source.id] = e.source;
        if (e.kind == K::SourceRemoved) srcs.erase(e.id);
        if (e.kind == K::StateSynced) ++synced;
        if (e.kind == K::Icon) icons[e.id] = e.data;
        if (e.kind == K::Meters) {
            ++meter_frames;
            for (auto& m : e.meters) levels[m.slot] = std::max(levels[m.slot], m.level);
        }
    }
};

template <class P>
bool wait(P p, int ms = 5000) {
    for (int i = 0; i < ms / 10; ++i) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (p()) return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    agent::WasapiBackend backend;
    Store store;
    Obs obs;
    ServerConfig cfg;
    cfg.agent_id = random_array<16>();
    cfg.agent_name = "E2E";
    cfg.loopback_only = true;
    cfg.port = 0;
    Server server(cfg, backend, store, obs);
    std::string err;
    if (!server.start(&err)) {
        std::printf("FAIL server start: %s\n", err.c_str());
        return 1;
    }
    L listener;
    Client client(random_array<16>(), "e2e", listener);
    client.pair("127.0.0.1", server.port());
    check(wait([] { return !code.empty() && status == ClientStatus::AwaitingCode; }), "pairing code issued");
    std::string c;
    {
        std::lock_guard<std::mutex> lk(mu);
        c = code;
    }
    client.submit_code(c);
    check(wait([] { return status == ClientStatus::Connected && synced > 0; }), "paired and full state received");

    std::string sys_id, icon_key;
    SourceInfo sys;
    {
        std::lock_guard<std::mutex> lk(mu);
        std::printf("\nEndpoints (%zu):\n", eps.size());
        for (auto& [id, e] : eps)
            std::printf("  %s%-40s vol=%5.1f%% %s\n", e.is_default ? "* " : "  ", e.name.c_str(), e.volume / 100.0,
                        e.muted ? "muted" : "");
        std::printf("Sources (%zu):\n", srcs.size());
        for (auto& [id, s] : srcs) {
            std::printf("  %-32s pid=%-6u vol=%5.1f%% %s%s icon=%s\n", s.name.c_str(), s.pid, s.volume / 100.0,
                        s.muted ? "muted " : "", s.playing ? "playing" : "", s.icon_key.empty() ? "-" : s.icon_key.c_str());
            if (s.system_sounds && sys_id.empty()) {
                sys_id = id;
                sys = s;
            }
            if (!s.icon_key.empty() && icon_key.empty()) icon_key = s.icon_key;
        }
        std::printf("\n");
    }
    check(!eps.empty(), "at least one output device listed");

    if (!sys_id.empty()) {
        const uint16_t target = sys.volume >= 5000 ? uint16_t(sys.volume - 1000) : uint16_t(sys.volume + 1000);
        client.set_source_volume(sys_id, target);
        check(wait([&] { return srcs[sys_id].volume == target; }), "system sounds volume changed via WASAPI");
        client.set_source_mute(sys_id, !sys.muted);
        check(wait([&] { return srcs[sys_id].muted == !sys.muted; }), "system sounds mute toggled via WASAPI");
        // Restore.
        client.set_source_volume(sys_id, sys.volume);
        client.set_source_mute(sys_id, sys.muted);
        check(wait([&] { return srcs[sys_id].volume == sys.volume && srcs[sys_id].muted == sys.muted; }),
              "original system sounds volume/mute restored");
    } else {
        std::printf("SKIP no system sounds session\n");
    }

    if (!icon_key.empty()) {
        client.request_icon(icon_key);
        check(wait([&] { return icons.count(icon_key) > 0; }), "icon response received");
        std::lock_guard<std::mutex> lk(mu);
        const Bytes& png = icons[icon_key];
        check(png.size() > 8 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G',
              "icon is a PNG");
        std::printf("     icon bytes: %zu\n", png.size());
    } else {
        std::printf("SKIP no app session with an icon (play something to cover this)\n");
    }

    client.set_meters(true);
    std::this_thread::sleep_for(2s);
    {
        std::lock_guard<std::mutex> lk(mu);
        int nonzero = 0;
        for (auto& [slot, v] : levels) nonzero += v > 0;
        std::printf("     meter frames in 2 s: %d, slots seen: %zu, slots with signal: %d\n", meter_frames, levels.size(), nonzero);
        check(meter_frames > 0, "level meters stream from WASAPI");
        for (auto& [id, s] : srcs)
            if (s.playing && levels.count(s.meter))
                std::printf("     %-30s peak %3u/255\n", s.name.c_str(), unsigned(levels[s.meter]));
    }
    client.set_meters(false);
    client.disconnect();
    server.stop();
    std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
