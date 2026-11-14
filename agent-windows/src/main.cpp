// Archon agent for Windows: tray app that exposes this PC's audio mixer to
// paired controllers on the local network.

#include <windows.h>
#include <objbase.h>

#include "archon/bytes.hpp"
#include "archon/server.hpp"
#include "archon/trust_store.hpp"
#include "mdns.hpp"
#include "ui.hpp"
#include "loopback_stream.hpp"
#include "wasapi_backend.hpp"
#include "win_util.hpp"

using namespace agent;

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // One agent per user session; a second launch opens the devices window.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\ArchonAgent");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(kMainWindowClass, nullptr)) PostMessageW(w, WM_APP_SHOW_DEVICES, 0, 0);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Config cfg = load_config();
    if (cfg.first_run) set_autostart(true);
    // Where the state lives and which identity was loaded: phones resume by this id.
    log_line("agent starting (data: " + to_utf8(app_dir()) + ", id " + arc::to_hex(cfg.agent_id).substr(0, 8) +
             (cfg.first_run ? ", new" : "") + ")");

    const std::wstring store_path = app_dir() + L"\\paired.bin";
    arc::FileTrustStore store(to_utf8(store_path), dpapi_protect, dpapi_unprotect);
    std::string err;
    if (!store.load(&err)) {
        // Unreadable (e.g. profile restored on another machine): keep a copy, start fresh.
        log_line("trust store unreadable (" + err + "); starting with no paired devices");
        const std::wstring bad = store_path + L".bad";
        MoveFileExW(store_path.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING);
        store.load(nullptr);
    }

    AgentUi ui;
    if (!ui.create(inst, cfg.first_run)) return 1;

    const std::wstring pc_name = computer_name();
    // Phones show this on the radar: the user's name is short and friendly;
    // the computer name stays in the devices window for manual connection.
    const std::wstring shown_name = display_name();
    WasapiBackend backend;
    LoopbackStreams streams;  // "listen on the phone"
    arc::ServerConfig scfg;
    scfg.agent_id = cfg.agent_id;
    scfg.agent_name = to_utf8(shown_name);
    scfg.port = cfg.port;
    scfg.initial_lockouts = cfg.lockouts;
    scfg.locked_until_unix = cfg.locked_until;
    auto server = std::make_unique<arc::Server>(scfg, backend, store, ui);
    server->set_stream_service(&streams);
    if (!server->start(&err)) {
        // Port taken by something else: fall back to any free port. Phones
        // learn the port through mDNS, so discovery keeps working.
        log_line("cannot listen on port " + std::to_string(cfg.port) + ": " + err + "; using a free port");
        scfg.port = 0;
        server = std::make_unique<arc::Server>(scfg, backend, store, ui);
        server->set_stream_service(&streams);
        if (!server->start(&err)) {
            log_line("server failed: " + err);
            ui.show_error(L"Archon não conseguiu iniciar / could not start:\n" + to_wide(err));
            ui.destroy();
            return 1;
        }
    }
    streams.attach(server.get());
    log_line("listening on port " + std::to_string(server->port()));

    MdnsAdvertiser mdns;
    const bool discovery_ok = mdns.start(shown_name, server->port(), arc::to_hex(cfg.agent_id), &err);
    if (!discovery_ok) log_line("mDNS unavailable: " + err);

    ui.attach(server.get(), &store, server->port(), pc_name, discovery_ok);
    const int rc = ui.run();

    log_line("agent stopping");
    mdns.stop();
    server->stop();
    streams.attach(nullptr);  // a capture thread must not report to a server being destroyed
    ui.destroy();
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return rc;
}
