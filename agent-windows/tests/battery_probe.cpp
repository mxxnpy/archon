// Manual, read-only check: every audio device with its container id and the
// battery Windows reports for it (Bluetooth headphones and speakers).
// Changes nothing, plays nothing, opens no window.

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>

#include "device_battery.hpp"
#include "win_util.hpp"

using Microsoft::WRL::ComPtr;

int wmain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto t0 = std::chrono::steady_clock::now();
    const auto batteries = agent::container_batteries();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("devices reporting a battery: %zu (scan %lld ms)\n", batteries.size(), (long long)ms);
    for (auto& [c, level] : batteries) std::printf("  %s  %d%%\n", c.c_str(), level);
    ComPtr<IMMDeviceEnumerator> en;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    for (EDataFlow flow : {eRender, eCapture}) {
        ComPtr<IMMDeviceCollection> coll;
        if (!en || FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) continue;
        UINT n = 0;
        coll->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            ComPtr<IMMDevice> dev;
            coll->Item(i, &dev);
            ComPtr<IPropertyStore> props;
            std::string name = "?";
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = agent::to_utf8(v.pwszVal);
                PropVariantClear(&v);
            }
            const std::string c = agent::endpoint_container(dev.Get());
            auto b = batteries.find(c);
            std::printf("%s %-50s %s %s\n", flow == eRender ? "OUT" : "MIC", name.c_str(), c.c_str(),
                        b == batteries.end() ? "" : (std::to_string(b->second) + "%").c_str());
        }
    }
    CoUninitialize();
    return 0;
}
