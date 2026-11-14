#include "hud.hpp"

#include <wincodec.h>

#include <algorithm>
#include <cmath>

#include "win_util.hpp"

using Microsoft::WRL::ComPtr;

namespace agent {

namespace {

constexpr wchar_t kClass[] = L"ArchonHud";
constexpr UINT_PTR kHideTimer = 1, kFadeTimer = 2;
constexpr float kW = 340, kH = 76;  // DIPs
constexpr float kPi = 3.14159265f;

D2D1_COLOR_F rgb(uint32_t v, float a = 1.f) {
    return D2D1::ColorF(float((v >> 16) & 255) / 255.f, float((v >> 8) & 255) / 255.f, float(v & 255) / 255.f, a);
}

std::wstring text_of(const arc::ServerObserver::RemoteChange& c, bool pt, std::wstring& sub) {
    using K = arc::ServerObserver::RemoteChange::Kind;
    const std::wstring who = to_wide(c.controller);
    const std::wstring pct = std::to_wstring((c.volume * 100 + 5000) / 10000) + L"%";
    switch (c.kind) {
        case K::Panic:
            sub = who;
            return c.on ? (pt ? L"Tudo silenciado" : L"Everything muted") : (pt ? L"Som do PC de volta" : L"PC sound back on");
        case K::Profile:
            sub = who;
            return (pt ? L"Perfil: " : L"Profile: ") + to_wide(c.target);
        case K::Mute:
            sub = who + L" \x00B7 " + (c.on ? (pt ? L"mudo" : L"muted") : (pt ? L"som de volta" : L"unmuted"));
            break;
        case K::Volume:
            sub = who + L" \x00B7 " + pct;
            break;
    }
    if (c.system) return pt ? L"Sons do sistema" : L"System sounds";
    return to_wide(c.target);
}

}  // namespace

bool foreground_is_fullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == GetShellWindow() || fg == GetDesktopWindow()) return false;
    wchar_t cls[64] = {0};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Progman")) return false;  // the desktop
    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right &&
           wr.bottom >= mi.rcMonitor.bottom;
}

Hud::~Hud() { destroy(); }

bool Hud::create(HINSTANCE inst) {
    inst_ = inst;
    pt_ = is_portuguese_ui();
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dw_.GetAddressOf()))))
        return false;
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    // Layered (per-pixel alpha), always on top, click-through, never activated, no taskbar button.
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                            kClass, L"Archon", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
    return hwnd_ != nullptr;
}

void Hud::destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (mem_) {
        SelectObject(mem_, old_);
        DeleteDC(mem_);
        mem_ = nullptr;
    }
    if (bmp_) DeleteObject(bmp_);
    bmp_ = nullptr;
}

LRESULT CALLBACK Hud::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    }
    auto* self = reinterpret_cast<Hud*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && m == WM_TIMER) {
        if (w == kHideTimer) {
            KillTimer(h, kHideTimer);
            if (self->reduce_motion_) {
                self->hide();
            } else {
                self->fade_ = 8;  // 8 × 20 ms
                SetTimer(h, kFadeTimer, 20, nullptr);
            }
        } else if (w == kFadeTimer) {
            if (--self->fade_ <= 0 || foreground_is_fullscreen()) {
                self->hide();
            } else {
                self->present(BYTE(255 * self->fade_ / 8));
            }
        }
        return 0;
    }
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

