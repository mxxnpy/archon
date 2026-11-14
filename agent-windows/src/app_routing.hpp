#pragma once
// Per-application output device ("App volume and device preferences" in
// Windows Settings). Windows has no public API for it; this uses the same
// internal WinRT factory as the Settings page (Windows 10 1803+, interface
// changed in 21H2). If neither layout is found, routing is reported as
// unavailable and the phone hides the option.
//
// Must be used on a thread with COM/WinRT initialized (the backend worker).

#include <windows.h>

#include <string>

namespace agent {

class AppRouting {
public:
    AppRouting();
    ~AppRouting();
    AppRouting(const AppRouting&) = delete;
    AppRouting& operator=(const AppRouting&) = delete;

    bool available() const { return factory_ != nullptr; }
    // endpoint_id: an MMDevice id ("{0.0.0.00000000}.{guid}"); empty = follow
    // the system default again. Applies to the multimedia and console roles.
    bool set(DWORD pid, const std::wstring& endpoint_id);
    // MMDevice id the process is pinned to, or empty.
    std::wstring get(DWORD pid);
    long last_error() const { return last_hr_; }

private:
    void* factory_ = nullptr;  // IAudioPolicyConfigFactory (either layout)
    long last_hr_ = 0;
};

// "\\?\SWD#MMDEVAPI#{id}#{e6327cad-...}" <-> "{id}" (exposed for tests).
std::wstring routing_device_path(const std::wstring& endpoint_id);
std::wstring routing_endpoint_id(const std::wstring& device_path);

}  // namespace agent
