// Manual end-to-end check of the browser-tabs bridge on real Windows: this
// program plays the browser. It starts archon-tabs-host.exe with pipes on its
// stdin/stdout (as Chrome/Firefox do), speaks native messaging to it, and
// checks that the tabs reach TabSources through the per-user pipe and that
// volume commands come back. Built as "Archon.exe" in its own folder, next to
// a copy of the host, because the host only talks to the Archon.exe beside it.
//
// Also checks the two guards: a host copied elsewhere is refused by the
// agent's pipe, and a host whose neighbour is not Archon.exe refuses to talk.
// Skips (exit 0) when a real Archon agent already owns the pipe.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "mini_json.hpp"
#include "tab_pipe.hpp"
#include "tab_sources.hpp"
#include "win_proc.hpp"
#include "win_util.hpp"

using namespace agent;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

class Inner : public arc::AudioBackend {
public:
    bool start(arc::AudioSink& s, std::string*) override {
        arc::EndpointInfo e;
        e.id = "OUT1";
        e.name = "Speakers";
        e.is_default = true;
        s.endpoint_upsert(e);
        return true;
    }
    void stop() override {}
    void set_endpoint_volume(const std::string&, uint16_t) override {}
    void set_endpoint_mute(const std::string&, bool) override {}
    void set_source_volume(const std::string&, uint16_t) override {}
    void set_source_mute(const std::string&, bool) override {}
    bool icon_png(const std::string&, arc::Bytes&) override { return false; }
};

class Sink : public arc::AudioSink {
public:
    std::mutex mu;
    std::map<std::string, arc::SourceInfo> sources;
    void endpoint_upsert(const arc::EndpointInfo&) override {}
    void endpoint_removed(const std::string&) override {}
    void source_upsert(const arc::SourceInfo& s) override {
        std::lock_guard<std::mutex> lk(mu);
        sources[s.id] = s;
    }
    void source_removed(const std::string& id) override {
        std::lock_guard<std::mutex> lk(mu);
        sources.erase(id);
    }
    void meters(const std::vector<std::pair<std::string, uint8_t>>&) override {}
    size_t tabs() {
        std::lock_guard<std::mutex> lk(mu);
        size_t n = 0;
        for (auto& [id, s] : sources) n += s.tab ? 1 : 0;
        return n;
    }
    arc::SourceInfo get(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu);
        return sources[id];
    }
};

