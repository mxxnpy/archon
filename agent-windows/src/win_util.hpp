#pragma once
// Windows helpers for the agent: text conversion, app data, DPAPI, config,
// autostart, logging, network addresses and icon -> PNG conversion.

#include <windows.h>

#include <string>
#include <vector>

#include "archon/bytes.hpp"

namespace agent {

std::string to_utf8(const std::wstring& w);
std::wstring to_wide(const std::string& s);

// %APPDATA%\Archon (created on demand).
std::wstring app_dir();
std::wstring exe_path();
// Atomic, durable file replace. 0 on success, else the Win32 error.
DWORD write_file_atomic(const std::wstring& path, const std::string& data);

bool dpapi_protect(const arc::Bytes& in, arc::Bytes& out);
bool dpapi_unprotect(const arc::Bytes& in, arc::Bytes& out);

struct Config {
    arc::Id16 agent_id{};
    uint16_t port = 47321;
    bool first_run = false;
    int lockouts = 0;             // pairing lockout escalation (survives restarts)
    int64_t locked_until = 0;     // unix seconds
};
void save_lockout(int lockouts, int64_t locked_until);
// Loads or creates %APPDATA%\Archon\agent.cfg.
Config load_config();

bool autostart_enabled();
bool set_autostart(bool on);

void log_line(const std::string& line);

// IPv4/IPv6 addresses of up, non-loopback adapters (for manual connection).
std::vector<std::string> local_addresses();

// Computer name for display and mDNS (NetBIOS-safe, at most 63 chars).
std::wstring computer_name();
// Short, human name shown on the phone: the user's first name, else the computer name.
std::wstring display_name();

// HICON -> PNG bytes (premultiplied alpha handled). Needs COM initialized.
bool icon_to_png(HICON icon, arc::Bytes& png);

bool is_portuguese_ui();

}  // namespace agent
