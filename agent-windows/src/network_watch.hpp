#pragma once
// 1.5 on Windows: the name of the network the PC is on (the Wi-Fi name, or the
// name Windows gives a wired network), for profiles that switch by network,
// and the file the profiles are kept in.

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "archon/server.hpp"

namespace agent {

// Name of the connected network Windows considers primary ("" when offline).
std::wstring current_network_name();

// Calls `changed` (from its own thread) with the network name at start and
// whenever it changes.
class NetworkWatch {
public:
    ~NetworkWatch() { stop(); }
    void start(std::function<void(const std::string&)> changed);
    void stop();

private:
    void run();
    std::function<void(const std::string&)> changed_;
    HANDLE stop_evt_ = nullptr;
    std::thread thread_;
};

// %APPDATA%\Archon\profiles.bin, written atomically.
class FileProfileStore final : public arc::ProfileStore {
public:
    bool load(arc::Bytes& data) override;
    bool save(const arc::Bytes& data) override;
};

}  // namespace agent