template <class F>
bool eventually(F f, int ms = 3000) {
    for (int i = 0; i < ms / 20; ++i) {
        if (f()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return f();
}

// A host process with our pipes on its stdin/stdout, like a browser starts it.
struct Host {
    PROCESS_INFORMATION pi{};
    HANDLE to = nullptr;    // we write -> host stdin
    HANDLE from = nullptr;  // host stdout -> we read
    bool start(const std::wstring& exe) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE in_r, in_w, out_r, out_w;
        if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) return false;
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_r;
        si.hStdOutput = out_w;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::wstring cmd = L"\"" + exe + L"\" chrome-extension://bneibifbhplfblkgnoglbifijppilaed/";
        const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       nullptr, &si, &pi);
        CloseHandle(in_r);
        CloseHandle(out_w);
        to = in_w;
        from = out_r;
        return ok;
    }
    void send(const std::string& json) {
        const uint32_t n = uint32_t(json.size());
        DWORD w = 0;
        WriteFile(to, &n, 4, &w, nullptr);
        WriteFile(to, json.data(), n, &w, nullptr);
    }
    // One framed message from the host (blocking, with a timeout via a thread).
    bool recv(std::string& out, int ms = 3000) {
        std::string got;
        std::atomic<bool> ok{false};
        std::thread t([&] {
            uint32_t n = 0;
            DWORD r = 0;
            if (!ReadFile(from, &n, 4, &r, nullptr) || r != 4 || n > 1 << 20) return;
            got.resize(n);
            DWORD off = 0;
            while (off < n) {
                if (!ReadFile(from, got.data() + off, n - off, &r, nullptr) || r == 0) return;
                off += r;
            }
            ok = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!ok && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
        if (!ok) CancelSynchronousIo(t.native_handle());
        t.join();
        out = got;
        return ok;
    }
    DWORD finish(int ms = 3000) {
        if (to) CloseHandle(to);  // the browser closes the port
        to = nullptr;
        DWORD code = 999;
        if (WaitForSingleObject(pi.hProcess, DWORD(ms)) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        else TerminateProcess(pi.hProcess, 998);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(from);
        return code;
    }
};

}  // namespace

int wmain() {
    Inner inner;
    Sink sink;
    TabSources tabs(inner, [](uint32_t pid) { return to_utf8(process_image(pid)); });
    std::string err;
    tabs.start(sink, &err);
    TabPipeServer server(tabs);
    if (!server.start(&err)) {
        std::printf("FAIL pipe server: %s\n", err.c_str());
        return 1;
    }
    std::this_thread::sleep_for(150ms);
    {
        // Is the pipe ours? A running Archon agent would own the name.
        ULONG owner = 0;
        HANDLE probe = CreateFileW(tab_pipe_name().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (probe != INVALID_HANDLE_VALUE) {
            GetNamedPipeServerProcessId(probe, &owner);
            CloseHandle(probe);
        }
        if (owner != GetCurrentProcessId()) {
            std::printf("SKIP an Archon agent is running and owns the tabs pipe\n");
            server.stop();
            return 0;
        }
    }
    const std::wstring here = dir_of(module_path());
    const std::wstring host_exe = here + L"\\" + kTabsHostExe;

    // 1. The real path: browser -> host -> pipe -> TabSources, and back.
    Host h;
    check(h.start(host_exe), "host started with stdio pipes");
    h.send(R"({"t":"hello","browser":"chrome","version":"1.0.0"})");
    h.send(R"({"t":"tabs","tabs":[{"id":11,"title":"Lo-fi radio","host":"youtube.com","audible":true,"muted":false,"volume":10000,"icon":""}]})");
    check(eventually([&] { return sink.tabs() == 1; }), "tab reached the agent as a source");
    const arc::SourceInfo s = sink.get("tab:1:11");
    check(s.tab && s.name == "Lo-fi radio" && s.detail == "youtube.com", "tab title and site");
    check(s.group == "Chrome" && s.endpoint_id == "OUT1", "tab grouped by browser on the default output");
    tabs.set_source_volume("tab:1:11", 3000);
    std::string reply;
    check(h.recv(reply), "volume command reached the browser side");
    agent::json::Value v;
    check(agent::json::parse(reply, v) && v["t"].string_or() == "volume" && v["id"].number_or(0) == 11 &&
              v["volume"].number_or(0) == 3000,
          "volume command content");
    tabs.set_source_mute("tab:1:11", true);
    check(h.recv(reply) && agent::json::parse(reply, v) && v["t"].string_or() == "mute" && v["muted"].bool_or(false),
          "mute command reached the browser side");
    const DWORD code = h.finish();
    check(code == 0, "host exits when the browser closes the port");
    check(eventually([&] { return sink.tabs() == 0; }), "the browser's tabs disappear with it");

    // 2. Any other program (here: this one) opening the pipe is refused by the agent.
    {
        HANDLE p = CreateFileW(tab_pipe_name().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        bool refused = p == INVALID_HANDLE_VALUE;
        if (!refused) {
            const std::string j = R"({"t":"tabs","tabs":[{"id":5,"title":"x"}]})";
            const uint32_t n = uint32_t(j.size());
            DWORD w = 0;
            WriteFile(p, &n, 4, &w, nullptr);
            WriteFile(p, j.data(), n, &w, nullptr);
            char b[4];
            DWORD r = 0;
            refused = !ReadFile(p, b, 4, &r, nullptr);  // disconnected by the agent
            CloseHandle(p);
        }
        std::this_thread::sleep_for(200ms);
        check(refused && sink.tabs() == 0, "another program cannot feed tabs through the pipe");
    }

    // 3. A host copied elsewhere (no Archon.exe beside it) refuses to relay at all.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring other_dir = std::wstring(tmp) + L"archon-tabs-e2e";
    CreateDirectoryW(other_dir.c_str(), nullptr);
    const std::wstring stray = other_dir + L"\\" + kTabsHostExe;
    CopyFileW(host_exe.c_str(), stray.c_str(), FALSE);
    Host h3;
    check(h3.start(stray), "lonely host started");
    check(h3.finish() == 3, "host refuses a pipe server that is not the Archon.exe beside it");
    DeleteFileW(stray.c_str());
    RemoveDirectoryW(other_dir.c_str());

    server.stop();
    tabs.stop();
    std::printf(failures ? "\n%d FAILED\n" : "\nALL PASSED\n", failures);
    return failures ? 1 : 0;
}
