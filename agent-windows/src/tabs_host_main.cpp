// archon-tabs-host.exe: the native-messaging host for the Archon browser
// extension. The browser starts it (one per browser profile) and talks JSON
// over stdin/stdout; this relays those frames, unchanged, to the running
// Archon agent through a pipe only this Windows user can open, and back.
//
// It first tells the agent which browser executable it serves (so tabs can be
// placed on that browser's output). It refuses to talk to anything but the
// Archon.exe next to it. When the agent is not running it exits at once; the
// extension retries later.

#include <windows.h>

#include <string>
#include <thread>

#include "mini_json.hpp"
#include "win_proc.hpp"

using namespace agent;

namespace {

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The browser: our parent, or the parent of the cmd.exe that Chromium uses to start hosts.
std::wstring browser_exe() {
    DWORD p = parent_pid(GetCurrentProcessId());
    std::wstring img = process_image(p);
    const std::wstring name = img.substr(img.find_last_of(L"\\/") + 1);
    if (CompareStringOrdinal(name.c_str(), -1, L"cmd.exe", -1, TRUE) == CSTR_EQUAL) img = process_image(parent_pid(p));
    return img;
}

bool pipe_io(HANDLE pipe, bool write, void* buf, DWORD n, DWORD* done) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = write ? WriteFile(pipe, buf, n, nullptr, &ov) : ReadFile(pipe, buf, n, nullptr, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = TRUE;
    ok = ok && GetOverlappedResult(pipe, &ov, done, TRUE);
    CloseHandle(ov.hEvent);
    return ok && *done > 0;
}

bool pipe_write_all(HANDLE pipe, const void* data, DWORD n) {
    auto p = static_cast<const char*>(data);
    while (n) {
        DWORD w = 0;
        if (!pipe_io(pipe, true, const_cast<char*>(p), n, &w)) return false;
        p += w;
        n -= w;
    }
    return true;
}

HANDLE open_agent_pipe() {
    const std::wstring name = tab_pipe_name();
    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        if (GetLastError() != ERROR_PIPE_BUSY) return INVALID_HANDLE_VALUE;  // agent not running
        WaitNamedPipeW(name.c_str(), 100);
    }
    return INVALID_HANDLE_VALUE;
}

}  // namespace

int wmain() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (in == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE) return 2;

    HANDLE pipe = open_agent_pipe();
    if (pipe == INVALID_HANDLE_VALUE) return 1;
    // Only the Archon agent next to this host may receive the browser's tabs.
    ULONG server = 0;
    if (!GetNamedPipeServerProcessId(pipe, &server) ||
        !same_path(process_image(server), dir_of(module_path()) + L"\\Archon.exe")) {
        CloseHandle(pipe);
        return 3;
    }

    const std::string hello = "{\"t\":\"host\",\"exe\":" + json::quote(utf8(browser_exe())) + "}";
    const uint32_t len = uint32_t(hello.size());
    if (!pipe_write_all(pipe, &len, 4) || !pipe_write_all(pipe, hello.data(), len)) return 1;

    // Browser -> agent on its own thread; agent -> browser here. Bytes pass
    // through unchanged (both sides use the same length-prefixed framing).
    std::thread up([&] {
        char buf[16384];
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(in, buf, sizeof buf, &n, nullptr) || n == 0) break;  // browser closed the port
            if (!pipe_write_all(pipe, buf, n)) break;
        }
        CancelIoEx(pipe, nullptr);  // unblock the reader below
        ExitProcess(0);
    });
    up.detach();
    char buf[16384];
    for (;;) {
        DWORD n = 0;
        if (!pipe_io(pipe, false, buf, sizeof buf, &n)) break;  // agent closed
        DWORD w = 0;
        for (DWORD off = 0; off < n; off += w)
            if (!WriteFile(out, buf + off, n - off, &w, nullptr) || w == 0) return 0;
    }
    return 0;
}
