#include "mixer_window.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "archon/protocol.hpp"
#include "mixer_uia.hpp"
#include "win_util.hpp"

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace agent {

namespace {

constexpr wchar_t kClass[] = L"ArchonMixer";
constexpr UINT WM_APP_MODEL = WM_APP + 50;
constexpr UINT_PTR kAnimTimer = 1;
constexpr float kPi = 3.14159265f;

// Tokens (docs/design.md).
D2D1_COLOR_F rgb(uint32_t v, float a = 1.f) {
    return D2D1::ColorF(float((v >> 16) & 255) / 255.f, float((v >> 8) & 255) / 255.f, float(v & 255) / 255.f, a);
}
const D2D1_COLOR_F kBg = rgb(0x000000), kSurface = rgb(0x111111), kRaised = rgb(0x1C1C1C), kTrack = rgb(0x262626),
                   kEdge = rgb(0x6B6B6B), kPaper = rgb(0xFFFFFF), kMuted = rgb(0x9A9A9A), kAccent = rgb(0xF6E7A1),
                   kInk = rgb(0x000000), kOk = rgb(0x6FD39B), kError = rgb(0xF07A7A), kHover = rgb(0x161616);

// Segoe Fluent Icons / MDL2 code points.
const wchar_t* kGlyphVolume = L"\xE767";
const wchar_t* kGlyphMute = L"\xE74F";
const wchar_t* kGlyphMic = L"\xE720";
const wchar_t* kGlyphMicOff = L"\xEC54";
const wchar_t* kGlyphSettings = L"\xE713";
const wchar_t* kGlyphProfiles = L"\xE8F1";
const wchar_t* kGlyphPanic = L"\xE74F";
const wchar_t* kGlyphChevron = L"\xE70D";
const wchar_t* kGlyphClose = L"\xE711";
const wchar_t* kGlyphEdit = L"\xE70F";
const wchar_t* kGlyphHeadphones = L"\xE7F6";

constexpr float kTop = 64;       // top bar height
constexpr float kCardH = 188, kPad = 24;
constexpr float kRingCy = 72;    // card ring centre (8 px below the corner mute button)
constexpr float kNameTop = 132;  // card name: 12 px under the ring
constexpr float kSheet = 380;    // panel width

std::wstring wstr(const std::string& s) { return to_wide(s); }
std::wstring pct(int v) { return std::to_wstring((v * 100 + 5000) / 10000) + L"%"; }

bool inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

float ease(float t) {  // cubic-bezier(0.2, 0, 0, 1), close enough: ease-out quint
    t = std::clamp(t, 0.f, 1.f);
    return 1.f - std::pow(1.f - t, 4.f);
}

bool volume_kind(Widget::Kind k) { return k == Widget::Kind::MasterRing || k == Widget::Kind::Card; }

}  // namespace

// ---- lifecycle --------------------------------------------------------------------

MixerWindow::MixerWindow() = default;
MixerWindow::~MixerWindow() { destroy(); }

bool MixerWindow::create(HINSTANCE inst, arc::Server* server, WindowHost host) {
    inst_ = inst;
    server_ = server;
    host_ = std::move(host);
    pt_ = is_portuguese_ui();
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dw_.GetAddressOf()))))
        return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    // Windows 11 ships Segoe Fluent Icons; Windows 10 has Segoe MDL2 Assets.
    {
        ComPtr<IDWriteFontCollection> fonts;
        dw_->GetSystemFontCollection(&fonts);
        UINT32 idx = 0;
        BOOL exists = FALSE;
        fonts->FindFamilyName(L"Segoe Fluent Icons", &idx, &exists);
        icon_font_ = exists ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    edit_brush_ = CreateSolidBrush(RGB(0x1C, 0x1C, 0x1C));
    return true;
}

void MixerWindow::destroy() {
    if (attached_ && server_) server_->attach_local(nullptr);
    attached_ = false;
    if (uia_) {
        uia_->disconnect();
        uia_ = nullptr;
    }
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (edit_brush_) DeleteObject(edit_brush_);
    edit_brush_ = nullptr;
    if (edit_font_) DeleteObject(edit_font_);
    edit_font_ = nullptr;
}

void MixerWindow::show(bool activate) {
    if (!hwnd_) {
        const UINT dpi = GetDpiForSystem();
        const int w = MulDiv(960, int(dpi), 96), h = MulDiv(680, int(dpi), 96);
        // Invisible mode: born off every monitor, no taskbar button, never activated.
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        hwnd_ = CreateWindowExW(invisible_ ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0, kClass, L"Archon",
                                WS_OVERLAPPEDWINDOW, invisible_ ? vx - w - 4000 : CW_USEDEFAULT,
                                invisible_ ? vy : CW_USEDEFAULT, w, h, nullptr, nullptr, inst_, this);
        if (!hwnd_) return;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
        scale_ = float(GetDpiForWindow(hwnd_)) / 96.f;
        make_formats();
    }
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    reduce_motion_ = !anim;
    if (!attached_ && server_) {
        server_->attach_local(this);  // full dump, then every change
        attached_ = true;
    }
    send(arc::encode_flag(arc::MsgType::SetMeters, !reduce_motion_));  // live dots only while shown
    if (activate && !invisible_) {
        ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(hwnd_);
    } else {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
    invalidate();
}

void MixerWindow::hide() {
    if (!hwnd_) return;
    send(arc::encode_flag(arc::MsgType::SetMeters, false));
    ShowWindow(hwnd_, SW_HIDE);
}

bool MixerWindow::visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

void MixerWindow::set_connected(size_t phones) {
    phones_ = phones;
    invalidate();
}

void MixerWindow::set_streams(size_t listening) {
    streams_ = listening;
    invalidate();
}

void MixerWindow::open_named(const std::string& which) {
    if (which == "profiles") open_panel(Panel::Profiles);
    else if (which == "settings") open_panel(Panel::Settings);
    else if (which == "editor") edit_profile(nullptr);
    else if (which == "editor-auto") {  // screenshots: an automatic profile with days, hours and network
        edit_profile(nullptr);
        editor_.p.automatic = true;
        editor_.use_time = true;
        editor_.p.days = 0b0111110;
        invalidate();
    }
    else close_panel();
}

// ---- model updates (server thread -> UI thread) ----------------------------------------

void MixerWindow::on_message(const arc::Bytes& msg) {
    std::lock_guard<std::mutex> lk(in_mu_);
    inbox_.push_back(msg);
    if (!posted_ && hwnd_) {
        posted_ = true;
        PostMessageW(hwnd_, WM_APP_MODEL, 0, 0);
    }
}

void MixerWindow::drain_messages() {
    std::deque<arc::Bytes> msgs;
    {
        std::lock_guard<std::mutex> lk(in_mu_);
        msgs.swap(inbox_);
        posted_ = false;
    }
    bool changed = false;
    for (const arc::Bytes& m : msgs) changed = model_.apply(m) || changed;
    // Forget optimistic values once the PC confirms them (or they expire).
    const auto now = Clock::now();
    for (auto it = overrides_.begin(); it != overrides_.end();) {
        bool done = now > it->second.until;
        if (!done && drag_key_ != it->first) {
            const std::string& id = it->first.substr(4);
            if (it->first.rfind("ep:", 0) == 0) {
                auto e = model_.endpoints.find(id);
                done = e != model_.endpoints.end() && e->second.volume == it->second.volume;
            } else {
                auto s = model_.sources.find(id);
                done = s != model_.sources.end() && s->second.volume == it->second.volume;
            }
        }
        it = done && drag_key_ != it->first ? overrides_.erase(it) : std::next(it);
    }
    request_icons();
    if (changed) invalidate();
}

void MixerWindow::request_icons() {
    for (const std::string& key : model_.icons_wanted) {
        if (std::find(icons_requested_.begin(), icons_requested_.end(), key) != icons_requested_.end()) continue;
        icons_requested_.push_back(key);
        send(arc::encode_id(arc::MsgType::GetIcon, key));
    }
}

void MixerWindow::send(arc::Bytes msg) {
    if (server_) server_->local_command(std::move(msg));
}

void MixerWindow::invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---- Direct2D resources -----------------------------------------------------------------

void MixerWindow::make_formats() {
    auto fmt = [&](float size, DWRITE_FONT_WEIGHT w, ComPtr<IDWriteTextFormat>& out) {
        out.Reset();
        dw_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              size, L"", &out);
        if (!out) dw_->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"", &out);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    fmt(13, DWRITE_FONT_WEIGHT_NORMAL, f_caption_);
    fmt(12, DWRITE_FONT_WEIGHT_NORMAL, f_small_);
    fmt(15, DWRITE_FONT_WEIGHT_MEDIUM, f_body_);
    fmt(22, DWRITE_FONT_WEIGHT_SEMI_BOLD, f_title_);
    fmt(64, DWRITE_FONT_WEIGHT_SEMI_BOLD, f_master_);
    fmt(26, DWRITE_FONT_WEIGHT_SEMI_BOLD, f_pct_);
    if (edit_font_) DeleteObject(edit_font_);
    edit_font_ = CreateFontW(-int(15 * scale_), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
}

bool MixerWindow::ensure_target() {
    if (rt_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = 96.f * scale_;
    if (FAILED(d2d_->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(UINT(rc.right), UINT(rc.bottom))), &rt_)))
        return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(kPaper, &brush_);
    bitmaps_.clear();
    return true;
}

void MixerWindow::discard_target() {
    bitmaps_.clear();
    brush_.Reset();
    rt_.Reset();
}

D2D1_RECT_F MixerWindow::client_dips() const {
    RECT rc{};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    return D2D1::RectF(0, 0, float(rc.right) / scale_, float(rc.bottom) / scale_);
}

