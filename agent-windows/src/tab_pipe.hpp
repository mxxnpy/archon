#pragma once
// 1.4 on Windows: the agent's end of the browser-extension bridge. Accepts
// archon-tabs-host.exe connections on a pipe only this user can open (and only
// from the host next to Archon.exe), and feeds TabSources. Also registers the
// host with the installed browsers.

#include <windows.h>

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "tab_sources.hpp"

namespace agent {

class TabPipeServer {
public:
    explicit TabPipeServer(TabSources& tabs);  // out of line: Conn is defined in the .cpp
    ~TabPipeServer();
    bool start(std::string* err);
    void stop();

private:
    struct Conn;
    void accept_loop();
    void serve(Conn* c);

    HANDLE create_instance(bool first);

    TabSources& tabs_;
    std::wstring name_;
    std::wstring host_path_;
    PSECURITY_DESCRIPTOR sd_ = nullptr;
    HANDLE first_ = INVALID_HANDLE_VALUE;  // created in start(), before the name is published
    HANDLE stop_evt_ = nullptr;
    std::thread acceptor_;
    std::mutex mu_;
    std::list<std::unique_ptr<Conn>> conns_;
};

// Writes the native-messaging manifests next to the agent's data and points
// Chrome, Edge, Brave, Chromium, Vivaldi and Firefox (current user) at them.
// Returns false (with a reason) when archon-tabs-host.exe is missing.
bool register_tabs_host(std::string* err);
// Removes those registrations (agent uninstall / "forget").
void unregister_tabs_host();

}  // namespace agent
