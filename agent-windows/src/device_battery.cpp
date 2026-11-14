#include "device_battery.hpp"

#include <initguid.h>
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <setupapi.h>
#include <wrl/client.h>

#include <vector>

using Microsoft::WRL::ComPtr;

namespace agent {

namespace {

// The battery level Windows shows for Bluetooth devices (hands-free / AVRCP
// battery reports and the GATT Battery Service): a byte, 0..100.
const DEVPROPKEY kBatteryLevel = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};

std::string guid_text(const GUID& g) {
    wchar_t w[64] = {0};
    StringFromGUID2(g, w, 64);
    std::string s;
    for (const wchar_t* p = w; *p; ++p) s += char(*p >= L'a' && *p <= L'f' ? *p - (L'a' - L'A') : *p);
    return s;
}

}  // namespace

std::string endpoint_container(IMMDevice* dev) {
    ComPtr<IPropertyStore> props;
    if (!dev || FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string out;
    if (SUCCEEDED(props->GetValue(PKEY_Device_ContainerId, &v)) && v.vt == VT_CLSID && v.puuid) out = guid_text(*v.puuid);
    PropVariantClear(&v);
    // Virtual and built-in devices share the PC's own container: never one physical device.
    if (out == "{00000000-0000-0000-FFFF-FFFFFFFFFFFF}") out.clear();
    return out;
}

std::map<std::string, int> container_batteries() {
    std::map<std::string, int> out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA info{sizeof info};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        DEVPROPTYPE type = 0;
        BYTE level = 0;
        if (!SetupDiGetDevicePropertyW(set, &info, &kBatteryLevel, &type, &level, sizeof level, nullptr, 0) ||
            type != DEVPROP_TYPE_BYTE || level > 100)
            continue;
        GUID container{};
        if (!SetupDiGetDevicePropertyW(set, &info, &DEVPKEY_Device_ContainerId, &type, reinterpret_cast<BYTE*>(&container),
                                       sizeof container, nullptr, 0) ||
            type != DEVPROP_TYPE_GUID)
            continue;
        out[guid_text(container)] = level;  // several nodes of one device report the same level
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

}  // namespace agent