ID2D1Bitmap* MixerWindow::icon_bitmap(const std::string& key) {
    if (key.empty() || !wic_) return nullptr;
    auto it = bitmaps_.find(key);
    if (it != bitmaps_.end()) return it->second.Get();
    auto png = model_.icons.find(key);
    if (png == model_.icons.end() || png->second.empty()) return nullptr;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    ComPtr<ID2D1Bitmap> bmp;
    if (SUCCEEDED(wic_->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(png->second.data()), DWORD(png->second.size()))) &&
        SUCCEEDED(wic_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic_->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
        rt_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
    bitmaps_[key] = bmp;  // null = undecodable: not retried
    return bmp.Get();
}

// ---- layout -------------------------------------------------------------------------------

uint16_t MixerWindow::shown_volume(const Widget& w) const {
    auto it = overrides_.find((w.endpoint ? "ep:" : "sr:") + w.target);
    return it != overrides_.end() ? it->second.volume : w.volume;
}

void MixerWindow::layout() {
    std::string keep = focus_key_;
    widgets_.clear();
    const D2D1_RECT_F c = client_dips();
    const float W = c.right, H = c.bottom;
    auto add = [&](Widget w) { widgets_.push_back(std::move(w)); };

    // Top bar.
    const arc::EndpointInfo* out = model_.output_or_default(output_);
    {
        Widget p;
        p.kind = Widget::Kind::OutputPill;
        p.key = "output";
        p.name = pt_ ? L"Dispositivo de saída" : L"Output device";
        p.sub = out ? wstr(out->name) : (pt_ ? L"Sem saída de som" : L"No output");
        p.r = D2D1::RectF(kPad, 12, kPad + std::min(320.f, std::max(160.f, W * 0.34f)), 52);
        p.glyph = kGlyphChevron;
        p.invoke = [this] {
            POINT pt{LONG((widgets_[0].r.left) * scale_), LONG(widgets_[0].r.bottom * scale_)};
            ClientToScreen(hwnd_, &pt);
            output_menu(pt);
        };
        add(p);
        Widget t;
        t.kind = Widget::Kind::Text;
        t.key = "phones";
        t.focusable = false;
        t.on = phones_ > 0;
        t.name = phones_ == 0 ? (pt_ ? L"Nenhum celular conectado" : L"No phone connected")
                 : phones_ == 1 ? (pt_ ? L"1 celular conectado" : L"1 phone connected")
                                : std::to_wstring(phones_) + (pt_ ? L" celulares conectados" : L" phones connected");
        t.r = D2D1::RectF(p.r.right + 16, 12, W - 3 * 48 - kPad, 52);
        if (t.r.right - t.r.left < 170) t.sub = std::to_wstring(phones_);  // narrow: dot + count
        if (t.r.right - t.r.left > 24) add(t);
    }
    // Buttons left to right in reading (and Tab) order, right-aligned.
    float bx = W - kPad - 40 - 48.f * float(model_.can_profiles() ? 2 : 1);
    auto top_button = [&](const char* key, const wchar_t* glyph, std::wstring name, bool on, bool danger,
                          std::function<void()> fn) {
        Widget b;
        b.kind = Widget::Kind::IconButton;
        b.key = key;
        b.glyph = glyph;
        b.name = std::move(name);
        b.on = on;
        b.danger = danger;
        b.r = D2D1::RectF(bx, 12, bx + 40, 52);
        b.invoke = std::move(fn);
        add(b);
        bx += 48;
    };
    if (model_.can_profiles())
        top_button("profiles", kGlyphProfiles, pt_ ? L"Perfis" : L"Profiles",
                   panel_ == Panel::Profiles || panel_ == Panel::Editor, false, [this] {
                       panel_ == Panel::Profiles ? close_panel() : open_panel(Panel::Profiles);
                   });
    top_button("panic", kGlyphPanic,
               model_.panic ? (pt_ ? L"Reativar o som do PC" : L"Unmute the PC")
                            : (pt_ ? L"Silenciar tudo no PC" : L"Mute everything on the PC"),
               model_.panic, model_.panic, [this] { send(arc::encode_flag(arc::MsgType::PanicMute, !model_.panic)); });
    top_button("settings", kGlyphSettings, pt_ ? L"Configurações" : L"Settings", panel_ == Panel::Settings, false,
               [this] { panel_ == Panel::Settings ? close_panel() : open_panel(Panel::Settings); });

    const bool wide = W >= 760;
    if (panel_ != Panel::None) {
        const float sw = wide ? kSheet : W;
        const float t = reduce_motion_ ? 1.f
                                       : ease(std::chrono::duration<float>(Clock::now() - panel_at_).count() / 0.22f);
        const float x = W - sw * t;
        if (wide) layout_body(0, kTop, W, H - kTop);
        layout_panel(x, kTop, sw, H - kTop);
    } else {
        layout_body(0, kTop, W, H - kTop);
    }
    // Keep keyboard focus on the same thing across relayouts.
    focus_ = -1;
    for (size_t i = 0; i < widgets_.size(); ++i)
        if (!keep.empty() && widgets_[i].key == keep) focus_ = int(i);
    if (hover_ >= int(widgets_.size())) hover_ = -1;
}

void MixerWindow::layout_body(float x0, float y0, float W, float H) {
    // 8 px grid: 24 margins, 16 gaps. Wide: the master card on the left and the
    // card grid on the right, both starting on the same line. Narrow: one
    // column that scrolls as a whole.
    constexpr float M = 24, G = 16, kMinCard = 168, kMaxCard = 232, kMasterW = 296;
    const bool wide = W >= 760;
    const arc::EndpointInfo* out = model_.output_or_default(output_);
    const bool covered = panel_ != Panel::None;  // the sheet is on top: body is not interactive
    const float top = y0 + 8;
    grid_clip_ = wide ? D2D1::RectF(x0 + M + kMasterW + 24 - 6, y0, x0 + W, y0 + H) : D2D1::RectF(x0, y0, x0 + W, y0 + H);
    const float shift = wide ? 0.f : scroll_;  // narrow: everything scrolls together
    auto add = [&](Widget w, bool scrolls) {
        if (covered) w.focusable = false;
        w.scrolls = scrolls;
        if (scrolls) w.clipped = w.r.bottom <= grid_clip_.top || w.r.top >= grid_clip_.bottom;
        widgets_.push_back(std::move(w));
    };
    auto text_line = [&](const std::string& key, std::wstring s, const wchar_t* glyph, D2D1_RECT_F r, bool scrolls) {
        Widget l;
        l.kind = Widget::Kind::Text;
        l.key = key;
        l.focusable = false;
        l.name = std::move(s);
        if (glyph) l.glyph = glyph;
        l.on = true;
        l.r = r;
        add(l, scrolls);
    };

    // ---- master card ----
    float master_bottom = top;
    if (out) {
        // Title on the same line as the grid's first header; the card starts with the first row.
        const float px = x0 + M, pw = wide ? kMasterW : W - 2 * M, py = top - shift + 32;
        const float R = wide ? 96.f : 84.f;
        const float cx = px + pw / 2, cy = py + 24 + R;
        float ly = cy + R + 8 + 40 + 16;  // ring, mute button, gap
        const bool listening = streams_ > 0;
        const arc::Profile* ap = model_.active_profile();
        float ph = (ly - py) + (listening ? 28.f : 0.f) + (ap ? 28.f : 0.f) + 12;
        // Wide: end on the bottom of a row of cards, so both columns share their lines.
        if (wide)
            for (int rows = 1; rows < 10; ++rows) {
                const float row_end = float(rows) * kCardH + float(rows - 1) * G;
                if (row_end >= ph) {
                    ph = row_end;
                    break;
                }
            }
        Widget card;
        card.kind = Widget::Kind::Surface;
        card.key = "master-card";
        card.focusable = false;
        card.r = D2D1::RectF(px, py, px + pw, py + ph);
        add(card, !wide);
        Widget h;
        h.kind = Widget::Kind::Header;
        h.key = "master-title";
        h.focusable = false;
        h.name = pt_ ? L"Volume geral" : L"Master volume";
        h.r = D2D1::RectF(px, py - 32, px + pw, py - 10);
        add(h, !wide);
        Widget m;
        m.kind = Widget::Kind::MasterRing;
        m.key = "master";
        m.endpoint = true;
        m.target = out->id;
        m.volume = out->volume;
        m.muted = out->muted;
        m.name = (pt_ ? L"Volume geral, " : L"Master volume, ") + wstr(out->name);
        m.r = D2D1::RectF(cx - R, cy - R, cx + R, cy + R);
        add(m, !wide);
        Widget mb;
        mb.kind = Widget::Kind::Mute;
        mb.key = "master:mute";
        mb.endpoint = true;
        mb.target = out->id;
        mb.on = out->muted;
        // A checkbox: fixed name, the state is read as checked / not checked.
        mb.name = pt_ ? L"Mutar o volume geral" : L"Mute master volume";
        mb.r = D2D1::RectF(cx - 20, cy + R + 8, cx + 20, cy + R + 48);
        add(mb, !wide);
        if (listening) {
            text_line("listening", pt_ ? L"Um celular está ouvindo este PC" : L"A phone is listening to this PC",
                      kGlyphHeadphones, D2D1::RectF(px + 12, ly, px + pw - 12, ly + 24), !wide);
            ly += 28;
        }
        if (ap) {
            text_line("active-profile", (pt_ ? L"Perfil em uso: " : L"Profile in use: ") + wstr(ap->name), nullptr,
                      D2D1::RectF(px + 12, ly, px + pw - 12, ly + 24), !wide);
            ly += 28;
        }
        master_bottom = py + ph + shift;
    }

    // ---- card grid ----
    const float gx = wide ? x0 + M + kMasterW + 24 : x0 + M;
    const float gw = wide ? W - gx - M : W - 2 * M;
    const int cols = std::max(1, int((gw + G) / (kMinCard + G)));
    const float cw = std::floor(std::min(kMaxCard, (gw - G * float(cols - 1)) / float(cols)));  // whole pixels: crisp edges
    float y = (wide ? top : master_bottom + 24) - scroll_;
    int col = 0;
    auto header = [&](const std::string& key, std::wstring s) {
        Widget h;
        h.kind = Widget::Kind::Header;
        h.key = key;
        h.focusable = false;
        h.name = std::move(s);
        h.r = D2D1::RectF(gx, y, gx + gw, y + 22);
        add(h, true);
        y += 22 + 10;
    };
    auto newline = [&] {
        if (col != 0) {
            col = 0;
            y += kCardH + G;
        }
    };
    auto card = [&](const std::string& key, bool endpoint, const std::string& target, std::wstring name, uint16_t vol,
                    bool muted, bool playing, uint16_t meter, const std::string& icon, std::wstring sub, bool system,
                    bool tab, bool input) {
        Widget w;
        w.kind = Widget::Kind::Card;
        w.key = key;
        w.endpoint = endpoint;
        w.target = target;
        w.name = std::move(name);
        w.sub = std::move(sub);
        w.volume = vol;
        w.muted = muted;
        w.playing = playing;
        w.meter = meter;
        w.icon_key = icon;
        w.system = system;
        w.tab = tab;
        w.input = input;
        // Whole-pixel widths; the leftover pixels go into the gaps, so both margins are equal.
        const float cx = cols > 1 ? gx + std::round(float(col) * (gw - cw) / float(cols - 1)) : gx;
        w.r = D2D1::RectF(cx, y, cx + cw, y + kCardH);
        if (++col == cols) {
            col = 0;
            y += kCardH + G;
        }
        Widget m;
        m.kind = Widget::Kind::Mute;
        m.key = key + ":mute";
        m.endpoint = endpoint;
        m.target = target;
        m.on = muted;
        m.name = (pt_ ? L"Mutar " : L"Mute ") + w.name;  // checkbox: state read as checked / not checked
        m.r = D2D1::RectF(w.r.right - 44, w.r.top + 4, w.r.right - 4, w.r.top + 44);
        add(w, true);
        add(m, true);
    };
    const auto apps = out ? model_.apps_on(out->id) : std::vector<const arc::SourceInfo*>{};
    const auto tabs = out ? model_.tabs_on(out->id) : std::vector<const arc::SourceInfo*>{};
    header("apps-header", pt_ ? L"Microfone e apps" : L"Microphone and apps");
    if (const arc::EndpointInfo* mic = model_.microphone())
        card("mic", true, mic->id, pt_ ? L"Microfone" : L"Microphone", mic->volume, mic->muted,
             model_.level(mic->meter) > 0.02f, mic->meter, "", wstr(mic->name), false, false, true);
    for (const arc::SourceInfo* s : apps) {
        std::wstring sub;
        if (!s->route.empty())
            if (auto r = model_.endpoints.find(s->route); r != model_.endpoints.end()) sub = L"\x2192 " + wstr(r->second.name);
        card("src:" + s->id, false, s->id, s->system_sounds ? (pt_ ? L"Sons do sistema" : L"System sounds") : wstr(s->name),
             s->volume, s->muted, s->playing, s->meter, s->icon_key, sub, s->system_sounds, false, false);
    }
    newline();
    if (apps.empty()) {
        Widget e;
        e.kind = Widget::Kind::Text;
        e.key = "empty";
        e.focusable = false;
        e.name = pt_ ? L"Nenhum app usando este dispositivo agora." : L"No apps are using this device right now.";
        e.r = D2D1::RectF(gx, y, gx + gw, y + 32);
        add(e, true);
        y += 32 + G;
    }
    if (!tabs.empty()) {
        y += 8;
        header("tabs-header", pt_ ? L"Abas do navegador" : L"Browser tabs");
        for (const arc::SourceInfo* s : tabs)
            card("src:" + s->id, false, s->id, wstr(s->name), s->volume, s->muted, s->playing, 0, s->icon_key,
                 wstr(s->detail), false, true, false);
        newline();
    }
    const float content = y + scroll_ - y0 + M;
    scroll_max_ = std::max(0.f, content - H);
    if (scroll_ > scroll_max_) scroll_ = scroll_max_;
}

void MixerWindow::layout_panel(float x0, float y0, float w, float h) {
    panel_clip_ = D2D1::RectF(x0, y0, x0 + w, y0 + h);
    float y = y0 + 16 - pscroll_;
    const float l = x0 + 20, r = x0 + w - 20;
    auto add = [&](Widget wd) {
        wd.clipped = wd.r.bottom <= panel_clip_.top || wd.r.top >= panel_clip_.bottom;
        widgets_.push_back(std::move(wd));
    };
    auto text = [&](const std::string& key, std::wstring s, float hgt = 22, bool header = false) {
        Widget t;
        t.kind = header ? Widget::Kind::Header : Widget::Kind::Text;
        t.key = key;
        t.focusable = false;
        t.name = std::move(s);
        const bool title = key.find(":title") != std::string::npos;
        if (header && !title) {
            y += 16;   // a section starts 24 px below the previous one (8 + 16)
            hgt = 22;
        }
        t.r = D2D1::RectF(l, y, r, y + hgt);
        add(t);
        y += hgt + 8;
    };
    auto button = [&](Widget::Kind kind, const std::string& key, std::wstring name, std::function<void()> fn) {
        Widget b;
        b.kind = kind;
        b.key = key;
        b.name = std::move(name);
        b.r = D2D1::RectF(l, y, r, y + 44);
        b.invoke = std::move(fn);
        add(b);
        y += 52;
    };
    auto toggle = [&](const std::string& key, std::wstring name, std::wstring sub, bool on, std::function<void()> fn) {
        Widget t;
        t.kind = Widget::Kind::Toggle;
        t.key = key;
        t.name = std::move(name);
        t.sub = std::move(sub);
        t.on = on;
        t.r = D2D1::RectF(l, y, r, y + (t.sub.empty() ? 44.f : 52.f));
        t.invoke = std::move(fn);
        y += t.r.bottom - t.r.top + 4;
        add(t);
    };
    {
        Widget c;
        c.kind = Widget::Kind::PanelClose;
        c.key = "panel:close";
        c.glyph = kGlyphClose;
        c.name = pt_ ? L"Fechar" : L"Close";
        c.r = D2D1::RectF(x0 + w - 56, y0 + 8, x0 + w - 16, y0 + 48);
        c.invoke = [this] { panel_ == Panel::Editor ? open_panel(Panel::Profiles) : close_panel(); };
        add(c);
    }

    if (panel_ == Panel::Profiles) {
        text("p:title", pt_ ? L"Perfis" : L"Profiles", 32, true);
        text("p:net", model_.network.empty()
                          ? (pt_ ? L"O PC não está em nenhuma rede agora." : L"The PC isn't on any network right now.")
                          : (pt_ ? L"Rede do PC: " : L"PC's network: ") + wstr(model_.network));
        const auto list = model_.sorted_profiles();
        if (list.empty())
            text("p:empty", pt_ ? L"Salve o som do PC como está e aplique de novo com um clique — ou deixe o PC "
                                  L"aplicar sozinho em certos dias, horários, redes ou quando um fone for conectado."
                                : L"Save the PC's sound as it is and apply it again with one click — or let the PC "
                                  L"apply it by itself on certain days and hours, networks, or when headphones are "
                                  L"plugged in.",
                 88);
        for (const arc::Profile* p : list) {
            Widget row;
            row.kind = Widget::Kind::ProfileRow;
            row.key = "p:" + p->id;
            row.name = wstr(p->name);
            row.sub = wstr(profile_summary(*p, pt_));
            row.on = p->active;
            row.focusable = false;
            row.r = D2D1::RectF(l, y, r, y + 72);
            add(row);
            Widget ap;
            ap.kind = Widget::Kind::ProfileApply;
            ap.key = "p:" + p->id + ":apply";
            ap.on = p->active;
            ap.focusable = !p->active;  // in force already: shown as "Em uso"
            ap.name = p->active ? (pt_ ? L"Em uso: " : L"In use: ") + wstr(p->name) : (pt_ ? L"Aplicar " : L"Apply ") + wstr(p->name);
            ap.r = D2D1::RectF(r - 140, y + 16, r - 52, y + 56);
            const std::string id = p->id;
            ap.invoke = [this, id] { send(arc::encode_id(arc::MsgType::ApplyProfile, id)); };
            add(ap);
            Widget ed;
            ed.kind = Widget::Kind::ProfileEdit;
            ed.key = "p:" + p->id + ":edit";
            ed.glyph = kGlyphEdit;
            ed.name = (pt_ ? L"Editar " : L"Edit ") + wstr(p->name);
            ed.r = D2D1::RectF(r - 44, y + 16, r - 4, y + 56);
            ed.invoke = [this, id] {
                auto it = model_.profiles.find(id);
                if (it != model_.profiles.end()) edit_profile(&it->second);
            };
            add(ed);
            y += 80;
        }
        y += 4;
        button(Widget::Kind::PrimaryButton, "p:new", pt_ ? L"Salvar o som atual como perfil" : L"Save the current sound as a profile",
               [this] { edit_profile(nullptr); });
    } else if (panel_ == Panel::Editor) {
        Editor& e = editor_;
        text("e:title", e.is_new ? (pt_ ? L"Novo perfil" : L"New profile") : (pt_ ? L"Editar perfil" : L"Edit profile"),
             32, true);
        y += 18;  // the "Nome" label sits above the box
        {
            Widget n;
            n.kind = Widget::Kind::Text;
            n.key = "e:name";
            n.name = pt_ ? L"Nome" : L"Name";
            n.focusable = true;  // focus goes to the text box
            n.r = D2D1::RectF(l, y, r, y + 44);
            add(n);
            y += 52;
        }
        if (!e.is_new)
            text("e:holds", (pt_ ? L"Guarda " : L"Holds ") + std::to_wstring(e.p.endpoints.size()) +
                                (pt_ ? L" dispositivos e " : L" devices and ") + std::to_wstring(e.p.apps.size()) +
                                L" apps.");
        toggle("e:auto", pt_ ? L"Aplicar sozinho" : L"Apply by itself", L"", e.p.automatic, [this] {
            editor_.p.automatic = !editor_.p.automatic;
            invalidate();
        });
        if (e.p.automatic) {
            text("e:hint", pt_ ? L"Quando todas as condições ligadas valerem:" : L"When every condition turned on holds:");
            toggle("e:time", pt_ ? L"Dias e horário" : L"Days and hours", L"", e.use_time, [this] {
                editor_.use_time = !editor_.use_time;
                invalidate();
            });
            if (e.use_time) {
                static const wchar_t* letters_pt[] = {L"D", L"S", L"T", L"Q", L"Q", L"S", L"S"};
                static const wchar_t* letters_en[] = {L"S", L"M", L"T", L"W", L"T", L"F", L"S"};
                static const wchar_t* names_pt[] = {L"domingo", L"segunda", L"terça", L"quarta", L"quinta", L"sexta", L"sábado"};
                static const wchar_t* names_en[] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"};
                for (int d = 0; d < 7; ++d) {
                    Widget c;
                    c.kind = Widget::Kind::DayChip;
                    c.key = "e:day" + std::to_string(d);
                    c.glyph = pt_ ? letters_pt[d] : letters_en[d];
                    c.name = pt_ ? names_pt[d] : names_en[d];
                    c.on = e.p.days & (1 << d);
                    c.r = D2D1::RectF(l + float(d) * 44, y, l + float(d) * 44 + 38, y + 38);
                    c.invoke = [this, d] {
                        editor_.p.days ^= uint8_t(1 << d);
                        invalidate();
                    };
                    add(c);
                }
                y += 44;
                text("e:days", e.p.days == 0 ? (pt_ ? L"Todos os dias" : L"Every day")
                                             : (pt_ ? L"Só nos dias marcados" : L"Only on the marked days"));
                for (int k = 0; k < 2; ++k) {
                    Widget t;
                    t.kind = Widget::Kind::TimeStepper;
                    t.key = k == 0 ? "e:from" : "e:until";
                    t.name = k == 0 ? (pt_ ? L"Das" : L"From") : (pt_ ? L"Até" : L"Until");
                    t.minutes = k == 0 ? e.p.start_min : e.p.end_min;
                    const float half = (r - l - 8) / 2;
                    t.r = D2D1::RectF(l + float(k) * (half + 8), y, l + float(k) * (half + 8) + half, y + 52);
                    add(t);
                }
                y += 60;
            }
            const std::wstring net = wstr(e.p.network.empty() ? model_.network : e.p.network);
            toggle("e:net", pt_ ? L"Só nesta rede" : L"Only on this network",
                   net.empty() ? (pt_ ? L"PC sem rede agora" : L"PC has no network now") : net, e.use_network, [this] {
                       if (!editor_.use_network && editor_.p.network.empty() && model_.network.empty()) return;
                       editor_.use_network = !editor_.use_network;
                       invalidate();
                   });
            {
                Widget c;
                c.kind = Widget::Kind::Choice;
                c.key = "e:output";
                c.name = pt_ ? L"Quando esta saída estiver conectada" : L"When this output is connected";
                c.sub = e.p.output_id.empty() ? (pt_ ? L"Qualquer" : L"Any") : wstr(e.p.output_name);
                c.glyph = kGlyphChevron;
                c.r = D2D1::RectF(l, y, r, y + 52);
                c.invoke = [this] {
                    HMENU m = CreatePopupMenu();
                    AppendMenuW(m, MF_STRING | (editor_.p.output_id.empty() ? MF_CHECKED : 0), 1, pt_ ? L"Qualquer" : L"Any");
                    const auto outs = model_.outputs();
                    for (size_t i = 0; i < outs.size(); ++i)
                        AppendMenuW(m, MF_STRING | (outs[i]->id == editor_.p.output_id ? MF_CHECKED : 0), UINT(i + 2),
                                    wstr(outs[i]->name).c_str());
                    POINT pt;
                    GetCursorPos(&pt);
                    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
                    DestroyMenu(m);
                    if (cmd == 1) {
                        editor_.p.output_id.clear();
                        editor_.p.output_name.clear();
                    } else if (cmd >= 2 && cmd - 2 < outs.size()) {
                        editor_.p.output_id = outs[cmd - 2]->id;
                        editor_.p.output_name = outs[cmd - 2]->name;
                    }
                    invalidate();
                };
                add(c);
                y += 60;
            }
            toggle("e:restore", pt_ ? L"Ao terminar, voltar como estava" : L"When it ends, put things back", L"",
                   e.p.restore, [this] {
                       editor_.p.restore = !editor_.p.restore;
                       invalidate();
                   });
        }
        y += 6;
        button(Widget::Kind::PrimaryButton, "e:save", pt_ ? L"Salvar" : L"Save", [this] { save_editor(editor_.is_new); });
        if (!e.is_new) {
            button(Widget::Kind::SecondaryButton, "e:capture",
                   pt_ ? L"Atualizar com o som atual" : L"Update with the current sound", [this] { save_editor(true); });
            const std::string id = e.p.id;
            button(Widget::Kind::DangerButton, "e:delete", pt_ ? L"Apagar perfil" : L"Delete profile", [this, id] {
                const std::wstring q = (pt_ ? L"Apagar \"" : L"Delete \"") + wstr(editor_.p.name) + L"\"?";
                if (MessageBoxW(hwnd_, q.c_str(), L"Archon", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
                send(arc::encode_id(arc::MsgType::DeleteProfile, id));
                open_panel(Panel::Profiles);
            });
        }
        button(Widget::Kind::SecondaryButton, "e:cancel", pt_ ? L"Cancelar" : L"Cancel", [this] { open_panel(Panel::Profiles); });
    } else if (panel_ == Panel::Settings) {
        text("s:title", pt_ ? L"Configurações" : L"Settings", 32, true);
        text("s:phones", pt_ ? L"Celulares pareados" : L"Paired phones", 26, true);
        const auto paired = host_.paired ? host_.paired() : std::vector<arc::PairedController>{};
        if (paired.empty())
            text("s:none", pt_ ? L"Nenhum celular pareado. Abra o Archon no celular para parear."
                               : L"No paired phones. Open Archon on the phone to pair.",
                 40);
        for (const arc::PairedController& pc : paired) {
            Widget d;
            d.kind = Widget::Kind::DeviceRow;
            d.key = "s:dev:" + arc::to_hex(pc.id);
            d.name = (pt_ ? L"Remover " : L"Remove ") + wstr(pc.name);
            d.sub = wstr(pc.name);
            d.r = D2D1::RectF(l, y, r, y + 48);
            const arc::Id16 id = pc.id;
            const std::wstring who = wstr(pc.name);
            d.invoke = [this, id, who] {
                const std::wstring q = (pt_ ? L"Remover \"" : L"Remove \"") + who +
                                       (pt_ ? L"\"? Ele vai precisar de um novo código para parear."
                                            : L"\"? It will need a new code to pair again.");
                if (MessageBoxW(hwnd_, q.c_str(), L"Archon", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
                if (host_.forget) host_.forget(id);
                invalidate();
            };
            add(d);
            y += 56;
        }
        y += 8;
        toggle("s:autostart", pt_ ? L"Iniciar com o Windows" : L"Start with Windows", L"",
               host_.autostart && host_.autostart(), [this] {
                   if (host_.set_autostart && host_.autostart) host_.set_autostart(!host_.autostart());
                   invalidate();
               });
        text("s:ext", pt_ ? L"Extensão do navegador" : L"Browser extension", 26, true);
        const int browsers = host_.browsers ? host_.browsers() : -1;
        text("s:ext-status",
             browsers < 0  ? (pt_ ? L"Indisponível: o archon-tabs-host.exe precisa estar ao lado do Archon.exe."
                                  : L"Unavailable: archon-tabs-host.exe must be next to Archon.exe.")
             : browsers == 0 ? (pt_ ? L"Pronto. Nenhum navegador conectado ainda." : L"Ready. No browser connected yet.")
            : browsers == 1 ? (pt_ ? L"1 navegador conectado." : L"1 browser connected.")
                            : std::to_wstring(browsers) + (pt_ ? L" navegadores conectados." : L" browsers connected."),
             browsers < 0 ? 40.f : 22.f);
        button(Widget::Kind::SecondaryButton, "s:ext-help", pt_ ? L"Como instalar a extensão" : L"How to install the extension",
               [] {
                   ShellExecuteW(nullptr, L"open", L"https://github.com/mxxnpy/archon#build", nullptr, nullptr,
                                 SW_SHOWNORMAL);
               });
        text("s:other", pt_ ? L"Mais" : L"More", 26, true);
        text("s:hotkey", host_.hotkey ? (pt_ ? L"Silenciar tudo: Ctrl+Alt+M" : L"Mute everything: Ctrl+Alt+M")
                                      : (pt_ ? L"Atalho Ctrl+Alt+M em uso por outro programa."
                                             : L"Ctrl+Alt+M is taken by another program."));
        text("s:net", (pt_ ? L"Rede do PC: " : L"PC's network: ") +
                          (model_.network.empty() ? std::wstring(pt_ ? L"nenhuma" : L"none") : wstr(model_.network)));
        button(Widget::Kind::SecondaryButton, "s:log", pt_ ? L"Abrir log de diagnóstico" : L"Open diagnostic log", [this] {
            if (host_.open_log) host_.open_log();
        });
        text("s:version", L"Archon 1.0.0");
    }
    const float content = y + pscroll_ - (y0 + 16) + 16;
    pscroll_max_ = std::max(0.f, content - h);
    if (pscroll_ > pscroll_max_) pscroll_ = pscroll_max_;
}

// ---- painting ----------------------------------------------------------------------------

void MixerWindow::draw_text(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, D2D1_COLOR_F c,
                            DWRITE_TEXT_ALIGNMENT align, bool wrap) {
    if (s.empty() || !f) return;
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(dw_->CreateTextLayout(s.c_str(), UINT32(s.size()), f, std::max(1.f, r.right - r.left),
                                     std::max(1.f, r.bottom - r.top), &lay)))
        return;
    lay->SetTextAlignment(align);
    lay->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ellipsis;
    dw_->CreateEllipsisTrimmingSign(lay.Get(), &ellipsis);
    lay->SetTrimming(&trim, ellipsis.Get());
    brush_->SetColor(c);
    rt_->DrawTextLayout(D2D1::Point2F(r.left, r.top), lay.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float MixerWindow::text_width(const std::wstring& s, IDWriteTextFormat* f, float max_w) {
    ComPtr<IDWriteTextLayout> lay;
    if (s.empty() || FAILED(dw_->CreateTextLayout(s.c_str(), UINT32(s.size()), f, max_w, 100, &lay))) return 0;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return std::min(max_w, m.widthIncludingTrailingWhitespace);
}

void MixerWindow::draw_glyph(const std::wstring& g, const D2D1_RECT_F& r, float size, D2D1_COLOR_F c) {
    ComPtr<IDWriteTextFormat> f;
    dw_->CreateTextFormat(icon_font_.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                          DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (!f) return;
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    brush_->SetColor(c);
    rt_->DrawTextW(g.c_str(), UINT32(g.size()), f.Get(), r, brush_.Get());
}

void MixerWindow::draw_ring(D2D1_POINT_2F c, float radius, float stroke, float fraction, D2D1_COLOR_F color,
                            float thumb) {
    auto at = [&](float deg) {
        const float a = deg * kPi / 180.f;
        return D2D1::Point2F(c.x + radius * std::cos(a), c.y + radius * std::sin(a));
    };
    ComPtr<ID2D1StrokeStyle> round;
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
    auto arc = [&](float from, float sweep, D2D1_COLOR_F col) {
        if (sweep <= 0.05f) return;
        ComPtr<ID2D1PathGeometry> g;
        ComPtr<ID2D1GeometrySink> s;
        d2d_->CreatePathGeometry(&g);
        g->Open(&s);
        s->BeginFigure(at(from), D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddArc(D2D1::ArcSegment(at(from + sweep), D2D1::SizeF(radius, radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                   sweep > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
        brush_->SetColor(col);
        rt_->DrawGeometry(g.Get(), brush_.Get(), stroke, round.Get());
    };
    fraction = std::clamp(fraction, 0.f, 1.f);
    arc(135, 270, kTrack);
    arc(135, 270 * fraction, color);
    if (thumb > 0) {
        brush_->SetColor(kPaper);
        rt_->FillEllipse(D2D1::Ellipse(at(135 + 270 * fraction), thumb, thumb), brush_.Get());
    }
}

void MixerWindow::draw_widget(const Widget& w) {
    using K = Widget::Kind;
    const bool hover = hover_ >= 0 && &widgets_[size_t(hover_)] == &w;
    const D2D1_RECT_F& r = w.r;
    auto round_fill = [&](const D2D1_RECT_F& rr, float rad, D2D1_COLOR_F col) {
        brush_->SetColor(col);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(rr, rad, rad), brush_.Get());
    };
    // 1 px edge so a control's boundary reaches 3:1 against its background (WCAG 1.4.11).
    auto round_edge = [&](const D2D1_RECT_F& rr, float rad) {
        brush_->SetColor(kEdge);
        const D2D1_RECT_F in = D2D1::RectF(rr.left + 0.5f, rr.top + 0.5f, rr.right - 0.5f, rr.bottom - 0.5f);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(in, rad, rad), brush_.Get(), 1);
    };
    // Smoothly animated arc fraction (remote changes), instant while dragging or with reduce motion.
    auto arc_fraction = [&](const Widget& v) {
        const float target = float(shown_volume(v)) / 10000.f;
        auto& from = arcs_[v.key];
        auto& last = arc_target_[v.key];
        const bool dragging = drag_ >= 0 && widgets_[size_t(drag_)].key == v.key;
        if (from.second == Clock::time_point{} || dragging || reduce_motion_) {
            from = {target, Clock::time_point::min()};
            last = target;
            return target;
        }
        if (std::fabs(last - target) > 0.0001f) {
            const float t0 = std::chrono::duration<float>(Clock::now() - from.second).count() / 0.24f;
            const float current = from.second == Clock::time_point::min() ? last : from.first + (last - from.first) * ease(t0);
            from = {current, Clock::now()};
            last = target;
        }
        if (from.second == Clock::time_point::min()) return target;
        const float t = std::chrono::duration<float>(Clock::now() - from.second).count() / 0.24f;
        if (t >= 1) {
            from.second = Clock::time_point::min();
            return target;
        }
        return from.first + (target - from.first) * ease(t);
    };

    switch (w.kind) {
        case K::OutputPill: {
            round_fill(r, 20, hover ? kRaised : kSurface);
            draw_text(w.sub, D2D1::RectF(r.left + 16, r.top, r.right - 36, r.bottom), f_body_.Get(), kPaper);
            draw_glyph(w.glyph, D2D1::RectF(r.right - 34, r.top, r.right - 10, r.bottom), 12, kMuted);
            break;
        }
        case K::IconButton: {
            const D2D1_COLOR_F fill = w.danger ? kError : (w.on ? kAccent : (hover ? rgb(0x2A2A2A) : kRaised));
            brush_->SetColor(fill);
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2), 20, 20),
                             brush_.Get());
            draw_glyph(w.glyph, r, 16, w.danger || w.on ? kInk : kPaper);
            break;
        }
        case K::Text: {
            if (w.key == "phones") {
                brush_->SetColor(w.on ? kOk : kEdge);
                const float cy = (r.top + r.bottom) / 2;
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 4, cy), 4, 4), brush_.Get());
                if (w.sub.empty()) {
                    draw_text(w.name, D2D1::RectF(r.left + 16, r.top, r.right, r.bottom), f_caption_.Get(),
                              w.on ? kPaper : kMuted);
                } else {  // narrow: a phone glyph and the count (screen readers keep the sentence)
                    draw_glyph(L"\xE8EA", D2D1::RectF(r.left + 12, r.top, r.left + 30, r.bottom), 14, w.on ? kPaper : kMuted);
                    draw_text(w.sub, D2D1::RectF(r.left + 32, r.top, r.right, r.bottom), f_caption_.Get(),
                              w.on ? kPaper : kMuted);
                }
            } else if (w.key == "e:name") {
                draw_text(w.name, D2D1::RectF(r.left, r.top - 22, r.right, r.top - 4), f_caption_.Get(), kMuted);
                round_fill(r, 12, kRaised);  // the text box itself is a child EDIT on top
                round_edge(r, 12);
            } else if (!w.glyph.empty()) {
                const float tw = text_width(w.name, f_caption_.Get(), r.right - r.left - 26);
                const float lx = (r.left + r.right) / 2 - (tw + 26) / 2;
                draw_glyph(w.glyph, D2D1::RectF(lx, r.top, lx + 18, r.bottom), 14, kAccent);
                draw_text(w.name, D2D1::RectF(lx + 26, r.top, lx + 26 + tw + 1, r.bottom), f_caption_.Get(), kPaper);
            } else {
                const bool center = w.key == "active-profile" || w.key == "empty";
                draw_text(w.name, r, f_caption_.Get(), kMuted,
                          center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING, true);
            }
            break;
        }
        case K::Surface:
            round_fill(r, 24, kSurface);
            break;
        case K::Header:
            draw_text(w.name, r, w.key.find(":title") != std::string::npos ? f_title_.Get() : f_caption_.Get(),
                      w.key.find(":title") != std::string::npos ? kPaper : kMuted);
            break;
        case K::MasterRing: {
            const D2D1_POINT_2F c = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
            const float rad = (r.right - r.left) / 2 - 4;
            const float f = arc_fraction(w);
            draw_ring(c, rad, 8, f, w.muted ? kEdge : kAccent, 9);
            const int v = shown_volume(w);
            const std::wstring n = std::to_wstring((v * 100 + 5000) / 10000);
            const float big = rad > 90 ? 60.f : 52.f;
            ComPtr<IDWriteTextLayout> lay;
            dw_->CreateTextLayout(n.c_str(), UINT32(n.size()), f_master_.Get(), 200, 90, &lay);
            DWRITE_TEXT_RANGE all{0, UINT32(n.size())};
            lay->SetFontSize(big, all);
            lay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            DWRITE_TEXT_METRICS m{};
            lay->GetMetrics(&m);
            DWRITE_LINE_METRICS line{};
            UINT32 lines = 0;
            lay->GetLineMetrics(&line, 1, &lines);
            const float pct_w = text_width(L"%", f_body_.Get(), 40);
            const float total = m.width + 3 + pct_w;
            const float left = c.x - total / 2;
            const float top = c.y - line.height / 2;  // the line box centred on the ring
            brush_->SetColor(w.muted ? kMuted : kPaper);
            rt_->DrawTextLayout(D2D1::Point2F(left, top), lay.Get(), brush_.Get());
            // "%" sits on the number's baseline.
            const float base = top + line.baseline;
            ComPtr<IDWriteTextLayout> pl;
            dw_->CreateTextLayout(L"%", 1, f_body_.Get(), 40, 40, &pl);
            if (pl) {
                pl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                DWRITE_LINE_METRICS plm{};
                UINT32 pn = 0;
                pl->GetLineMetrics(&plm, 1, &pn);
                brush_->SetColor(kMuted);
                rt_->DrawTextLayout(D2D1::Point2F(left + m.width + 3, base - plm.baseline), pl.Get(), brush_.Get());
            }
            break;
        }
        case K::Card: {
            round_fill(r, 24, hover ? kHover : kSurface);
            const float cx = (r.left + r.right) / 2, cy = r.top + kRingCy;
            const bool active = drag_ >= 0 && widgets_[size_t(drag_)].key == w.key;
            draw_ring(D2D1::Point2F(cx, cy), 46, active ? 6.f : 4.f, arc_fraction(w), w.muted ? kEdge : kAccent,
                      active ? 7.f : 0.f);
            if (active) {
                draw_text(pct(shown_volume(w)), D2D1::RectF(cx - 40, cy - 20, cx + 40, cy + 20), f_pct_.Get(), kPaper,
                          DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                round_fill(D2D1::RectF(cx - 24, cy - 24, cx + 24, cy + 24), 14, kRaised);
                if (w.input) {
                    draw_glyph(w.muted ? kGlyphMicOff : kGlyphMic, D2D1::RectF(cx - 24, cy - 24, cx + 24, cy + 24), 20,
                               w.muted ? kMuted : kPaper);
                } else if (ID2D1Bitmap* b = icon_bitmap(w.icon_key)) {
                    rt_->DrawBitmap(b, D2D1::RectF(cx - 16, cy - 16, cx + 16, cy + 16), w.muted ? 0.45f : 1.f);
                } else {
                    const std::wstring mono = w.name.empty() ? L"?" : std::wstring(1, wchar_t(towupper(w.name[0])));
                    draw_text(mono, D2D1::RectF(cx - 24, cy - 24, cx + 24, cy + 24), f_body_.Get(),
                              w.muted ? rgb(0x9A9A9A, 0.45f) : kMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
                }
            }
            // Name (two lines for tabs), with the playing dot following the live level.
            const float ny = r.top + kNameTop;
            const bool dot = w.playing && !w.muted;
            // The name is centred; the playing dot sits right before its first line (8 px, 6 px gap).
            const float avail = (r.right - r.left) - 28 - (dot ? 14.f : 0.f);
            ComPtr<IDWriteTextLayout> name;
            dw_->CreateTextLayout(w.name.c_str(), UINT32(w.name.size()), f_caption_.Get(), avail, 40, &name);
            float text_w = avail, name_h = 18, line1 = 9;
            if (name) {
                name->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);  // one line; the full title is in the UIA name
                name->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                name->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                ComPtr<IDWriteInlineObject> ellipsis;
                dw_->CreateEllipsisTrimmingSign(name.Get(), &ellipsis);
                name->SetTrimming(&trim, ellipsis.Get());
                DWRITE_TEXT_METRICS m{};
                name->GetMetrics(&m);
                text_w = std::min(avail, m.width);
                name_h = std::min(m.height, 36.f);  // at most two lines
                DWRITE_LINE_METRICS lm[2]{};
                UINT32 n = 0;
                name->GetLineMetrics(lm, 2, &n);
                if (n > 0) line1 = lm[0].height / 2;
                name->SetMaxWidth(text_w + 1);
                name->SetMaxHeight(name_h + 1);
            }
            const float block = text_w + (dot ? 14.f : 0.f);
            const float left = std::floor((r.left + r.right) / 2 - block / 2);
            if (dot) {
                const float lvl = w.meter ? model_.level(w.meter) : 1.f;
                brush_->SetColor(kOk);
                brush_->SetOpacity(reduce_motion_ ? 1.f : 0.55f + 0.45f * std::clamp(lvl, 0.f, 1.f));
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left + 4, ny + line1), 4, 4), brush_.Get());
                brush_->SetOpacity(1.f);
            }
            if (name) {
                brush_->SetColor(w.muted ? kMuted : kPaper);
                rt_->DrawTextLayout(D2D1::Point2F(left + (dot ? 14.f : 0.f), ny), name.Get(), brush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            if (!w.sub.empty())
                draw_text(w.sub, D2D1::RectF(r.left + 14, ny + name_h + 2, r.right - 14, ny + name_h + 22), f_small_.Get(),
                          kMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        }
        case K::Mute: {
            const D2D1_POINT_2F c = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
            brush_->SetColor(w.on ? kError : (hover ? rgb(0x2A2A2A) : kRaised));
            rt_->FillEllipse(D2D1::Ellipse(c, 18, 18), brush_.Get());
            draw_glyph(w.on ? kGlyphMute : kGlyphVolume, r, 14, w.on ? kInk : kPaper);
            break;
        }
        case K::PanelClose:
        case K::ProfileEdit: {
            brush_->SetColor(hover ? rgb(0x2A2A2A) : kRaised);
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2), 20, 20),
                             brush_.Get());
            draw_glyph(w.glyph, r, 14, kPaper);
            break;
        }
        case K::ProfileRow: {
            round_fill(r, 20, kRaised);
            const float tx = r.left + 20;  // name and summary share one column; the in-use dot hangs left of it
            if (w.on) {
                brush_->SetColor(kAccent);
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 10, r.top + 24), 3, 3), brush_.Get());
            }
            draw_text(w.name, D2D1::RectF(tx, r.top + 12, r.right - 150, r.top + 36), f_body_.Get(), kPaper);
            draw_text(w.sub, D2D1::RectF(tx, r.top + 36, r.right - 150, r.bottom - 8), f_small_.Get(), kMuted);
            break;
        }
        case K::ProfileApply:
            if (w.on) {  // the profile already in force: a label, not a button
                draw_text(pt_ ? L"Em uso" : L"In use", r, f_caption_.Get(), kAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                round_fill(r, (r.bottom - r.top) / 2, hover ? rgb(0x2A2A2A) : rgb(0x262626));
                draw_text(pt_ ? L"Aplicar" : L"Apply", r, f_caption_.Get(), kPaper, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            break;
        case K::PrimaryButton:
            round_fill(r, (r.bottom - r.top) / 2, hover ? rgb(0xFFF1B8) : kAccent);
            draw_text(w.name, r, f_body_.Get(), kInk, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        case K::SecondaryButton:
            round_fill(r, (r.bottom - r.top) / 2, hover ? rgb(0x2A2A2A) : kRaised);
            draw_text(w.name, r, f_body_.Get(), kPaper, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        case K::DangerButton:
            draw_text(w.name, r, f_body_.Get(), kError, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        case K::Toggle: {
            draw_text(w.name, D2D1::RectF(r.left, r.top + (w.sub.empty() ? 0.f : 4.f), r.right - 64,
                                          w.sub.empty() ? r.bottom : r.top + 28),
                      f_body_.Get(), kPaper);
            if (!w.sub.empty()) draw_text(w.sub, D2D1::RectF(r.left, r.top + 28, r.right - 64, r.bottom - 2), f_small_.Get(), kMuted);
            const float cy = (r.top + r.bottom) / 2;
            const D2D1_RECT_F tr = D2D1::RectF(r.right - 52, cy - 14, r.right, cy + 14);
            round_fill(tr, 14, w.on ? kAccent : kTrack);
            if (!w.on) round_edge(tr, 14);
            brush_->SetColor(w.on ? kInk : kMuted);
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(w.on ? tr.right - 14 : tr.left + 14, cy), 10, 10), brush_.Get());
            break;
        }
        case K::DayChip:
            brush_->SetColor(w.on ? kAccent : kRaised);
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2), 19, 19),
                             brush_.Get());
            if (!w.on) {
                brush_->SetColor(kEdge);
                rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2), 18.5f, 18.5f),
                                 brush_.Get(), 1);
            }
            draw_text(w.glyph, r, f_caption_.Get(), w.on ? kInk : kPaper, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        case K::TimeStepper: {
            round_fill(r, 16, hover ? rgb(0x2A2A2A) : kRaised);
            draw_text(w.name, D2D1::RectF(r.left + 14, r.top + 4, r.right - 50, r.top + 22), f_small_.Get(), kMuted);
            wchar_t b[8];
            swprintf_s(b, L"%02d:%02d", w.minutes / 60, w.minutes % 60);
            draw_text(b, D2D1::RectF(r.left + 14, r.top + 22, r.right - 50, r.bottom - 4), f_body_.Get(), kPaper);
            draw_text(L"\x2212", D2D1::RectF(r.right - 56, r.top, r.right - 30, r.bottom), f_body_.Get(), kMuted,
                      DWRITE_TEXT_ALIGNMENT_CENTER);
            draw_text(L"+", D2D1::RectF(r.right - 30, r.top, r.right - 4, r.bottom), f_body_.Get(), kMuted,
                      DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        }
        case K::Choice:
            draw_text(w.name, D2D1::RectF(r.left, r.top + 4, r.right - 30, r.top + 28), f_body_.Get(), kPaper);
            draw_text(w.sub, D2D1::RectF(r.left, r.top + 28, r.right - 30, r.bottom - 2), f_small_.Get(), kAccent);
            draw_glyph(w.glyph, D2D1::RectF(r.right - 28, r.top, r.right, r.bottom), 12, kMuted);
            break;
        case K::DeviceRow:
            round_fill(r, 20, kRaised);
            draw_text(w.sub, D2D1::RectF(r.left + 16, r.top, r.right - 120, r.bottom), f_body_.Get(), kPaper);
            draw_text(pt_ ? L"Remover" : L"Remove", D2D1::RectF(r.right - 116, r.top, r.right - 12, r.bottom),
                      f_caption_.Get(), hover ? kError : kMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
            break;
    }
}

void MixerWindow::paint() {
    if (!ensure_target()) return;
    layout();
    rt_->BeginDraw();
    rt_->Clear(kBg);
    const D2D1_RECT_F all = client_dips();
    // Body first (grid clipped to its area), then the sheet, then focus.
    bool grid_clipped = false;
    for (const Widget& w : widgets_) {
        const bool in_panel = panel_ != Panel::None && w.r.left >= panel_clip_.left - 0.5f && w.r.top >= kTop &&
                              (w.kind == Widget::Kind::PanelClose || w.key.rfind("p:", 0) == 0 ||
                               w.key.rfind("e:", 0) == 0 || w.key.rfind("s:", 0) == 0);
        if (in_panel) continue;
        if (w.scrolls && !grid_clipped) {
            rt_->PushAxisAlignedClip(grid_clip_, D2D1_ANTIALIAS_MODE_ALIASED);
            grid_clipped = true;
        } else if (!w.scrolls && grid_clipped) {
            rt_->PopAxisAlignedClip();
            grid_clipped = false;
        }
        if (!w.clipped) draw_widget(w);
    }
    if (grid_clipped) rt_->PopAxisAlignedClip();
    if (scroll_max_ > 0) {  // the grid scrolls: show where (3 px, track + edge thumb)
        const float top = grid_clip_.top + 8, bottom = grid_clip_.bottom - 8, h = bottom - top;
        const float view = grid_clip_.bottom - grid_clip_.top;
        const float th = std::max(32.f, h * view / (view + scroll_max_));
        const float ty = top + (h - th) * (scroll_ / scroll_max_);
        const float x = grid_clip_.right - 8;
        brush_->SetColor(kTrack);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, top, x + 3, bottom), 1.5f, 1.5f), brush_.Get());
        brush_->SetColor(kEdge);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, ty, x + 3, ty + th), 1.5f, 1.5f), brush_.Get());
    }
    if (panel_ != Panel::None) {
        if (all.right < 760) {  // narrow: the sheet covers the body
        } else {
            brush_->SetColor(kTrack);
            rt_->FillRectangle(D2D1::RectF(panel_clip_.left - 1, kTop, panel_clip_.left, all.bottom), brush_.Get());
        }
        brush_->SetColor(kSurface);
        rt_->FillRectangle(panel_clip_, brush_.Get());
        rt_->PushAxisAlignedClip(panel_clip_, D2D1_ANTIALIAS_MODE_ALIASED);
        for (const Widget& w : widgets_) {
            const bool in_panel = w.r.left >= panel_clip_.left - 0.5f && w.r.top >= kTop &&
                                  (w.kind == Widget::Kind::PanelClose || w.key.rfind("p:", 0) == 0 ||
                                   w.key.rfind("e:", 0) == 0 || w.key.rfind("s:", 0) == 0);
            if (in_panel && !w.clipped) draw_widget(w);
        }
        rt_->PopAxisAlignedClip();
    }
    // Focus ring (keyboard only): 2 px accent, 3 px outside the control's shape.
    if (focus_visible_ && focus_ >= 0 && focus_ < int(widgets_.size())) {
        const Widget& w = widgets_[size_t(focus_)];
        D2D1_RECT_F r = w.r;
        r.left -= 3, r.top -= 3, r.right += 3, r.bottom += 3;
        brush_->SetColor(kAccent);
        using K = Widget::Kind;
        const bool circle = w.kind == K::MasterRing || w.kind == K::Mute || w.kind == K::IconButton ||
                            w.kind == K::PanelClose || w.kind == K::ProfileEdit || w.kind == K::DayChip;
        if (circle) {
            const float rad = std::min(r.right - r.left, r.bottom - r.top) / 2;
            rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2), rad, rad),
                             brush_.Get(), 2);
        } else {
            const float rad = w.kind == K::Card ? 27.f : (r.bottom - r.top) / 2 > 24 ? 22.f : (r.bottom - r.top) / 2;
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush_.Get(), 2);
        }
    }
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) discard_target();
    place_name_edit();
    if (animating()) SetTimer(hwnd_, kAnimTimer, 16, nullptr);
    else KillTimer(hwnd_, kAnimTimer);
}

