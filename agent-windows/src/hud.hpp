#pragma once
// On-screen notice on the PC when a phone changes the sound ("iPhone · Spotify
// 40%", "Tudo silenciado"). Built never to get in the way:
//  - it never appears while a full-screen game, video or presentation is on
//    (Windows' own "busy" state, or the foreground window covering its monitor);
//  - it never takes focus or clicks (layered, click-through, no-activate);
//  - it lives ~1.6 s, at the bottom of the screen the pointer is on.

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

#include "archon/server.hpp"

namespace agent {

// True when the window in front covers its whole monitor (borderless or
// exclusive full screen) and is not the desktop itself.
bool foreground_is_fullscreen();

class Hud {
public:
    ~Hud();
    bool create(HINSTANCE inst);
    void destroy();
    void set_enabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    // UI thread. Shows (or updates) the notice unless something full-screen is in front.
    void show(const arc::ServerObserver::RemoteChange& change);
    // For tests: renders the notice for `change` into a PNG without showing anything.
    bool render_png(const arc::ServerObserver::RemoteChange& change, const std::wstring& path, float scale = 1.f);

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    bool draw(const arc::ServerObserver::RemoteChange& c, float scale, HDC dc, int w, int h);
    void present(BYTE alpha);
    void hide();

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    bool enabled_ = true;
    bool pt_ = true;
    bool reduce_motion_ = false;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw_;
    HDC mem_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
    SIZE size_{};
    POINT pos_{};
    int fade_ = 0;  // fade-out steps left
};

}  // namespace agent
