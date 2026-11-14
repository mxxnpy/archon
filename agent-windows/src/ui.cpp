#include "ui.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <ctime>
#include <memory>

#include "win_util.hpp"

namespace agent {

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_PAIR_REQUEST = WM_APP + 2;
constexpr UINT WM_APP_PAIR_DONE = WM_APP + 3;
constexpr UINT WM_APP_CONTROLLERS = WM_APP + 4;
constexpr UINT WM_APP_PANIC = WM_APP + 5;
constexpr UINT WM_APP_STREAMS = WM_APP + 6;
constexpr int kPanicHotkey = 1;  // Ctrl+Alt+M
constexpr UINT kTrayId = 1;

// Archon palette (docs/design.md): ink, paper, pastel yellow accent.
// Halo v2 tokens (docs/design.md).
constexpr COLORREF kInk = RGB(0x00, 0x00, 0x00);
constexpr COLORREF kSurface = RGB(0x11, 0x11, 0x11);
constexpr COLORREF kRaised = RGB(0x1C, 0x1C, 0x1C);
constexpr COLORREF kPaper = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kMuted = RGB(0x9A, 0x9A, 0x9A);
constexpr COLORREF kAccent = RGB(0xF6, 0xE7, 0xA1);
constexpr COLORREF kOk = RGB(0x6F, 0xD3, 0x9B);
constexpr COLORREF kTrack = RGB(0x26, 0x26, 0x26);
constexpr UINT kCodeTimeoutMs = 120000;  // matches ServerConfig::code_timeout_ms
constexpr UINT_PTR kCountdownTimer = 7;

Gdiplus::Color gp(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

// Ring drawn like the phone app: 270° from 135°, round caps, `fraction` lit.
void draw_ring(Gdiplus::Graphics& g, float x, float y, float d, float w, float fraction, COLORREF lit, float start = 135.f,
               float sweep = 270.f) {
    Gdiplus::Pen track(gp(kTrack), w), on(gp(lit), w);
    track.SetStartCap(Gdiplus::LineCapRound); track.SetEndCap(Gdiplus::LineCapRound);
    on.SetStartCap(Gdiplus::LineCapRound); on.SetEndCap(Gdiplus::LineCapRound);
    g.DrawArc(&track, x, y, d, d, start, sweep);
    if (fraction > 0.002f) g.DrawArc(&on, x, y, d, d, start, sweep * std::min(fraction, 1.f));
}

enum MenuId : UINT { ID_DEVICES = 100, ID_AUTOSTART, ID_OPEN_LOG, ID_UNLOCK, ID_EXIT, ID_PANIC };
enum CtrlId : int { IDC_REJECT = 200, IDC_LIST, IDC_REMOVE, IDC_CLOSE, IDC_TITLE = 300, IDC_WANTS, IDC_CODE, IDC_HINT };

struct PairRequest {
    uint64_t conn;
    std::wstring name;
    std::wstring code;
};
struct PairDone {
    uint64_t conn;
    bool ok;
    std::wstring name;
};

// ---- Strings (Portuguese when the Windows UI is Portuguese, else English) ----
struct Text {
    const wchar_t* app = L"Archon";
    const wchar_t* devices;
    const wchar_t* autostart;
    const wchar_t* open_log;
    const wchar_t* exit;
    const wchar_t* connected_fmt;  // %d
    const wchar_t* wants;          // %s
    const wchar_t* type_code;
    const wchar_t* reject;
    const wchar_t* paired_title;
    const wchar_t* paired_fmt;     // %s
    const wchar_t* col_name;
    const wchar_t* col_status;
    const wchar_t* col_paired;
    const wchar_t* col_seen;
    const wchar_t* online;
    const wchar_t* remove;
    const wchar_t* close;
    const wchar_t* address_fmt;    // %s %u
    const wchar_t* no_discovery;
    const wchar_t* first_title;
    const wchar_t* first_text;
    const wchar_t* confirm_remove; // %s
    const wchar_t* never;
    const wchar_t* unlock;
    const wchar_t* panic;        // menu item
    const wchar_t* panic_title;
    const wchar_t* panic_text;
    const wchar_t* listening;  // appended to the status line
};

const Text& T() {
    static const Text pt{L"Archon",
                         L"Dispositivos pareados…",
                         L"Iniciar com o Windows",
                         L"Abrir log",
                         L"Sair",
                         L"Archon — %d dispositivo(s) conectado(s)",
                         L"“%s” quer se conectar",
                         L"Digite no celular",
                         L"Recusar",
                         L"Dispositivo pareado",
                         L"%s agora controla o som deste PC.",
                         L"Dispositivo",
                         L"Status",
                         L"Pareado em",
                         L"Visto por último",
                         L"Conectado",
                         L"Remover",
                         L"Fechar",
                         L"Este PC: %s  ·  porta %u  (use para conectar manualmente)",
                         L"A descoberta automática (mDNS) não está disponível; conecte pelo endereço.",
                         L"Archon está rodando",
                         L"Abra o app Archon no celular, na mesma rede, para conectar.",
                         L"Remover “%s”? O aparelho precisará parear de novo.",
                         L"—",
                         L"Desbloquear pareamento",
                         L"Silenciar tudo",
                         L"Tudo silenciado",
                         L"Saídas e microfones foram silenciados. Use o mesmo atalho para voltar.",
                         L" · ouvindo no celular"};
    static const Text en{L"Archon",
                         L"Paired devices…",
                         L"Start with Windows",
                         L"Open log",
                         L"Exit",
                         L"Archon — %d device(s) connected",
                         L"“%s” wants to connect",
                         L"Type it on the phone",
                         L"Reject",
                         L"Device paired",
                         L"%s now controls this PC's sound.",
                         L"Device",
                         L"Status",
                         L"Paired",
                         L"Last seen",
                         L"Connected",
                         L"Remove",
                         L"Close",
                         L"This PC: %s  ·  port %u  (for manual connection)",
                         L"Automatic discovery (mDNS) is unavailable; connect by address.",
                         L"Archon is running",
                         L"Open the Archon app on your phone, on the same network, to connect.",
                         L"Remove “%s”? The device will need to pair again.",
                         L"—",
                         L"Unlock pairing",
                         L"Mute everything",
                         L"Everything muted",
                         L"Outputs and microphones are muted. Use the same shortcut to undo.",
                         L" · listening on a phone"};
    return is_portuguese_ui() ? pt : en;
}

std::wstring fmt(const wchar_t* f, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf_s(buf, _TRUNCATE, f, ap);
    va_end(ap);
    return buf;
}

std::wstring format_time(int64_t unix_s) {
    if (unix_s <= 0) return T().never;
    __time64_t t = unix_s;
    tm local{};
    if (_localtime64_s(&local, &t) != 0) return T().never;
    wchar_t buf[64];
    wcsftime(buf, 64, L"%d/%m/%Y %H:%M", &local);
    return buf;
}

// App/tray icon drawn at runtime (no binary assets): the Halo ring.
HICON make_icon(int size, COLORREF lit) {
    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        const float w = std::max(2.f, size * 0.14f);
        const float d = size - w - 1.f;
        draw_ring(g, (size - d) / 2.f, (size - d) / 2.f, d, w, 0.7f, lit);
    }
    HICON icon = nullptr;
    if (bmp.GetHICON(&icon) != Gdiplus::Ok || !icon) return LoadIcon(nullptr, IDI_APPLICATION);
    return icon;
}

int scale(HWND w, int v) { return MulDiv(v, int(GetDpiForWindow(w)), 96); }

HFONT make_font(UINT dpi, int pt, int weight) {
    return CreateFontW(-MulDiv(pt, int(dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

}  // namespace

// ---- lifecycle ---------------------------------------------------------------

bool AgentUi::create(HINSTANCE inst, bool first_run) {
    inst_ = inst;
    first_run_ = first_run;
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&gdiplus_, &gsi, nullptr);
    const int tray_px = GetSystemMetrics(SM_CXSMICON);  // 'small' is a macro in rpcndr.h
    icon_small_ = make_icon(tray_px, kAccent);
    icon_connected_ = make_icon(tray_px, kOk);
    icon_large_ = make_icon(GetSystemMetrics(SM_CXICON), kAccent);

    WNDCLASSEXW wc{sizeof wc};
    wc.hInstance = inst;
    wc.hIcon = icon_large_;
    wc.hIconSm = icon_small_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = kMainWindowClass;
    if (!RegisterClassExW(&wc)) return false;
    wc.lpfnWndProc = pair_proc;
    wc.lpszClassName = L"ArchonPair";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = devices_proc;
    wc.lpszClassName = L"ArchonDevices";
    RegisterClassExW(&wc);

    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    main_ = CreateWindowExW(0, kMainWindowClass, L"Archon", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, this);
    if (!main_) return false;
    // Panic mute from anywhere. Another app may own the combination: then the
    // tray item is the way in (logged, never fatal).
    hotkey_ok_ = RegisterHotKey(main_, kPanicHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M') != FALSE;
    if (!hotkey_ok_) log_line("Ctrl+Alt+M is taken by another app; panic mute stays in the tray menu");
    return true;
}

void AgentUi::attach(arc::Server* server, arc::TrustStore* store, uint16_t port, const std::wstring& pc_name,
                     bool discovery_ok) {
    server_ = server;
    store_ = store;
    port_ = port;
    pc_name_ = pc_name;
    discovery_ok_ = discovery_ok;
    add_tray();
    if (first_run_) balloon(T().first_title, T().first_text);
}

int AgentUi::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (devices_ && IsDialogMessageW(devices_, &msg)) continue;
        if (pair_ && IsDialogMessageW(pair_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return int(msg.wParam);
}

void AgentUi::destroy() {
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = main_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (pair_) DestroyWindow(pair_);
    if (devices_) DestroyWindow(devices_);
    if (main_ && hotkey_ok_) UnregisterHotKey(main_, kPanicHotkey);
    if (main_) DestroyWindow(main_);
    if (font_) DeleteObject(font_);
    if (font_code_) DeleteObject(font_code_);
    if (font_title_) DeleteObject(font_title_);
    if (surface_brush_) DeleteObject(surface_brush_);
    if (gdiplus_) Gdiplus::GdiplusShutdown(gdiplus_);
}

void AgentUi::show_error(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), L"Archon", MB_ICONERROR | MB_OK);
}

// ---- server observer (network thread -> UI thread) ----------------------------

void AgentUi::on_pair_request(uint64_t conn, const std::string& name, const std::string& code) {
    auto* p = new PairRequest{conn, to_wide(name), to_wide(code)};
    if (!PostMessageW(main_, WM_APP_PAIR_REQUEST, 0, reinterpret_cast<LPARAM>(p))) delete p;
}

void AgentUi::on_pair_finished(uint64_t conn, bool ok, const std::string& name) {
    auto* p = new PairDone{conn, ok, to_wide(name)};
    if (!PostMessageW(main_, WM_APP_PAIR_DONE, 0, reinterpret_cast<LPARAM>(p))) delete p;
}

void AgentUi::on_controllers_changed(const std::vector<arc::ConnectedController>& now) {
    auto* p = new std::vector<arc::ConnectedController>(now);
    if (!PostMessageW(main_, WM_APP_CONTROLLERS, 0, reinterpret_cast<LPARAM>(p))) delete p;
}

void AgentUi::on_log(const std::string& line) { log_line(line); }

void AgentUi::on_pairing_lockout(int lockouts, int64_t locked_until_unix) { save_lockout(lockouts, locked_until_unix); }

void AgentUi::on_panic_changed(bool active) { PostMessageW(main_, WM_APP_PANIC, active ? 1 : 0, 0); }

void AgentUi::on_streams_changed(size_t active) { PostMessageW(main_, WM_APP_STREAMS, WPARAM(active), 0); }

// ---- tray ---------------------------------------------------------------------

void AgentUi::add_tray() {
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = main_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = icon_small_;
    wcscpy_s(nid.szTip, fmt(T().connected_fmt, int(connected_.size())).c_str());
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void AgentUi::update_tray_tip() {
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = main_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP | NIF_ICON;
    // Ring turns green while a phone is connected.
    nid.hIcon = connected_.empty() ? icon_small_ : icon_connected_;
    std::wstring tip = fmt(T().connected_fmt, int(connected_.size()));
    if (streams_) tip += T().listening;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AgentUi::balloon(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = main_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AgentUi::show_menu() {
    HMENU m = CreatePopupMenu();
    std::wstring header = fmt(T().connected_fmt, int(connected_.size()));
    if (streams_) header += T().listening;
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, header.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_DEVICES, T().devices);
    const std::wstring panic_label = std::wstring(T().panic) + (hotkey_ok_ ? L"\tCtrl+Alt+M" : L"");
    AppendMenuW(m, MF_STRING | (panic_ ? MF_CHECKED : 0), ID_PANIC, panic_label.c_str());
    AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), ID_AUTOSTART, T().autostart);
    AppendMenuW(m, MF_STRING, ID_UNLOCK, T().unlock);  // clears a lockout caused by wrong codes
    AppendMenuW(m, MF_STRING, ID_OPEN_LOG, T().open_log);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, T().exit);
    SetMenuDefaultItem(m, ID_DEVICES, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(main_);
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, main_, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case ID_DEVICES: show_devices(); break;
        case ID_AUTOSTART: set_autostart(!autostart_enabled()); break;
        case ID_OPEN_LOG: {
            const std::wstring log = app_dir() + L"\\agent.log";
            ShellExecuteW(nullptr, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_UNLOCK:
            if (server_) server_->reset_pairing_lockout();
            break;
        case ID_PANIC:
            if (server_) server_->panic_toggle();
            break;
        case ID_EXIT: PostQuitMessage(0); break;
        default: break;
    }
}

LRESULT CALLBACK AgentUi::main_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<AgentUi*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->handle_main(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT AgentUi::handle_main(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == taskbar_created_ && taskbar_created_ != 0) {
        add_tray();  // Explorer restarted
        return 0;
    }
    switch (m) {
        case WM_APP_TRAY:
            switch (LOWORD(l)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    show_menu();
                    break;
                default:
                    break;
            }
            return 0;
        case WM_APP_SHOW_DEVICES:
            show_devices();
            return 0;
        case WM_HOTKEY:
            if (w == kPanicHotkey && server_) server_->panic_toggle();
            return 0;
        case WM_APP_STREAMS:
            streams_ = size_t(w);
            update_tray_tip();
            return 0;
        case WM_APP_PANIC:
            panic_ = w != 0;
            if (panic_) balloon(T().panic_title, T().panic_text);
            return 0;
        case WM_APP_PAIR_REQUEST: {
            std::unique_ptr<PairRequest> p(reinterpret_cast<PairRequest*>(l));
            show_pairing(p->conn, p->name, p->code);
            return 0;
        }
        case WM_APP_PAIR_DONE: {
            std::unique_ptr<PairDone> p(reinterpret_cast<PairDone*>(l));
            if (p->conn == pair_conn_) close_pairing();
            if (p->ok) balloon(T().paired_title, fmt(T().paired_fmt, p->name.c_str()));
            if (devices_) fill_devices();
            return 0;
        }
        case WM_APP_CONTROLLERS: {
            std::unique_ptr<std::vector<arc::ConnectedController>> p(
                reinterpret_cast<std::vector<arc::ConnectedController>*>(l));
            connected_ = *p;
            update_tray_tip();
            if (devices_) fill_devices();
            return 0;
        }
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

// ---- pairing window ---------------------------------------------------------------

void AgentUi::show_pairing(uint64_t conn, const std::wstring& name, const std::wstring& code) {
    close_pairing();
    pair_conn_ = conn;
    pair_name_ = name;
    pair_code_ = code.size() == 6 ? code.substr(0, 3) + L" " + code.substr(3) : code;
    pair_started_ = GetTickCount64();

    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    // Borderless card (Windows 11 rounds it and keeps the system shadow).
    pair_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ArchonPair", L"Archon", WS_POPUP | WS_CLIPCHILDREN,
                            0, 0, 100, 100, nullptr, nullptr, inst_, this);
    if (!pair_) return;
    const DWM_WINDOW_CORNER_PREFERENCE round = DWMWCP_ROUND;
    DwmSetWindowAttribute(pair_, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof round);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(pair_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    const int w = scale(pair_, 340), hgt = scale(pair_, 212), margin = scale(pair_, 12);
    SetWindowPos(pair_, HWND_TOPMOST, work.right - w - margin, work.bottom - hgt - margin, w, hgt, 0);
    const int bw = scale(pair_, 96), bh = scale(pair_, 32), pad = scale(pair_, 20);
    HWND btn = CreateWindowExW(0, L"BUTTON", T().reject, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                               w - bw - pad, hgt - bh - pad, bw, bh, pair_,
                               reinterpret_cast<HMENU>(INT_PTR(IDC_REJECT)), inst_, nullptr);
    const UINT dpi = GetDpiForWindow(pair_);
    if (font_dpi_ != dpi || !font_) {
        if (font_) DeleteObject(font_);
        if (font_code_) DeleteObject(font_code_);
        if (font_title_) DeleteObject(font_title_);
        font_ = make_font(dpi, 10, FW_NORMAL);
        font_title_ = make_font(dpi, 11, FW_SEMIBOLD);
        font_code_ = make_font(dpi, 27, FW_SEMIBOLD);  // 36 px at 96 dpi
        font_dpi_ = dpi;
    }
    SendMessageW(btn, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    // Real text controls (not painted text) so Narrator and other screen
    // readers can read the request and the code.
    if (!surface_brush_) surface_brush_ = CreateSolidBrush(kSurface);
    const std::wstring wants = fmt(T().wants, pair_name_.c_str());
    auto label = [&](int id, const std::wstring& text, int x, int y, int cw, int ch, HFONT f) {
        HWND c = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX |
                                 SS_CENTERIMAGE | SS_ENDELLIPSIS, x, y, cw, ch, pair_,
                                 reinterpret_cast<HMENU>(INT_PTR(id)), inst_, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    };
    label(IDC_TITLE, L"Archon", pad + scale(pair_, 28), pad - scale(pair_, 2), w - 2 * pad - scale(pair_, 28),
          scale(pair_, 22), font_title_);
    label(IDC_WANTS, wants, pad, pad + scale(pair_, 34), w - 2 * pad, scale(pair_, 20), font_);
    label(IDC_CODE, pair_code_, pad, pad + scale(pair_, 58), w - 2 * pad, scale(pair_, 54), font_code_);
    label(IDC_HINT, T().type_code, pad, hgt - pad - bh, w - 2 * pad - bw - scale(pair_, 8), bh, font_);
    // Window name read first by Narrator when the popup takes focus.
    SetWindowTextW(pair_, (L"Archon. " + wants + L". " + pair_code_).c_str());
    SetTimer(pair_, kCountdownTimer, 250, nullptr);
    ShowWindow(pair_, SW_SHOWNORMAL);
    SetForegroundWindow(pair_);
    MessageBeep(MB_ICONINFORMATION);
}

void AgentUi::close_pairing() {
    if (pair_) {
        HWND w = pair_;
        pair_ = nullptr;
        DestroyWindow(w);
    }
    pair_conn_ = 0;
    SecureZeroMemory(pair_code_.data(), pair_code_.size() * sizeof(wchar_t));
    pair_code_.clear();
}

LRESULT CALLBACK AgentUi::pair_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<AgentUi*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, w, l);
    switch (m) {
        case WM_ERASEBKGND:
            return 1;  // painted in WM_PAINT (double-buffered)
        case WM_TIMER:
            if (w == kCountdownTimer) InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC wdc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HDC dc = CreateCompatibleDC(wdc);
            HBITMAP bm = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
            HGDIOBJ oldbm = SelectObject(dc, bm);
            HBRUSH bg = CreateSolidBrush(kSurface);
            FillRect(dc, &rc, bg);
            DeleteObject(bg);

            const int pad = scale(h, 20);
            // Header: countdown ring (depletes over the code lifetime) + "Archon".
            const float left = float(GetTickCount64() - self->pair_started_) / float(kCodeTimeoutMs);
            {
                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                const float d = float(scale(h, 16)), sw = float(std::max(2, scale(h, 2)));
                draw_ring(g, float(pad) + sw / 2, float(pad) + sw / 2, d, sw, 1.f - std::min(left, 1.f), kAccent, -90.f, 360.f);
            }
            // Texts are STATIC child controls (accessible); only the ring is painted here.

            BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, oldbm);
            DeleteObject(bm);
            DeleteDC(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, (id == IDC_WANTS || id == IDC_HINT) ? kMuted : kPaper);
            SetBkColor(dc, kSurface);
            return reinterpret_cast<LRESULT>(self->surface_brush_);
        }
        case WM_DRAWITEM: {
            // Pill button: raised fill, white label; accent outline when focused.
            auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            HBRUSH sb = CreateSolidBrush(kSurface);
            FillRect(di->hDC, &di->rcItem, sb);
            DeleteObject(sb);
            const bool pressed = (di->itemState & ODS_SELECTED) != 0;
            const bool focus = (di->itemState & ODS_FOCUS) != 0;
            {
                Gdiplus::Graphics g(di->hDC);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                const float x = float(di->rcItem.left) + 1, y = float(di->rcItem.top) + 1;
                const float wd = float(di->rcItem.right - di->rcItem.left) - 2, ht = float(di->rcItem.bottom - di->rcItem.top) - 2;
                Gdiplus::GraphicsPath path;
                path.AddArc(x, y, ht, ht, 90, 180);
                path.AddArc(x + wd - ht, y, ht, ht, 270, 180);
                path.CloseFigure();
                Gdiplus::SolidBrush fill(gp(pressed ? kTrack : kRaised));
                g.FillPath(&fill, &path);
                if (focus) {
                    Gdiplus::Pen ring(gp(kAccent), 1.5f);
                    g.DrawPath(&ring, &path);
                }
            }
            SetBkMode(di->hDC, TRANSPARENT);
            SetTextColor(di->hDC, kPaper);
            HGDIOBJ of = SelectObject(di->hDC, self->font_);
            DrawTextW(di->hDC, T().reject, -1, &di->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(di->hDC, of);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(w) == IDC_REJECT && self->server_ && self->pair_conn_) {
                self->server_->reject_pairing(self->pair_conn_);
                self->close_pairing();
            }
            return 0;
        case WM_CLOSE:
            // Closing the window is a rejection: the code must not stay valid unseen.
            if (self->server_ && self->pair_conn_) self->server_->reject_pairing(self->pair_conn_);
            self->close_pairing();
            return 0;
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
            return 0;
        }
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

// ---- paired devices window ---------------------------------------------------------

void AgentUi::show_devices() {
    if (devices_) {
        ShowWindow(devices_, SW_SHOWNORMAL);
        SetForegroundWindow(devices_);
        fill_devices();
        return;
    }
    devices_ = CreateWindowExW(0, L"ArchonDevices", T().devices, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, 100, 100, nullptr, nullptr, inst_, this);
    if (!devices_) return;
    SetWindowPos(devices_, nullptr, 0, 0, scale(devices_, 640), scale(devices_, 400), SWP_NOMOVE | SWP_NOZORDER);
    const UINT dpi = GetDpiForWindow(devices_);
    if (font_dpi_ != dpi || !font_) {
        if (font_) DeleteObject(font_);
        if (font_code_) DeleteObject(font_code_);
        font_ = make_font(dpi, 10, FW_NORMAL);
        font_code_ = make_font(dpi, 34, FW_SEMIBOLD);
        font_dpi_ = dpi;
    }
    info_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, devices_, nullptr,
                            inst_, nullptr);
    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0,
                            10, 10, devices_, reinterpret_cast<HMENU>(INT_PTR(IDC_LIST)), inst_, nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    btn_remove_ = CreateWindowExW(0, L"BUTTON", T().remove, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0,
                                  10, 10, devices_, reinterpret_cast<HMENU>(INT_PTR(IDC_REMOVE)), inst_, nullptr);
    btn_close_ = CreateWindowExW(0, L"BUTTON", T().close, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0,
                                 0, 10, 10, devices_, reinterpret_cast<HMENU>(INT_PTR(IDC_CLOSE)), inst_, nullptr);
    for (HWND c : {info_, list_, btn_remove_, btn_close_})
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    const wchar_t* cols[] = {T().col_name, T().col_status, T().col_paired, T().col_seen};
    const int widths[] = {200, 100, 140, 140};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        c.cx = scale(devices_, widths[i]);
        ListView_InsertColumn(list_, i, &c);
    }
    layout_devices();
    fill_devices();
    ShowWindow(devices_, SW_SHOWNORMAL);
    SetForegroundWindow(devices_);
}

void AgentUi::layout_devices() {
    if (!devices_) return;
    RECT rc;
    GetClientRect(devices_, &rc);
    const int m = scale(devices_, 12), bh = scale(devices_, 30), bw = scale(devices_, 100), ih = scale(devices_, 40);
    MoveWindow(info_, m, m, rc.right - 2 * m, ih, TRUE);
    MoveWindow(list_, m, m + ih, rc.right - 2 * m, rc.bottom - ih - bh - 3 * m, TRUE);
    MoveWindow(btn_remove_, m, rc.bottom - bh - m, bw, bh, TRUE);
    MoveWindow(btn_close_, rc.right - bw - m, rc.bottom - bh - m, bw, bh, TRUE);
}

void AgentUi::fill_devices() {
    if (!devices_ || !store_) return;
    std::wstring addrs;
    for (auto& a : local_addresses()) addrs += (addrs.empty() ? L"" : L", ") + to_wide(a);
    std::wstring info = fmt(T().address_fmt, (pc_name_ + L" (" + addrs + L")").c_str(), unsigned(port_));
    if (!discovery_ok_) info += L"\n" + std::wstring(T().no_discovery);
    SetWindowTextW(info_, info.c_str());

    listed_ = store_->list();
    ListView_DeleteAllItems(list_);
    for (size_t i = 0; i < listed_.size(); ++i) {
        const auto& d = listed_[i];
        bool online = false;
        for (auto& c : connected_) online = online || c.id == d.id;
        std::wstring name = to_wide(d.name);
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = int(i);
        it.pszText = name.data();
        ListView_InsertItem(list_, &it);
        std::wstring status = online ? T().online : T().never;
        std::wstring paired = format_time(d.paired_at);
        std::wstring seen = online ? std::wstring(T().online) : format_time(d.last_seen);
        ListView_SetItemText(list_, int(i), 1, status.data());
        ListView_SetItemText(list_, int(i), 2, paired.data());
        ListView_SetItemText(list_, int(i), 3, seen.data());
    }
    EnableWindow(btn_remove_, !listed_.empty());
}

void AgentUi::remove_selected_device() {
    const int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (sel < 0 || size_t(sel) >= listed_.size() || !server_) return;
    const auto dev = listed_[size_t(sel)];
    const std::wstring q = fmt(T().confirm_remove, to_wide(dev.name).c_str());
    if (MessageBoxW(devices_, q.c_str(), L"Archon", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    server_->revoke(dev.id);  // removes from the store and disconnects it
    log_line("pairing removed by user: " + dev.name);
    // The revoke runs on the server thread; refresh shortly after.
    SetTimer(devices_, 1, 300, nullptr);
}

LRESULT CALLBACK AgentUi::devices_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<AgentUi*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, w, l);
    switch (m) {
        case WM_SIZE:
            self->layout_devices();
            return 0;
        case WM_TIMER:
            KillTimer(h, 1);
            self->fill_devices();
            return 0;
        case WM_COMMAND:
            if (LOWORD(w) == IDC_REMOVE) self->remove_selected_device();
            if (LOWORD(w) == IDC_CLOSE || LOWORD(w) == IDCANCEL) DestroyWindow(h);
            return 0;
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            self->devices_ = self->list_ = self->info_ = self->btn_remove_ = self->btn_close_ = nullptr;
            return 0;
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

}  // namespace agent