bool MixerWindow::animating() const {
    if (reduce_motion_) return false;
    const auto now = Clock::now();
    if (panel_ != Panel::None && now - panel_at_ < std::chrono::milliseconds(240)) return true;
    for (auto& [k, v] : arcs_)
        if (v.second != Clock::time_point{} && v.second != Clock::time_point::min() &&
            now - v.second < std::chrono::milliseconds(260))
            return true;
    return false;
}

void MixerWindow::tick() { invalidate(); }

// ---- input --------------------------------------------------------------------------------

int MixerWindow::hit(float x, float y) const {
    // Later widgets are on top (mute buttons over cards, the sheet over the body).
    for (int i = int(widgets_.size()) - 1; i >= 0; --i) {
        const Widget& w = widgets_[size_t(i)];
        if (!w.focusable || w.clipped || !inside(w.r, x, y)) continue;
        if (w.scrolls && !inside(grid_clip_, x, y)) continue;
        if (panel_ != Panel::None && inside(panel_clip_, x, y) && !inside(panel_clip_, (w.r.left + w.r.right) / 2, w.r.top + 1))
            continue;  // body widgets under the sheet
        return i;
    }
    return -1;
}

void MixerWindow::set_volume(const Widget& w, int volume, bool final) {
    volume = std::clamp(volume, 0, 10000);
    const std::string okey = (w.endpoint ? "ep:" : "sr:") + w.target;
    overrides_[okey] = Override{uint16_t(volume), Clock::now() + std::chrono::milliseconds(800)};
    // Coalesce: at most one command every 30 ms while dragging, always the last one.
    const auto now = Clock::now();
    if (final || now - last_sent_ > std::chrono::milliseconds(30)) {
        last_sent_ = now;
        send(arc::encode_set_volume(w.endpoint ? arc::MsgType::SetEndpointVolume : arc::MsgType::SetSourceVolume,
                                    w.target, uint16_t(volume)));
    }
    invalidate();
    if (uia_) uia_->value_changed(w.key, volume);
}

