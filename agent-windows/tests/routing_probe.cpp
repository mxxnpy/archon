// Manual check of per-app routing on this Windows build (not part of ctest):
// pins *this probe process* to each render device in turn, reads it back, then
// clears the pin. Touches no other application.

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/app_routing.hpp"

using Microsoft::WRL::ComPtr;

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int failures = 0;
    {
        // Path helpers.
        const std::wstring id = L"{0.0.0.00000000}.{11111111-2222-3333-4444-555555555555}";
        const std::wstring path = agent::routing_device_path(id);
        if (agent::routing_endpoint_id(path) != id) ++failures, std::printf("FAIL path roundtrip\n");
        if (!agent::routing_endpoint_id(L"garbage").empty()) ++failures, std::printf("FAIL garbage path\n");

        agent::AppRouting r;
        std::printf("routing available: %s\n", r.available() ? "yes" : "no");
        if (!r.available()) return failures ? 1 : 2;

        ComPtr<IMMDeviceEnumerator> en;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        ComPtr<IMMDeviceCollection> coll;
        en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
        UINT n = 0;
        coll->GetCount(&n);
        std::vector<std::wstring> ids;
        for (UINT i = 0; i < n; ++i) {
            ComPtr<IMMDevice> d;
            LPWSTR w = nullptr;
            if (SUCCEEDED(coll->Item(i, &d)) && SUCCEEDED(d->GetId(&w))) {
                ids.emplace_back(w);
                CoTaskMemFree(w);
            }
        }
        std::printf("render devices: %u\n", n);
        // Windows only accepts a process that owns an audio session: open a
        // silent shared-mode stream on the default output (like any app playing).
        ComPtr<IMMDevice> def;
        ComPtr<IAudioClient> client;
        WAVEFORMATEX* fmt = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def)) &&
            SUCCEEDED(def->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) &&
            SUCCEEDED(client->GetMixFormat(&fmt)) &&
            SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, fmt, nullptr)) &&
            SUCCEEDED(client->Start()))
            std::printf("probe has an audio session\n");
        else
            std::printf("could not open an audio session\n");
        if (fmt) CoTaskMemFree(fmt);
        const DWORD self = GetCurrentProcessId();
        std::printf("before: '%ls'\n", r.get(self).c_str());
        for (const auto& id2 : ids) {
            const bool ok = r.set(self, id2);
            const std::wstring back = r.get(self);
            const bool match = _wcsicmp(back.c_str(), id2.c_str()) == 0;
            std::printf("set %ls -> %s (hr 0x%08lx), read back %s\n", id2.c_str(), ok ? "ok" : "FAILED",
                        static_cast<unsigned long>(r.last_error()), match ? "MATCH" : "MISMATCH");
            if (!ok || !match) ++failures;
        }
        r.set(self, L"");
        const std::wstring cleared = r.get(self);
        std::printf("after clear: '%ls'\n", cleared.c_str());
        if (!cleared.empty()) ++failures;
    }
    CoUninitialize();
    std::printf(failures ? "FAILED (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
