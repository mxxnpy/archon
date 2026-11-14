#include "tab_pipe.hpp"

#include <sddl.h>

#include <deque>
#include <vector>

#include "mini_json.hpp"
#include "win_proc.hpp"
#include "win_util.hpp"

namespace agent {

// Extension ids the host accepts: the unpacked build (fixed key in
// browser-extension/build.mjs) and, once published, the store ids.
static const char* const kChromiumOrigins[] = {
    "chrome-extension://bneibifbhplfblkgnoglbifijppilaed/",
};
static const char* const kFirefoxIds[] = {"tabs@archon.dev"};

struct TabPipeServer::Conn final : TabLink {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE wake = nullptr;
    std::mutex mu;
    std::deque<std::string> out;
    std::thread thread;
    std::atomic<bool> done{false};

    void send(const std::string& json) override {
        std::lock_guard<std::mutex> lk(mu);
        if (out.size() >= 256) return;  // the browser is not reading: drop, it gets fresh state later
        std::string frame(4, '\0');
        const uint32_t n = uint32_t(json.size());
        memcpy(frame.data(), &n, 4);
        out.push_back(frame + json);
        SetEvent(wake);
    }
};

TabPipeServer::TabPipeServer(TabSources& tabs) : tabs_(tabs) {}
TabPipeServer::~TabPipeServer() { stop(); }

bool TabPipeServer::start(std::string* err) {
    name_ = tab_pipe_name();
    host_path_ = dir_of(module_path()) + L"\\" + kTabsHostExe;
    stop_evt_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (name_.size() < 25 || !stop_evt_) {
        if (err) *err = "no user id for the tabs pipe";
        return false;
    }
    acceptor_ = std::thread([this] { accept_loop(); });
    return true;
}

void TabPipeServer::stop() {
    if (!stop_evt_) return;
    SetEvent(stop_evt_);
    if (acceptor_.joinable()) acceptor_.join();
    std::list<std::unique_ptr<Conn>> all;
    {
        std::lock_guard<std::mutex> lk(mu_);
        all.swap(conns_);
    }
    for (auto& c : all)
        if (c->thread.joinable()) c->thread.join();
    CloseHandle(stop_evt_);
    stop_evt_ = nullptr;
}

void TabPipeServer::accept_loop() {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(current_user_only_sddl().c_str(), SDDL_REVISION_1, &sd,
                                                              nullptr)) {
        log_line("browser tabs: cannot build the pipe's security descriptor");
        return;
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool first = true;
    while (WaitForSingleObject(stop_evt_, 0) == WAIT_TIMEOUT) {
        // Reap finished connections.
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto it = conns_.begin(); it != conns_.end();) {
                if ((*it)->done) {
                    if ((*it)->thread.joinable()) (*it)->thread.join();
                    it = conns_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        HANDLE p = CreateNamedPipeW(name_.c_str(),
                                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
        if (p == INVALID_HANDLE_VALUE) {
            // Someone else already owns the name (another agent?): give up rather than share it.
            log_line("browser tabs: pipe unavailable (" + std::to_string(GetLastError()) + ")");
            break;
        }
        first = false;
        OVERLAPPED ov{};
        ResetEvent(evt);
        ov.hEvent = evt;
        BOOL ok = ConnectNamedPipe(p, &ov);
        DWORD e = ok ? ERROR_PIPE_CONNECTED : GetLastError();
        if (e == ERROR_IO_PENDING) {
            HANDLE w[2] = {evt, stop_evt_};
            if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(p, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(p, &ov, &ignored, TRUE);
                CloseHandle(p);
                break;
            }
            DWORD ignored = 0;
            e = GetOverlappedResult(p, &ov, &ignored, FALSE) ? ERROR_PIPE_CONNECTED : GetLastError();
        }
        if (e != ERROR_PIPE_CONNECTED) {
            CloseHandle(p);
            continue;
        }
        // Only our own host (next to Archon.exe) may connect.
        ULONG client = 0;
        if (!GetNamedPipeClientProcessId(p, &client) || !same_path(process_image(client), host_path_)) {
            log_line("browser tabs: refused a connection from another program");
            DisconnectNamedPipe(p);
            CloseHandle(p);
            continue;
        }
        auto c = std::make_unique<Conn>();
        c->pipe = p;
        c->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Conn* raw = c.get();
        std::lock_guard<std::mutex> lk(mu_);
        conns_.push_back(std::move(c));
        raw->thread = std::thread([this, raw] { serve(raw); });
    }
    CloseHandle(evt);
    LocalFree(sd);
}

void TabPipeServer::serve(Conn* c) {
    const uint64_t id = tabs_.connect(c);
    if (id == 0) log_line("browser tabs: too many browsers connected");
    HANDLE rd_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE wr_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<char> buf(16384);
    std::string in;
    OVERLAPPED rov{};
    bool reading = false;
    bool alive = id != 0;
    while (alive) {
        if (!reading) {
            ResetEvent(rd_evt);
            rov = OVERLAPPED{};
            rov.hEvent = rd_evt;
            if (!ReadFile(c->pipe, buf.data(), DWORD(buf.size()), nullptr, &rov) && GetLastError() != ERROR_IO_PENDING)
                break;
            reading = true;
        }
        HANDLE w[3] = {rd_evt, c->wake, stop_evt_};
        const DWORD r = WaitForMultipleObjects(3, w, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) {
            DWORD n = 0;
            reading = false;
            if (!GetOverlappedResult(c->pipe, &rov, &n, FALSE) || n == 0) break;  // host gone
            in.append(buf.data(), n);
            // Complete frames: u32 length + JSON.
            while (in.size() >= 4) {
                uint32_t len = 0;
                memcpy(&len, in.data(), 4);
                if (len > kMaxFrame) {
                    alive = false;
                    break;
                }
                if (in.size() < 4 + size_t(len)) break;
                tabs_.message(id, in.substr(4, len));
                in.erase(0, 4 + size_t(len));
            }
        } else if (r == WAIT_OBJECT_0 + 1) {
            std::deque<std::string> q;
            {
                std::lock_guard<std::mutex> lk(c->mu);
                q.swap(c->out);
            }
            for (const std::string& f : q) {
                OVERLAPPED wov{};
                ResetEvent(wr_evt);
                wov.hEvent = wr_evt;
                DWORD n = 0;
                BOOL ok = WriteFile(c->pipe, f.data(), DWORD(f.size()), nullptr, &wov);
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    HANDLE ww[2] = {wr_evt, stop_evt_};
                    if (WaitForMultipleObjects(2, ww, FALSE, 5000) != WAIT_OBJECT_0) {
                        CancelIoEx(c->pipe, &wov);
                        GetOverlappedResult(c->pipe, &wov, &n, TRUE);
                        alive = false;
                        break;
                    }
                    ok = TRUE;
                }
                if (!ok || !GetOverlappedResult(c->pipe, &wov, &n, FALSE)) {
                    alive = false;
                    break;
                }
            }
        } else {
            break;  // stopping
        }
    }
    if (reading) {
        CancelIoEx(c->pipe, &rov);
        DWORD n = 0;
        GetOverlappedResult(c->pipe, &rov, &n, TRUE);
    }
    if (id) tabs_.disconnect(id);
    DisconnectNamedPipe(c->pipe);
    CloseHandle(c->pipe);
    CloseHandle(c->wake);
    CloseHandle(rd_evt);
    CloseHandle(wr_evt);
    c->done = true;
}

// ---- registration ---------------------------------------------------------------------

namespace {

const wchar_t* const kChromiumKeys[] = {
    L"Software\\Google\\Chrome\\NativeMessagingHosts\\",
    L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\",
    L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\",
    L"Software\\Chromium\\NativeMessagingHosts\\",
    L"Software\\Vivaldi\\NativeMessagingHosts\\",
};
const wchar_t kFirefoxKey[] = L"Software\\Mozilla\\NativeMessagingHosts\\";

bool write_if_changed(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        std::string cur(size_t(GetFileSize(f, nullptr)), '\0');
        DWORD n = 0;
        const bool same = ReadFile(f, cur.data(), DWORD(cur.size()), &n, nullptr) && cur == text;
        CloseHandle(f);
        if (same) return true;
    }
    return write_file_atomic(path, text) == 0;
}

bool set_default_value(const std::wstring& key, const std::wstring& value) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &h,
                        nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS s = RegSetValueExW(h, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                     DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return s == ERROR_SUCCESS;
}

std::string manifest(const std::string& host_path, const char* list_key, const char* const* ids, size_t n) {
    std::string j = "{\n  \"name\": \"" + std::string(kTabsHostName) +
                    "\",\n  \"description\": \"Archon: per-tab volume from your phone\",\n  \"path\": " +
                    json::quote(host_path) + ",\n  \"type\": \"stdio\",\n  \"" + list_key + "\": [";
    for (size_t i = 0; i < n; ++i) j += (i ? ", " : "") + json::quote(ids[i]);
    return j + "]\n}\n";
}

}  // namespace

bool register_tabs_host(std::string* err) {
    const std::wstring host = dir_of(module_path()) + L"\\" + kTabsHostExe;
    if (GetFileAttributesW(host.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = "archon-tabs-host.exe is not next to Archon.exe";
        return false;
    }
    const std::wstring chromium = app_dir() + L"\\" + to_wide(kTabsHostName) + L".chromium.json";
    const std::wstring firefox = app_dir() + L"\\" + to_wide(kTabsHostName) + L".firefox.json";
    const std::string hp = to_utf8(host);
    bool ok = write_if_changed(chromium, manifest(hp, "allowed_origins", kChromiumOrigins, std::size(kChromiumOrigins)));
    ok = write_if_changed(firefox, manifest(hp, "allowed_extensions", kFirefoxIds, std::size(kFirefoxIds))) && ok;
    for (const wchar_t* k : kChromiumKeys) ok = set_default_value(k + to_wide(kTabsHostName), chromium) && ok;
    ok = set_default_value(kFirefoxKey + to_wide(kTabsHostName), firefox) && ok;
    if (!ok && err) *err = "could not write the browser registration";
    return ok;
}

void unregister_tabs_host() {
    for (const wchar_t* k : kChromiumKeys) RegDeleteTreeW(HKEY_CURRENT_USER, (k + to_wide(kTabsHostName)).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (kFirefoxKey + to_wide(kTabsHostName)).c_str());
    DeleteFileW((app_dir() + L"\\" + to_wide(kTabsHostName) + L".chromium.json").c_str());
    DeleteFileW((app_dir() + L"\\" + to_wide(kTabsHostName) + L".firefox.json").c_str());
}

}  // namespace agent