void MixerWindow::toggle_mute(const Widget& w) {
    bool muted = w.on;
    if (volume_kind(w.kind)) muted = w.muted;
    send(arc::encode_set_mute(w.endpoint ? arc::MsgType::SetEndpointMute : arc::MsgType::SetSourceMute, w.target, !muted));
}

void MixerWindow::on_mouse_down(float x, float y) {
    focus_visible_ = false;
    const int i = hit(x, y);
    if (i < 0) return;
    Widget& w = widgets_[size_t(i)];
    focus_ = i;
    focus_key_ = w.key;
    if (volume_kind(w.kind)) {
        drag_ = i;
        drag_key_ = (w.endpoint ? "ep:" : "sr:") + w.target;
        drag_x_ = x;
        drag_from_ = shown_volume(w);
        SetCapture(hwnd_);
        if (w.kind == Widget::Kind::MasterRing) on_mouse_move(x, y, true);  // the ring jumps to the pointer
        invalidate();
        return;
    }
    using K = Widget::Kind;
    if (w.kind == K::Mute) return toggle_mute(w);
    if (w.kind == K::TimeStepper) {
        const int step = (x > w.r.right - 30) ? 15 : (x > w.r.right - 56) ? -15 : 0;
        if (step != 0) {
            uint16_t& t = w.key == "e:from" ? editor_.p.start_min : editor_.p.end_min;
            t = uint16_t((t + step + 1440) % 1440);
            invalidate();
        }
        return;
    }
    if (w.invoke) w.invoke();
    invalidate();
}

