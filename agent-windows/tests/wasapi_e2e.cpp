// Manual end-to-end check against the real Windows audio stack:
// WasapiBackend + Server + Client over loopback. Pairs, lists every endpoint
// and source, changes the "System sounds" volume and mute, verifies the change
// comes back through WASAPI notifications, fetches an app icon, then restores
// the original values. Needs an active output device.
//
// Run: archon_wasapi_e2e.exe   (exit code 0 = pass)

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

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
AgentInfo agent_info;
int agent_infos = 0;
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
        if (e.kind == K::AgentInfo) {
            agent_info = e.agent;
            ++agent_infos;
        }
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

    // ---- 1.2: AGENT_INFO, microphones, per-app routing ----
    check(wait([] { return agent_infos > 0; }), "AGENT_INFO received in the dump");
    {
        std::lock_guard<std::mutex> lk(mu);
        std::printf("     features=0x%x os=%s routing=%s\n", agent_info.features, agent_info.os.c_str(),
                    (agent_info.features & kFeatureRouting) ? "yes" : "no");
        check((agent_info.features & kFeatureInputs) && (agent_info.features & kFeaturePanic) && agent_info.os == "windows",
              "agent announces inputs + panic on windows");
    }
    std::string mic_id;
    EndpointInfo mic;
    {
        std::lock_guard<std::mutex> lk(mu);
        int inputs = 0;
        for (auto& [id, e] : eps) {
            if (!e.input) continue;
            ++inputs;
            if (e.is_default_input || mic_id.empty()) {
                mic_id = id;
                mic = e;
            }
        }
        std::printf("     microphones listed: %d\n", inputs);
    }
    if (!mic_id.empty()) {
        client.set_endpoint_mute(mic_id, !mic.muted);
        check(wait([&] { return eps[mic_id].muted == !mic.muted; }), "microphone mute toggled via WASAPI");
        client.set_endpoint_mute(mic_id, mic.muted);
        check(wait([&] { return eps[mic_id].muted == mic.muted; }), "microphone mute restored");
    } else {
        std::printf("SKIP no microphone on this PC\n");
    }

    // Open a silent stream so this test process is an app in the mixer, then
    // route it to another output and back (touches no real application).
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> mmde;
    Microsoft::WRL::ComPtr<IMMDevice> defdev;
    Microsoft::WRL::ComPtr<IAudioClient> ac;
    WAVEFORMATEX* fmt = nullptr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool stream_ok =
        SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&mmde))) &&
        SUCCEEDED(mmde->GetDefaultAudioEndpoint(eRender, eConsole, &defdev)) &&
        SUCCEEDED(defdev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &ac)) && SUCCEEDED(ac->GetMixFormat(&fmt)) &&
        SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, fmt, nullptr)) && SUCCEEDED(ac->Start());
    if (fmt) CoTaskMemFree(fmt);
    const uint32_t self = GetCurrentProcessId();
    auto my_source = [&]() -> const SourceInfo* {
        for (auto& [id, s] : srcs)
            if (s.pid == self) return &s;
        return nullptr;
    };
    if (stream_ok && (agent_info.features & kFeatureRouting)) {
        check(wait([&] { return my_source() != nullptr; }), "test process listed as an app");
        std::string sid, target;
        {
            std::lock_guard<std::mutex> lk(mu);
            const SourceInfo* me = my_source();
            if (me) {
                sid = me->id;
                for (auto& [id, e] : eps)
                    if (!e.input && id != me->endpoint_id) target = id;
            }
        }
        if (!sid.empty() && !target.empty()) {
            client.set_source_route(sid, target);
            check(wait([&] {
                      const SourceInfo* me = my_source();
                      return me && me->route == target;
                  }),
                  "app pinned to another output (route reported)");
            {
                std::lock_guard<std::mutex> lk(mu);
                const SourceInfo* me = my_source();
                std::printf("     now on endpoint %s\n", me ? me->endpoint_id.c_str() : "?");
                if (me) sid = me->id;
            }
            client.set_source_route(sid, "");
            check(wait([&] {
                      const SourceInfo* me = my_source();
                      return me && me->route.empty();
                  }),
                  "app unpinned (follows the default again)");
        } else {
            std::printf("SKIP routing: needs a second output device\n");
        }
    } else {
        std::printf("SKIP routing (stream %s, routing %s)\n", stream_ok ? "ok" : "failed",
                    (agent_info.features & kFeatureRouting) ? "available" : "unavailable");
    }
    if (ac) ac->Stop();

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
