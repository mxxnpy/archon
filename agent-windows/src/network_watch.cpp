#include "network_watch.hpp"

#include <iphlpapi.h>
#include <netlistmgr.h>
#include <wrl/client.h>

#include "win_util.hpp"

using Microsoft::WRL::ComPtr;

namespace agent {

std::wstring current_network_name() {
    ComPtr<INetworkListManager> nlm;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nlm)))) return {};
    ComPtr<IEnumNetworks> nets;
    if (FAILED(nlm->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &nets))) return {};
    std::wstring best;
    int best_rank = -1;
    for (;;) {
        ComPtr<INetwork> net;
        ULONG got = 0;
        if (nets->Next(1, &net, &got) != S_OK || got == 0) break;
        BSTR name = nullptr;
        if (FAILED(net->GetName(&name)) || !name) continue;
        NLM_CONNECTIVITY c = NLM_CONNECTIVITY_DISCONNECTED;
        net->GetConnectivity(&c);
        // Prefer the network with internet, then any with local access.
        const int rank = (c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) ? 2
                         : (c & (NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_LOCALNETWORK)) ? 1
                                                                                                             : 0;
        if (rank > best_rank) {
            best_rank = rank;
            best.assign(name, SysStringLen(name));
        }
        SysFreeString(name);
    }
    return best;
}

void NetworkWatch::start(std::function<void(const std::string&)> changed) {
    changed_ = std::move(changed);
    stop_evt_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
}

void NetworkWatch::stop() {
    if (!stop_evt_) return;
    SetEvent(stop_evt_);
    if (thread_.joinable()) thread_.join();
    CloseHandle(stop_evt_);
    stop_evt_ = nullptr;
}

void NetworkWatch::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string last = "\x01";  // never a real name: the first report always goes out
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool stopping = false;
    while (!stopping) {
        // Windows may name the network a moment after the address changes: look
        // again a few seconds later too.
        for (int pass = 0; pass < 2 && !stopping; ++pass) {
            const std::string now = to_utf8(current_network_name());
            if (now != last) {
                last = now;
                log_line(now.empty() ? "network: none" : "network: " + now);
                changed_(now);
            }
            if (pass == 0) stopping = WaitForSingleObject(stop_evt_, 3000) == WAIT_OBJECT_0;
        }
        if (stopping) break;
        HANDLE h = nullptr;
        const DWORD r = NotifyAddrChange(&h, &ov);
        HANDLE w[2] = {ov.hEvent, stop_evt_};
        // An address change wakes us; otherwise look again every minute anyway.
        const DWORD got = WaitForMultipleObjects(2, w, FALSE, r == ERROR_IO_PENDING ? 60000 : 30000);
        if (r == ERROR_IO_PENDING && got != WAIT_OBJECT_0) CancelIPChangeNotify(&ov);
        if (got == WAIT_OBJECT_0 + 1) break;
    }
    CloseHandle(ov.hEvent);
    CoUninitialize();
}

bool FileProfileStore::load(arc::Bytes& data) {
    const std::wstring path = app_dir() + L"\\profiles.bin";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 8 * 1024 * 1024;
    if (ok) {
        data.resize(size_t(size.QuadPart));
        DWORD n = 0;
        ok = ReadFile(f, data.data(), DWORD(data.size()), &n, nullptr) && n == data.size();
    }
    CloseHandle(f);
    if (!ok) data.clear();
    return ok;
}

bool FileProfileStore::save(const arc::Bytes& data) {
    return write_file_atomic(app_dir() + L"\\profiles.bin", std::string(data.begin(), data.end())) == 0;
}

}  // namespace agent