void MixerWindow::on_mouse_move(float x, float y, bool left) {
    if (drag_ >= 0 && left) {
        const Widget& w = widgets_[size_t(drag_)];
        int v;
        if (w.kind == Widget::Kind::MasterRing) {
            const float cx = (w.r.left + w.r.right) / 2, cy = (w.r.top + w.r.bottom) / 2;
            float deg = std::atan2(y - cy, x - cx) * 180.f / kPi;
            if (deg < 0) deg += 360;
            float rel = std::fmod(deg - 135 + 360, 360.f);
            if (rel > 270) rel = rel > 315 ? 0.f : 270.f;  // the gap at the bottom snaps to the nearest end
            v = int(rel / 270.f * 10000);
        } else {
            v = int(drag_from_) + int((x - drag_x_) / 220.f * 10000);
        }
        set_volume(w, v, false);
        return;
    }
    const int h = hit(x, y);
    if (h != hover_) {
        hover_ = h;
        invalidate();
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&t);
    }
}

void MixerWindow::on_mouse_up(float, float) {
    if (drag_ >= 0) {
        const Widget& w = widgets_[size_t(drag_)];
        set_volume(w, shown_volume(w), true);
        drag_ = -1;
        drag_key_.clear();
        ReleaseCapture();
        invalidate();
    }
}

