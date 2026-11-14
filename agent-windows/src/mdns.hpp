#pragma once
// DNS-SD advertisement through the Windows built-in mDNS responder
// (DnsServiceRegister, Windows 10 1809+). No Bonjour install required.

#include <windows.h>
#include <windns.h>

#include <string>

namespace agent {

class MdnsAdvertiser {
public:
    ~MdnsAdvertiser();
    // instance: shown on phones (computer name). Returns false if the OS API
    // is unavailable or registration fails synchronously.
    bool start(const std::wstring& instance, uint16_t port, const std::string& agent_id_hex, std::string* err);
    void stop();

private:
    DNS_SERVICE_INSTANCE* instance_ = nullptr;
    DNS_SERVICE_REGISTER_REQUEST request_{};
    bool registered_ = false;
};

}  // namespace agent