bool Hud::draw(const arc::ServerObserver::RemoteChange& c, float s, HDC dc, int w, int h) {
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f * s, 96.f * s);
    ComPtr<ID2D1DCRenderTarget> rt;
    if (FAILED(d2d_->CreateDCRenderTarget(&props, &rt))) return false;
    RECT rc{0, 0, w, h};
    if (FAILED(rt->BindDC(dc, &rc))) return false;
    ComPtr<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(rgb(0x111111), &br);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    // Card: surface, 1 px track edge (flat, no shadow or glow — docs/design.md).
    const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, kW - 0.5f, kH - 0.5f), 24, 24);
    rt->FillRoundedRectangle(card, br.Get());
    br->SetColor(rgb(0x262626));
    rt->DrawRoundedRectangle(card, br.Get(), 1);
    // Ring (volume) or a solid mark (mute / panic / profile).
    const D2D1_POINT_2F rc_center = D2D1::Point2F(16 + 22, kH / 2);
    const float r = 19;
    using K = arc::ServerObserver::RemoteChange::Kind;
    const bool muted_look = c.on && c.kind != K::Profile;  // muted target / panic on
    auto at = [&](float deg) {
        const float a = deg * kPi / 180.f;
        return D2D1::Point2F(rc_center.x + r * std::cos(a), rc_center.y + r * std::sin(a));
    };
    ComPtr<ID2D1StrokeStyle> round;
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
    auto arc = [&](float sweep, D2D1_COLOR_F col) {
        if (sweep <= 0.05f) return;
        ComPtr<ID2D1PathGeometry> g;
        ComPtr<ID2D1GeometrySink> sk;
        d2d_->CreatePathGeometry(&g);
        g->Open(&sk);
        sk->BeginFigure(at(135), D2D1_FIGURE_BEGIN_HOLLOW);
        sk->AddArc(D2D1::ArcSegment(at(135 + sweep), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                    sweep > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sk->EndFigure(D2D1_FIGURE_END_OPEN);
        sk->Close();
        br->SetColor(col);
        rt->DrawGeometry(g.Get(), br.Get(), 4, round.Get());
    };
    if (c.kind == K::Volume || c.kind == K::Mute) {
        arc(270, rgb(0x262626));
        arc(270.f * float(c.volume) / 10000.f, muted_look ? rgb(0x6B6B6B) : rgb(0xF6E7A1));
    } else {
        br->SetColor(muted_look ? rgb(0xF07A7A) : rgb(0xF6E7A1));
        rt->FillEllipse(D2D1::Ellipse(rc_center, 8, 8), br.Get());
    }
    // Two lines: what changed, and who / to what.
    std::wstring sub;
    const std::wstring title = text_of(c, pt_, sub);
    auto text = [&](const std::wstring& t, float y, float size, DWRITE_FONT_WEIGHT wgt, D2D1_COLOR_F col) {
        ComPtr<IDWriteTextFormat> f;
        dw_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, wgt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              size, L"", &f);
        if (!f) return;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteTextLayout> lay;
        dw_->CreateTextLayout(t.c_str(), UINT32(t.size()), f.Get(), kW - 76 - 20, 24, &lay);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> el;
        dw_->CreateEllipsisTrimmingSign(lay.Get(), &el);
        lay->SetTrimming(&trim, el.Get());
        br->SetColor(col);
        rt->DrawTextLayout(D2D1::Point2F(76, y), lay.Get(), br.Get());
    };
    text(title, 16, 15, DWRITE_FONT_WEIGHT_MEDIUM, rgb(0xFFFFFF));
    text(sub, 40, 13, DWRITE_FONT_WEIGHT_NORMAL, rgb(0x9A9A9A));
    return SUCCEEDED(rt->EndDraw());
}

void Hud::present(BYTE alpha) {
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    POINT src{0, 0};
    UpdateLayeredWindow(hwnd_, nullptr, &pos_, &size_, mem_, &src, 0, &bf, ULW_ALPHA);
}

void Hud::hide() {
    KillTimer(hwnd_, kHideTimer);
    KillTimer(hwnd_, kFadeTimer);
    ShowWindow(hwnd_, SW_HIDE);
}

void Hud::show(const arc::ServerObserver::RemoteChange& change) {
    if (!enabled_ || !hwnd_) return;
    // Never over a full-screen game, video or presentation (and never steal focus).
    if (user_is_busy_fullscreen() || foreground_is_fullscreen()) {
        hide();
        return;
    }
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    reduce_motion_ = !anim;
    // The screen the pointer is on, bottom centre, above the taskbar.
    POINT cur{};
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96;
    {
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        static auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor"));
        UINT y = 96;
        if (fn) fn(mon, 0, &dpi, &y);
    }
    const float s = float(dpi) / 96.f;
    size_ = SIZE{LONG(std::ceil(kW * s)), LONG(std::ceil(kH * s))};
    pos_ = POINT{mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - size_.cx) / 2, mi.rcWork.bottom - size_.cy - LONG(48 * s)};
    if (mem_) {
        SelectObject(mem_, old_);
        DeleteDC(mem_);
    }
    if (bmp_) DeleteObject(bmp_);
    HDC screen = GetDC(nullptr);
    mem_ = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size_.cx, -size_.cy, 1, 32, BI_RGB};
    void* bits = nullptr;
    bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    old_ = SelectObject(mem_, bmp_);
    if (!draw(change, s, mem_, size_.cx, size_.cy)) return;
    KillTimer(hwnd_, kFadeTimer);
    present(255);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(hwnd_, kHideTimer, 1600, nullptr);  // restarts while the phone keeps changing things
}

bool Hud::render_png(const arc::ServerObserver::RemoteChange& change, const std::wstring& path, float s) {
    const int w = int(std::ceil(kW * s)), h = int(std::ceil(kH * s));
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    bool ok = draw(change, s, mem, w, h);
    SelectObject(mem, old);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> wb;
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    ok = ok && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
         SUCCEEDED(wic->CreateBitmapFromMemory(UINT(w), UINT(h), GUID_WICPixelFormat32bppPBGRA, UINT(w * 4), UINT(w * h * 4),
                                               static_cast<BYTE*>(bits), &wb)) &&
         SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
         SUCCEEDED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
         SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->WriteSource(wb.Get(), nullptr)) && SUCCEEDED(fr->Commit()) &&
         SUCCEEDED(enc->Commit());
    DeleteObject(bmp);
    DeleteDC(mem);
    return ok;
}

}  // namespace agent