void MixerWindow::on_wheel(float x, float y, int delta, bool fine) {
    const int i = hit(x, y);
    const int notches = delta / WHEEL_DELTA ? delta / WHEEL_DELTA : (delta > 0 ? 1 : -1);
    if (i >= 0) {
        Widget& w = widgets_[size_t(i)];
        // The wheel changes a volume only over its ring; elsewhere it scrolls.
        bool on_ring = false;
        if (w.kind == Widget::Kind::MasterRing) on_ring = true;
        if (w.kind == Widget::Kind::Card) {
            const float cx = (w.r.left + w.r.right) / 2, cy = w.r.top + kRingCy;
            on_ring = (x - cx) * (x - cx) + (y - cy) * (y - cy) <= 52 * 52;
        }
        if (on_ring) {
            set_volume(w, shown_volume(w) + notches * (fine ? 100 : 500), true);
            return;
        }
        if (w.kind == Widget::Kind::TimeStepper) {
            uint16_t& t = w.key == "e:from" ? editor_.p.start_min : editor_.p.end_min;
            t = uint16_t((t + notches * (fine ? 1 : 15) + 1440) % 1440);
            invalidate();
            return;
        }
    }
    if (panel_ != Panel::None && inside(panel_clip_, x, y)) {
        pscroll_ = std::clamp(pscroll_ - float(notches) * 60, 0.f, pscroll_max_);
    } else {
        scroll_ = std::clamp(scroll_ - float(notches) * 60, 0.f, scroll_max_);
    }
    invalidate();
}

