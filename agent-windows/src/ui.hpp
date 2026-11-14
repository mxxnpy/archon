#pragma once
// Tray icon, pairing-code window and paired-devices window (plain Win32).
// Server callbacks arrive on the network thread and are marshalled to the UI
// thread with PostMessage.

#include <windows.h>

#include <string>
#include <vector>

#include "archon/server.hpp"

namespace agent {

constexpr const wchar_t* kMainWindowClass = L"ArchonAgentMain";
constexpr UINT WM_APP_SHOW_DEVICES = WM_APP + 10;

class AgentUi final : public arc::ServerObserver {
public:
    bool create(HINSTANCE inst, bool first_run);
    void attach(arc::Server* server, arc::TrustStore* store, uint16_t port, const std::wstring& pc_name,
                bool discovery_ok);
    int run();
    void destroy();

    // arc::ServerObserver (network thread)
    void on_pair_request(uint64_t conn_id, const std::string& name, const std::string& code) override;
    void on_pair_finished(uint64_t conn_id, bool ok, const std::string& name) override;
    void on_controllers_changed(const std::vector<arc::ConnectedController>& now) override;
    void on_log(const std::string& line) override;
    void on_pairing_lockout(int lockouts, int64_t locked_until_unix) override;
    void on_panic_changed(bool active) override;

    void show_error(const std::wstring& text);

private:
    static LRESULT CALLBACK main_proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK pair_proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK devices_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_main(HWND, UINT, WPARAM, LPARAM);

    void add_tray();
    void update_tray_tip();
    void balloon(const std::wstring& title, const std::wstring& text);
    void show_menu();
    void show_pairing(uint64_t conn, const std::wstring& name, const std::wstring& code);
    void close_pairing();
    void show_devices();
    void fill_devices();
    void remove_selected_device();
    void layout_devices();

    HINSTANCE inst_ = nullptr;
    HWND main_ = nullptr;
    HWND pair_ = nullptr;
    HWND devices_ = nullptr;
    HWND list_ = nullptr;
    HWND info_ = nullptr;
    HWND btn_remove_ = nullptr;
    HWND btn_close_ = nullptr;
    HICON icon_small_ = nullptr;
    HICON icon_connected_ = nullptr;
    ULONG_PTR gdiplus_ = 0;
    ULONGLONG pair_started_ = 0;
    HFONT font_title_ = nullptr;
    HBRUSH surface_brush_ = nullptr;
    HICON icon_large_ = nullptr;
    UINT taskbar_created_ = 0;
    bool first_run_ = false;
    bool panic_ = false;     // everything muted by the panic mute
    bool hotkey_ok_ = false;  // Ctrl+Alt+M registered

    arc::Server* server_ = nullptr;
    arc::TrustStore* store_ = nullptr;
    uint16_t port_ = 0;
    std::wstring pc_name_;
    bool discovery_ok_ = true;

    // Pairing window state
    uint64_t pair_conn_ = 0;
    std::wstring pair_name_;
    std::wstring pair_code_;
    HFONT font_ = nullptr;
    HFONT font_code_ = nullptr;
    UINT font_dpi_ = 0;

    std::vector<arc::ConnectedController> connected_;
    std::vector<arc::PairedController> listed_;
};

}  // namespace agent
