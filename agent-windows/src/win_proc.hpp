#pragma once
// Process and pipe helpers shared by the agent and archon-tabs-host.exe (the
// native-messaging host the browser starts for the Archon extension).

#include <windows.h>

#include <cstdint>
#include <string>

namespace agent {

constexpr wchar_t kTabsHostExe[] = L"archon-tabs-host.exe";
constexpr char kTabsHostName[] = "dev.archon.tabs";

// \\.\pipe\archon-tabs-<user SID>: one per Windows user.
std::wstring tab_pipe_name();
// Security descriptor string that grants the pipe only to the current user.
std::wstring current_user_only_sddl();
// Full path of a running process's executable ("" if unknown / no access).
std::wstring process_image(DWORD pid);
DWORD parent_pid(DWORD pid);
// Case-insensitive path equality.
bool same_path(const std::wstring& a, const std::wstring& b);
std::wstring dir_of(const std::wstring& path);
std::wstring module_path();  // this exe

// Native messaging framing: u32 length (little-endian) + UTF-8 JSON.
constexpr uint32_t kMaxFrame = 4 * 1024 * 1024;

}  // namespace agent