void MixerWindow::move_focus(int dir) {
    if (widgets_.empty()) return;
    const int n = int(widgets_.size());
    int i = focus_;
    for (int k = 0; k < n; ++k) {
        i = (i + dir + n) % n;
        if (i < 0) i = n - 1;
        if (widgets_[size_t(i)].focusable) {
            focus_ = i;
            focus_key_ = widgets_[size_t(i)].key;
            ensure_visible(i);
            if (focus_key_ == "e:name" && name_edit_) SetFocus(name_edit_);
            if (uia_) uia_->focus_changed(focus_key_);
            invalidate();
            return;
        }
    }
}

void MixerWindow::ensure_visible(int index) {
    const Widget& w = widgets_[size_t(index)];
    if (w.scrolls) {
        if (w.r.top < grid_clip_.top + 8) scroll_ = std::max(0.f, scroll_ - (grid_clip_.top + 8 - w.r.top));
        if (w.r.bottom > grid_clip_.bottom - 8) scroll_ = std::min(scroll_max_, scroll_ + (w.r.bottom - grid_clip_.bottom + 8));
    } else if (panel_ != Panel::None && inside(panel_clip_, w.r.left + 1, std::max(w.r.top, panel_clip_.top) + 1)) {
        if (w.r.top < panel_clip_.top + 8) pscroll_ = std::max(0.f, pscroll_ - (panel_clip_.top + 8 - w.r.top));
        if (w.r.bottom > panel_clip_.bottom - 8) pscroll_ = std::min(pscroll_max_, pscroll_ + (w.r.bottom - panel_clip_.bottom + 8));
    }
}

void MixerWindow::on_key(WPARAM vk, bool shift) {
    focus_visible_ = true;
    if (vk == VK_TAB) return move_focus(shift ? -1 : 1);
    if (vk == VK_ESCAPE) {
        if (panel_ == Panel::Editor) open_panel(Panel::Profiles);
        else if (panel_ != Panel::None) close_panel();
        return;
    }
    if (focus_ < 0 || focus_ >= int(widgets_.size())) {
        if (vk == VK_DOWN || vk == VK_RIGHT) move_focus(1);
        invalidate();
        return;
    }
    Widget& w = widgets_[size_t(focus_)];
    using K = Widget::Kind;
    if (volume_kind(w.kind)) {
        const int step = shift ? 100 : 500;
        const int v = shown_volume(w);
        switch (vk) {
            case VK_RIGHT: case VK_UP: return set_volume(w, v + step, true);
            case VK_LEFT: case VK_DOWN: return set_volume(w, v - step, true);
            case VK_PRIOR: return set_volume(w, v + 1000, true);
            case VK_NEXT: return set_volume(w, v - 1000, true);
            case VK_HOME: return set_volume(w, 0, true);
            case VK_END: return set_volume(w, 10000, true);
            case VK_SPACE: case 'M': return toggle_mute(w);
            case VK_APPS: case VK_F10: {
                if (vk == VK_F10 && !shift) return;
                POINT p{LONG((w.r.left + 20) * scale_), LONG((w.r.top + 40) * scale_)};
                ClientToScreen(hwnd_, &p);
                return route_menu(w, p);
            }
            default: return;
        }
    }
    if (w.kind == K::TimeStepper) {
        uint16_t& t = w.key == "e:from" ? editor_.p.start_min : editor_.p.end_min;
        const int step = shift ? 1 : 15;
        if (vk == VK_RIGHT || vk == VK_UP) t = uint16_t((t + step) % 1440);
        if (vk == VK_LEFT || vk == VK_DOWN) t = uint16_t((t + 1440 - step) % 1440);
        invalidate();
        return;
    }
    if (vk == VK_SPACE || vk == VK_RETURN) {
        if (w.kind == K::Mute) toggle_mute(w);
        else if (w.invoke) w.invoke();
        invalidate();
    }
}

