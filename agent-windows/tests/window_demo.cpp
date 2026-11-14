// The Archon window with a made-up mixer, for screenshots and manual checks.
// Runs the real server (loopback only) and window over a fake audio backend;
// it is not the agent and touches no device.
//   archon_window_demo                         -> interactive
//   archon_window_demo --shot out.png [--panel profiles|settings|editor]
//                      [--size 960x680] [--focus N]   -> one screenshot, then exit

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "archon/profiles.hpp"
#include "archon/server.hpp"
#include "mixer_window.hpp"
#include "win_util.hpp"

using namespace agent;
using Microsoft::WRL::ComPtr;

namespace {

class FakeMixer : public arc::AudioBackend {
public:
    std::mutex mu;
    arc::AudioSink* sink = nullptr;
    std::map<std::string, arc::EndpointInfo> eps;
    std::map<std::string, arc::SourceInfo> srcs;
    FakeMixer() {
        auto ep = [&](const char* id, const char* name, uint16_t v, bool def, bool input = false, bool muted = false) {
            arc::EndpointInfo e;
            e.id = id;
            e.name = name;
            e.volume = v;
            e.is_default = def && !input;
            e.input = input;
            e.is_default_input = def && input;
            e.muted = muted;
            eps[id] = e;
        };
        ep("e1", "Alto-falantes (Realtek Audio)", 6400, true);
        ep("e2", "Fone Bluetooth", 5000, false);
        ep("m1", "Microfone (USB)", 7500, true, true);
        auto src = [&](const char* id, const char* name, uint16_t v, bool playing, bool muted = false,
                       bool system = false, const char* route = "") {
            arc::SourceInfo s;
            s.id = id;
            s.endpoint_id = "e1";
            s.name = name;
            s.volume = v;
            s.playing = playing;
            s.muted = muted;
            s.system_sounds = system;
            s.route = route;
            srcs[id] = s;
        };
        src("s1", "Spotify", 7200, true);
        src("s2", "Discord", 8000, true, false, false, "e1");
        src("s3", "Counter-Strike 2", 5800, false);
        src("s4", "Brave", 4500, false, true);
        src("s5", "System sounds", 4000, false, false, true);
        auto tab = [&](const char* id, const char* title, const char* site, uint16_t v, bool playing, bool muted) {
            arc::SourceInfo s;
            s.id = id;
            s.endpoint_id = "e1";
            s.name = title;
            s.detail = site;
            s.group = "Brave";
            s.tab = true;
            s.volume = v;
            s.playing = playing;
            s.muted = muted;
            srcs[id] = s;
        };
        tab("tab:1:1", "lofi hip hop radio - beats to relax/study to", "youtube.com", 6000, true, false);
        tab("tab:1:2", "Podcast - Episódio 42", "open.spotify.com", 10000, true, false);
        tab("tab:1:3", "Reunião da equipe", "meet.google.com", 8000, false, true);
    }
    bool start(arc::AudioSink& s, std::string*) override {
        std::lock_guard<std::mutex> lk(mu);
        sink = &s;
        for (auto& [id, e] : eps) s.endpoint_upsert(e);
        for (auto& [id, x] : srcs) s.source_upsert(x);
        return true;
    }
    void stop() override {
        std::lock_guard<std::mutex> lk(mu);
        sink = nullptr;
    }
    void set_endpoint_volume(const std::string& id, uint16_t v) override {
        std::lock_guard<std::mutex> lk(mu);
        if (eps.count(id) && sink) eps[id].volume = v, sink->endpoint_upsert(eps[id]);
    }
    void set_endpoint_mute(const std::string& id, bool m) override {
        std::lock_guard<std::mutex> lk(mu);
        if (eps.count(id) && sink) eps[id].muted = m, sink->endpoint_upsert(eps[id]);
    }
    void set_source_volume(const std::string& id, uint16_t v) override {
        std::lock_guard<std::mutex> lk(mu);
        if (srcs.count(id) && sink) srcs[id].volume = v, sink->source_upsert(srcs[id]);
    }
    void set_source_mute(const std::string& id, bool m) override {
        std::lock_guard<std::mutex> lk(mu);
        if (srcs.count(id) && sink) srcs[id].muted = m, sink->source_upsert(srcs[id]);
    }
    void set_source_route(const std::string& id, const std::string& ep) override {
        std::lock_guard<std::mutex> lk(mu);
        if (srcs.count(id) && sink) srcs[id].route = ep, sink->source_upsert(srcs[id]);
    }
    bool icon_png(const std::string&, arc::Bytes&) override { return false; }
    uint32_t features() const override { return arc::kFeatureInputs | arc::kFeatureRouting | arc::kFeatureTabs; }
    std::string os_name() const override { return "windows"; }
};

class MemStore : public arc::TrustStore {
public:
    std::map<arc::Id16, arc::PairedController> items;
    std::optional<arc::PairedController> find(const arc::Id16& id) override {
        auto it = items.find(id);
        if (it == items.end()) return std::nullopt;
        return it->second;
    }
    bool put(const arc::PairedController& c) override {
        items[c.id] = c;
        return true;
    }
    bool remove(const arc::Id16& id) override { return items.erase(id) > 0; }
    std::vector<arc::PairedController> list() override {
        std::vector<arc::PairedController> v;
        for (auto& [k, c] : items) v.push_back(c);
        return v;
    }
};

class MemProfiles : public arc::ProfileStore {
public:
    arc::Bytes blob;
    bool load(arc::Bytes& d) override {
        d = blob;
        return !blob.empty();
    }
    bool save(const arc::Bytes& d) override {
        blob = d;
        return true;
    }
};

bool save_png(HWND h, const std::wstring& path) {
    RECT rc;
    GetClientRect(h, &rc);
    HDC win = GetDC(h);
    HDC mem = CreateCompatibleDC(win);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), rc.right, -rc.bottom, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(win, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(h, mem, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
    SelectObject(mem, old);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> wb;
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &wb)) &&
              SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
              SUCCEEDED(fr->WriteSource(wb.Get(), nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(h, win);
    return ok;
}

void pump(DWORD ms) {
    const DWORD until = GetTickCount() + ms;
    MSG m;
    while (GetTickCount() < until) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring shot, panel;
    int w = 960, h = 680, tabs = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == L"--panel" && i + 1 < argc) panel = argv[++i];
        else if (a == L"--size" && i + 1 < argc) swscanf_s(argv[++i], L"%dx%d", &w, &h);
        else if (a == L"--focus" && i + 1 < argc) tabs = _wtoi(argv[++i]);
    }
    FakeMixer mixer;
    MemStore store;
    for (const char* name : {"iPhone de Ayu", "Pixel 8"}) {
        arc::PairedController pc;
        pc.id = arc::random_array<16>();
        pc.name = name;
        store.put(pc);
    }
    MemProfiles profiles;
    {
        arc::Profile work;
        work.name = "Trabalho";
        work.id = "0000000000000001";
        work.automatic = true;
        work.restore = true;
        work.days = 0b0111110;
        work.start_min = 0;
        work.end_min = 0;
        work.network = "MaHe-Casa-5G";
        arc::Profile phones;
        phones.name = "Fones";
        phones.id = "0000000000000002";
        phones.automatic = true;
        phones.output_id = "e2";
        phones.output_name = "Fone Bluetooth";
        arc::Profile night;
        night.name = "Noite";
        night.id = "0000000000000003";
        night.automatic = true;
        night.restore = true;
        night.start_min = 22 * 60;
        night.end_min = 7 * 60;
        arc::Profile live;
        live.name = "Live";
        live.id = "0000000000000004";
        profiles.blob = arc::serialize_profiles({work, phones, night, live});
    }
    arc::ServerConfig cfg;
    cfg.agent_id = arc::random_array<16>();
    cfg.agent_name = "PC";
    cfg.loopback_only = true;
    cfg.port = 0;
    arc::ServerObserver obs;
    arc::Server server(cfg, mixer, store, obs);
    server.set_profile_store(&profiles);
    std::string err;
    if (!server.start(&err)) {
        std::printf("server: %s\n", err.c_str());
        return 1;
    }
    server.network_changed("MaHe-Casa-5G");
    MixerWindow win;
    WindowHost host;
    host.paired = [&] { return store.list(); };
    host.forget = [&](const arc::Id16& id) { store.remove(id); };
    static bool autostart = true;
    host.autostart = [] { return autostart; };
    host.set_autostart = [](bool on) { autostart = on; };
    host.browsers = [] { return 1; };
    host.hotkey = true;
    win.create(GetModuleHandleW(nullptr), &server, host);
    // Screenshots never take focus or appear on screen: the window is shown
    // inactive, off the visible desktop (a game in use keeps the focus).
    if (!shot.empty()) {
        win.show(false);
        SetWindowLongPtrW(win.hwnd(), GWL_EXSTYLE,
                          GetWindowLongPtrW(win.hwnd(), GWL_EXSTYLE) | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    } else {
        win.show();
    }
    win.set_connected(2);
    win.set_streams(1);
    const UINT dpi = GetDpiForWindow(win.hwnd());
    RECT r{0, 0, MulDiv(w, int(dpi), 96), MulDiv(h, int(dpi), 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int px = shot.empty() ? 40 : vx - (r.right - r.left) - 200;  // off-screen for screenshots
    SetWindowPos(win.hwnd(), shot.empty() ? nullptr : HWND_BOTTOM, px, shot.empty() ? 40 : vy, r.right - r.left,
                 r.bottom - r.top, (shot.empty() ? SWP_NOZORDER : 0) | SWP_NOACTIVATE);
    pump(2500);  // dump + the profile engine's first look (1.5 s)
    if (!panel.empty()) {
        win.open_named(std::string(panel.begin(), panel.end()));
        pump(500);
    }
    for (int i = 0; i < tabs; ++i) {
        SendMessageW(win.hwnd(), WM_KEYDOWN, VK_TAB, 0);
        pump(30);
    }
    if (!shot.empty()) {
        pump(300);
        const bool ok = save_png(win.hwnd(), shot);
        std::printf(ok ? "saved\n" : "screenshot failed\n");
        win.destroy();
        server.stop();
        return ok ? 0 : 1;
    }
    MSG m;
    while (IsWindowVisible(win.hwnd()) && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    win.destroy();
    server.stop();
    return 0;
}
