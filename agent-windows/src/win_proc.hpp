#pragma once
// Process and pipe helpers shared by the agent and archon-tabs-host.exe (the
// native-messaging host the browser starts for the Archon extension).

#include <windows.h>

#include <cstdint>
#include <string>

namespace agent {

constexpr wchar_t kTabsHostExe[] = L"archon-tabs-host.exe";
constexpr char kTabsHostName[] = "dev.archon.tabs";

// The current user's SID as text ("S-1-5-21-...").
std::wstring user_sid();
// A fresh, unguessable pipe name for this run: \\.\pipe\archon-tabs-<SID>-<random>.
std::wstring new_tab_pipe_name();
// The agent publishes its pipe name where only this user, at normal integrity,
// can write (HKCU\Software\Archon, "TabsPipe"); the host reads it there.
bool publish_tab_pipe_name(const std::wstring& name);
void unpublish_tab_pipe_name(const std::wstring& name);
std::wstring published_tab_pipe_name();  // "" if missing or not ours
// Security descriptor string that grants the pipe only to the current user.
std::wstring current_user_only_sddl();

// Full path of a running process's executable ("" if unknown / no access).
std::wstring process_image(DWORD pid);
DWORD parent_pid(DWORD pid);
// True when `pid` runs as this same user at an integrity level at least ours
// (not a low-integrity sandbox, not another account).
bool process_is_peer(DWORD pid);
// True when the pipe's owner is this user.
bool pipe_owned_by_me(HANDLE pipe);
// Same file on disk (volume + file id: immune to 8.3 names, case, junctions).
bool same_file(const std::wstring& a, const std::wstring& b);
bool same_path(const std::wstring& a, const std::wstring& b);
std::wstring dir_of(const std::wstring& path);
std::wstring module_path();  // this exe

// Native messaging framing: u32 length (little-endian) + UTF-8 JSON.
constexpr uint32_t kMaxFrame = 4 * 1024 * 1024;

}  // namespace agent