void MixerWindow::route_menu(const Widget& w, POINT screen) {
    if (w.kind != Widget::Kind::Card || w.endpoint || w.tab || w.system || !model_.can_route()) return;
    auto s = model_.sources.find(w.target);
    if (s == model_.sources.end()) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, pt_ ? L"Saída deste app" : L"This app's output");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (s->second.route.empty() ? MF_CHECKED : 0), 1, pt_ ? L"Padrão do PC" : L"PC default");
    const auto outs = model_.outputs();
    for (size_t i = 0; i < outs.size(); ++i)
        AppendMenuW(m, MF_STRING | (outs[i]->id == s->second.route ? MF_CHECKED : 0), UINT(i + 2), wstr(outs[i]->name).c_str());
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == 1) send(arc::encode_set_route(w.target, ""));
    else if (cmd >= 2 && cmd - 2 < outs.size()) send(arc::encode_set_route(w.target, outs[cmd - 2]->id));
}

void MixerWindow::output_menu(POINT screen) {
    const auto outs = model_.outputs();
    if (outs.empty()) return;
    const arc::EndpointInfo* cur = model_.output_or_default(output_);
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < outs.size(); ++i) {
        std::wstring label = wstr(outs[i]->name);
        if (outs[i]->is_default) label += pt_ ? L"  \x00B7  padrão" : L"  \x00B7  default";
        AppendMenuW(m, MF_STRING | (outs[i] == cur ? MF_CHECKED : 0), UINT(i + 1), label.c_str());
    }
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd >= 1 && cmd - 1 < outs.size()) {
        output_ = outs[cmd - 1]->id;
        scroll_ = 0;
        invalidate();
    }
}

// ---- panels -----------------------------------------------------------------------------

void MixerWindow::open_panel(Panel p) {
    const bool was_open = panel_ != Panel::None;
    panel_ = p;
    if (!was_open) panel_at_ = Clock::now();
    pscroll_ = 0;
    if (p != Panel::Editor && name_edit_) {
        DestroyWindow(name_edit_);
        name_edit_ = nullptr;
    }
    focus_key_ = p == Panel::Editor ? "e:name" : "panel:close";
    invalidate();
}

void MixerWindow::close_panel() {
    panel_ = Panel::None;
    if (name_edit_) {
        DestroyWindow(name_edit_);
        name_edit_ = nullptr;
    }
    focus_key_.clear();
    SetFocus(hwnd_);
    invalidate();
}

void MixerWindow::edit_profile(const arc::Profile* p) {
    editor_ = Editor{};
    if (p) {
        editor_.p = *p;
    } else {
        editor_.is_new = true;
        editor_.p.name = to_utf8((pt_ ? L"Perfil " : L"Profile ") + std::to_wstring(model_.profiles.size() + 1));
    }
    editor_.use_time = editor_.p.has_time();
    if (editor_.p.start_min == editor_.p.end_min) {
        editor_.p.start_min = 9 * 60;
        editor_.p.end_min = 18 * 60;
    }
    editor_.use_network = !editor_.p.network.empty();
    open_panel(Panel::Editor);
    name_edit_ = CreateWindowExW(0, L"EDIT", to_wide(editor_.p.name).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 0, 0, 10, 10, hwnd_, nullptr, inst_, nullptr);
    SendMessageW(name_edit_, WM_SETFONT, WPARAM(edit_font_), TRUE);
    SendMessageW(name_edit_, EM_LIMITTEXT, 64, 0);
    SetWindowTextW(name_edit_, to_wide(editor_.p.name).c_str());
    // Tab / Esc inside the box hand the keyboard back to the window.
    SetWindowSubclass(
        name_edit_,
        [](HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) -> LRESULT {
            auto* self = reinterpret_cast<MixerWindow*>(ref);
            if (m == WM_KEYDOWN && (w == VK_TAB || w == VK_ESCAPE)) {
                SetFocus(self->hwnd_);
                self->on_key(w, GetKeyState(VK_SHIFT) < 0);
                return 0;
            }
            if (m == WM_CHAR && (w == L'\t' || w == 27)) return 0;
            if (m == WM_KEYDOWN && w == VK_RETURN) {
                self->save_editor(self->editor_.is_new);
                return 0;
            }
            return DefSubclassProc(h, m, w, l);
        },
        1, DWORD_PTR(this));
    SetFocus(name_edit_);
    SendMessageW(name_edit_, EM_SETSEL, 0, -1);
}

void MixerWindow::place_name_edit() {
    if (!name_edit_) return;
    for (const Widget& w : widgets_)
        if (w.key == "e:name") {
            const int x = int((w.r.left + 12) * scale_), y = int((w.r.top + 11) * scale_);
            const int wd = int((w.r.right - w.r.left - 24) * scale_), h = int(22 * scale_);
            const bool show = !w.clipped && w.r.top >= panel_clip_.top && w.r.bottom <= panel_clip_.bottom;
            RECT old{};
            GetWindowRect(name_edit_, &old);
            const bool first = old.right - old.left < 20;
            SetWindowPos(name_edit_, HWND_TOP, x, y, wd, h, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
            if (first) {  // created tiny: the text scrolled to its end; show it from the start, all selected
                wchar_t buf[80] = {0};
                GetWindowTextW(name_edit_, buf, 80);
                SetWindowTextW(name_edit_, buf);
                SendMessageW(name_edit_, EM_SETSEL, 0, -1);
            }
            return;
        }
}

void MixerWindow::save_editor(bool capture) {
    wchar_t buf[80] = {0};
    if (name_edit_) GetWindowTextW(name_edit_, buf, 80);
    std::wstring name = buf;
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    if (name.empty()) {
        MessageBeep(MB_ICONWARNING);
        if (name_edit_) SetFocus(name_edit_);
        return;
    }
    arc::Profile p = editor_.p;
    p.name = to_utf8(name);
    if (editor_.is_new) p.id.clear();
    if (!p.automatic || !editor_.use_time) {
        p.days = 0;
        p.start_min = p.end_min = 0;
    }
    p.network = p.automatic && editor_.use_network ? (p.network.empty() ? model_.network : p.network) : "";
    if (!p.automatic) {
        p.output_id.clear();
        p.output_name.clear();
    }
    // A rules edit keeps the profile's settings on the PC (empty lists); capture takes the mixer now.
    p.endpoints.clear();
    p.apps.clear();
    p.active = false;
    send(arc::encode_profile(arc::MsgType::SaveProfile, p, capture ? arc::kProfileFlagCapture : 0));
    open_panel(Panel::Profiles);
}

// ---- UI Automation entry points ------------------------------------------------------------

void MixerWindow::uia_focus(const std::string& key) {
    for (size_t i = 0; i < widgets_.size(); ++i)
        if (widgets_[i].key == key && widgets_[i].focusable) {
            focus_ = int(i);
            focus_key_ = key;
            focus_visible_ = true;
            ensure_visible(int(i));
            invalidate();
        }
}

void MixerWindow::uia_invoke(const std::string& key) {
    for (Widget& w : widgets_)
        if (w.key == key) {
            if (w.kind == Widget::Kind::Mute) toggle_mute(w);
            else if (w.invoke) w.invoke();
            invalidate();
            return;
        }
}

void MixerWindow::uia_set_volume(const std::string& key, double percent) {
    for (Widget& w : widgets_)
        if (w.key == key && volume_kind(w.kind)) return set_volume(w, int(percent * 100), true);
}

void MixerWindow::uia_toggle(const std::string& key) {
    for (Widget& w : widgets_)
        if (w.key == key) {
            if (w.kind == Widget::Kind::Mute || volume_kind(w.kind)) toggle_mute(w);
            else if (w.invoke) w.invoke();
            invalidate();
            return;
        }
}

// ---- window procedure ----------------------------------------------------------------------

LRESULT CALLBACK MixerWindow::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        reinterpret_cast<MixerWindow*>(cs->lpCreateParams)->hwnd_ = h;
    }
    auto* self = reinterpret_cast<MixerWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT MixerWindow::handle(UINT m, WPARAM w, LPARAM l) {
    const float x = float(GET_X_LPARAM(l)) / scale_, y = float(GET_Y_LPARAM(l)) / scale_;
    switch (m) {
        case WM_APP_MODEL:
            drain_messages();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            paint();
            EndPaint(hwnd_, &ps);
            if (uia_) uia_->structure_maybe_changed();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
            invalidate();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            mm->ptMinTrackSize.x = LONG(400 * scale_ + 16);
            mm->ptMinTrackSize.y = LONG(560 * scale_ + 39);
            return 0;
        }
        case WM_DPICHANGED: {
            scale_ = float(HIWORD(w)) / 96.f;
            make_formats();
            discard_target();
            auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_TIMER:
            if (w == kAnimTimer) tick();
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(hwnd_);
            on_mouse_down(x, y);
            return 0;
        case WM_LBUTTONDBLCLK:
            on_mouse_down(x, y);
            return 0;
        case WM_MOUSEMOVE:
            on_mouse_move(x, y, (w & MK_LBUTTON) != 0);
            return 0;
        case WM_LBUTTONUP:
            on_mouse_up(x, y);
            return 0;
        case WM_MOUSELEAVE:
            hover_ = -1;
            invalidate();
            return 0;
        case WM_CAPTURECHANGED:
            if (drag_ >= 0 && HWND(l) != hwnd_) on_mouse_up(0, 0);
            return 0;
        case WM_RBUTTONUP: {
            const int i = hit(x, y);
            if (i >= 0) {
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ClientToScreen(hwnd_, &p);
                route_menu(widgets_[size_t(i)], p);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(hwnd_, &p);
            on_wheel(float(p.x) / scale_, float(p.y) / scale_, GET_WHEEL_DELTA_WPARAM(w), (GET_KEYSTATE_WPARAM(w) & MK_SHIFT) != 0);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (m == WM_SYSKEYDOWN && w != VK_F10) break;
            on_key(w, GetKeyState(VK_SHIFT) < 0);
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = HDC(w);
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkColor(dc, RGB(0x1C, 0x1C, 0x1C));
            return LRESULT(edit_brush_);
        }
        case WM_SETTINGCHANGE: {
            BOOL anim = TRUE;
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
            reduce_motion_ = !anim;
            invalidate();
            return 0;
        }
        case WM_GETOBJECT:
            if (LONG(l) == LONG(UiaRootObjectId)) {
                if (!uia_) uia_ = new MixerUia(this);
                return uia_->return_root(hwnd_, w, l);
            }
            break;
        case WM_CLOSE:
            hide();  // the agent keeps running in the tray
            return 0;
        case WM_DESTROY:
            if (uia_) {
                uia_->disconnect();
                uia_ = nullptr;
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

}  // namespace agent
