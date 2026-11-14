#include "mdns.hpp"

#include <vector>

#include "win_util.hpp"

namespace agent {

static void WINAPI on_registered(DWORD status, PVOID, PDNS_SERVICE_INSTANCE instance) {
    log_line(status == ERROR_SUCCESS ? "mDNS: service registered" : "mDNS: registration failed, status " + std::to_string(status));
    if (instance) DnsServiceFreeInstance(instance);
}

MdnsAdvertiser::~MdnsAdvertiser() { stop(); }

bool MdnsAdvertiser::start(const std::wstring& instance, uint16_t port, const std::string& agent_id_hex,
                           std::string* err) {
    stop();
    // Instance labels may not contain dots and are limited to 63 bytes.
    std::wstring label = instance;
    for (auto& c : label)
        if (c == L'.') c = L'-';
    if (label.size() > 60) label.resize(60);
    const std::wstring service = label + L"._archon._tcp.local";
    const std::wstring host = computer_name() + L".local";
    const std::wstring id = to_wide(agent_id_hex);

    PCWSTR keys[] = {L"v", L"id", L"os"};
    PCWSTR values[] = {L"1", id.c_str(), L"windows"};
    instance_ = DnsServiceConstructInstance(service.c_str(), host.c_str(), nullptr, nullptr, port, 0, 0, 3, keys, values);
    if (!instance_) {
        if (err) *err = "DnsServiceConstructInstance failed";
        return false;
    }
    request_ = DNS_SERVICE_REGISTER_REQUEST{};
    request_.Version = DNS_QUERY_REQUEST_VERSION1;
    request_.InterfaceIndex = 0;  // all interfaces (Ethernet and Wi-Fi)
    request_.pServiceInstance = instance_;
    request_.pRegisterCompletionCallback = on_registered;
    request_.pQueryContext = nullptr;
    request_.unicastEnabled = FALSE;
    const DWORD rc = DnsServiceRegister(&request_, nullptr);
    if (rc != DNS_REQUEST_PENDING) {
        if (err) *err = "DnsServiceRegister failed: " + std::to_string(rc);
        DnsServiceFreeInstance(instance_);
        instance_ = nullptr;
        return false;
    }
    registered_ = true;
    return true;
}

void MdnsAdvertiser::stop() {
    if (registered_) {
        request_.pRegisterCompletionCallback = on_registered;
        DnsServiceDeRegister(&request_, nullptr);
        registered_ = false;
    }
    if (instance_) {
        DnsServiceFreeInstance(instance_);
        instance_ = nullptr;
    }
}

}  // namespace agent
