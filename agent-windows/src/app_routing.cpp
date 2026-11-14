#include "app_routing.hpp"

#include <mmdeviceapi.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>

namespace agent {

namespace {

constexpr wchar_t kPrefix[] = L"\\\\?\\SWD#MMDEVAPI#";
constexpr wchar_t kRenderSuffix[] = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";

// Vtable layout of Windows.Media.Internal.AudioPolicyConfig's factory: after
// IInspectable come 19 methods we never call, then the three we use. Both
// known interface ids share this layout.
struct IAudioPolicyConfigFactory : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE m00() = 0;
    virtual HRESULT STDMETHODCALLTYPE m01() = 0;
    virtual HRESULT STDMETHODCALLTYPE m02() = 0;
    virtual HRESULT STDMETHODCALLTYPE m03() = 0;
    virtual HRESULT STDMETHODCALLTYPE m04() = 0;
    virtual HRESULT STDMETHODCALLTYPE m05() = 0;
    virtual HRESULT STDMETHODCALLTYPE m06() = 0;
    virtual HRESULT STDMETHODCALLTYPE m07() = 0;
    virtual HRESULT STDMETHODCALLTYPE m08() = 0;
    virtual HRESULT STDMETHODCALLTYPE m09() = 0;
    virtual HRESULT STDMETHODCALLTYPE m10() = 0;
    virtual HRESULT STDMETHODCALLTYPE m11() = 0;
    virtual HRESULT STDMETHODCALLTYPE m12() = 0;
    virtual HRESULT STDMETHODCALLTYPE m13() = 0;
    virtual HRESULT STDMETHODCALLTYPE m14() = 0;
    virtual HRESULT STDMETHODCALLTYPE m15() = 0;
    virtual HRESULT STDMETHODCALLTYPE m16() = 0;
    virtual HRESULT STDMETHODCALLTYPE m17() = 0;
    virtual HRESULT STDMETHODCALLTYPE m18() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(UINT pid, EDataFlow flow, ERole role,
                                                                       HSTRING device) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(UINT pid, EDataFlow flow, ERole role,
                                                                       HSTRING* device) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

// Windows 10 21H2 and later / Windows 10 1803..21H1.
const IID kIidCurrent = {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};
const IID kIidDownlevel = {0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}};

IAudioPolicyConfigFactory* as_factory(void* p) { return static_cast<IAudioPolicyConfigFactory*>(p); }

}  // namespace

std::wstring routing_device_path(const std::wstring& endpoint_id) {
    return endpoint_id.empty() ? std::wstring() : kPrefix + endpoint_id + kRenderSuffix;
}

std::wstring routing_endpoint_id(const std::wstring& path) {
    const std::wstring prefix = kPrefix, suffix = kRenderSuffix;
    if (path.size() <= prefix.size() + suffix.size()) return {};
    if (_wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0) return {};
    if (_wcsicmp(path.c_str() + path.size() - suffix.size(), suffix.c_str()) != 0) return {};
    return path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
}

AppRouting::AppRouting() {
    const wchar_t cls[] = L"Windows.Media.Internal.AudioPolicyConfig";
    HSTRING name = nullptr;
    if (FAILED(WindowsCreateString(cls, UINT32(wcslen(cls)), &name))) return;
    for (const IID* iid : {&kIidCurrent, &kIidDownlevel}) {
        void* f = nullptr;
        if (SUCCEEDED(RoGetActivationFactory(name, *iid, &f)) && f) {
            factory_ = f;
            break;
        }
    }
    WindowsDeleteString(name);
}

AppRouting::~AppRouting() {
    if (factory_) as_factory(factory_)->Release();
}

bool AppRouting::set(DWORD pid, const std::wstring& endpoint_id) {
    if (!factory_ || pid == 0) return false;
    const std::wstring path = routing_device_path(endpoint_id);
    HSTRING dev = nullptr;
    if (!path.empty() && FAILED(WindowsCreateString(path.c_str(), UINT32(path.size()), &dev))) return false;
    bool ok = true;
    for (ERole role : {eMultimedia, eConsole}) {
        const HRESULT hr = as_factory(factory_)->SetPersistedDefaultAudioEndpoint(pid, eRender, role, dev);
        if (FAILED(hr)) last_hr_ = hr;
        ok = SUCCEEDED(hr) && ok;
    }
    if (dev) WindowsDeleteString(dev);
    return ok;
}

std::wstring AppRouting::get(DWORD pid) {
    if (!factory_ || pid == 0) return {};
    HSTRING dev = nullptr;
    if (FAILED(as_factory(factory_)->GetPersistedDefaultAudioEndpoint(pid, eRender, eMultimedia, &dev)) || !dev)
        return {};
    UINT32 n = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(dev, &n);
    std::wstring path(raw ? raw : L"", n);
    WindowsDeleteString(dev);
    return routing_endpoint_id(path);
}

}  // namespace agent
