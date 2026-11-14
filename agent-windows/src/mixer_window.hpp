#pragma once
// The Archon window on Windows: the same mixer as the phone (docs/design.md,
// "Desktop window"), drawn with Direct2D/DirectWrite, fed by the server's
// local view. Native only: Win32, Direct2D, DirectWrite, WIC, UI Automation.

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "archon/server.hpp"
#include "desktop_model.hpp"

namespace agent {

class MixerUia;  // UI Automation (mixer_uia.cpp)

// What the window shows outside the mixer itself; filled by the tray UI.
struct WindowHost {
    std::function<std::vector<arc::PairedController>()> paired;  // paired phones
    std::function<void(const arc::Id16&)> forget;                // remove a paired phone
    std::function<bool()> autostart;
    std::function<void(bool)> set_autostart;
    std::function<int()> browsers;     // browsers connected through the extension (-1 = bridge off)
    std::function<void()> open_log;
    bool hotkey = false;               // Ctrl+Alt+M registered
};

// One thing on screen the user can reach: drawn, hit-tested, focused with the
// keyboard and exposed to screen readers. Rebuilt by every layout.
struct Widget {
    enum class Kind {
        OutputPill, IconButton, MasterRing, Card, Mute, Header, Text, PanelClose, ProfileRow, ProfileApply,
        ProfileEdit, PrimaryButton, SecondaryButton, DangerButton, Toggle, DayChip, TimeStepper, Choice, DeviceRow
    };
    Kind kind = Kind::Text;
    std::string key;            // stable identity (focus, screen readers)
    D2D1_RECT_F r{};            // in DIPs, window coordinates (scroll already applied)
    std::wstring name;          // label read by screen readers
    std::wstring sub;           // second line / help text
    std::wstring glyph;         // Segoe Fluent Icons code point(s)
    bool focusable = true;
    bool on = false;            // toggled / muted / selected
    bool danger = false;        // panic on, destructive
    bool scrolls = false;       // inside the scrolling grid
    bool clipped = false;       // scrolled out of view
    // Volume controls (MasterRing, Card): what they drive.
    bool endpoint = false;
    std::string target;         // endpoint or source id
    uint16_t volume = 0;
    bool muted = false;
    bool playing = false;
    bool system = false;
    bool tab = false;
    bool input = false;
    std::string icon_key;
    uint16_t meter = 0;
    int minutes = 0;            // TimeStepper
    std::function<void()> invoke;  // buttons, rows, chips, toggles
};

class MixerWindow final : public arc::LocalView {
public:
    MixerWindow();
    ~MixerWindow() override;
    bool create(HINSTANCE inst, arc::Server* server, WindowHost host);
    void destroy();
    // Shows the window (creates it the first time). activate = bring it to the
    // front; false never takes focus from the app in use (e.g. a game).
    void show(bool activate = true);
    void hide();
    bool visible() const;
    // From the tray UI (UI thread).
    void set_connected(size_t phones);
    void set_streams(size_t listening);
    // Opens a panel by name: "profiles", "settings", "editor" (new profile) or "" (none).
    void open_named(const std::string& which);

    // arc::LocalView (server thread)
    void on_message(const arc::Bytes& msg) override;

    // For UI Automation (mixer_uia.cpp), UI thread.
    HWND hwnd() const { return hwnd_; }
    const std::vector<Widget>& widgets() const { return widgets_; }
    int focus_index() const { return focus_; }
    void uia_focus(const std::string& key);
    void uia_invoke(const std::string& key);
    void uia_set_volume(const std::string& key, double percent);
    void uia_toggle(const std::string& key);
    D2D1_RECT_F client_dips() const;
    float dpi_scale() const { return scale_; }

private:
    enum class Panel { None, Profiles, Settings, Editor };
    struct Override {
        uint16_t volume;
        std::chrono::steady_clock::time_point until;
    };
    struct Editor {
        arc::Profile p;
        bool is_new = false;
        bool use_time = false;
        bool use_network = false;
    };

    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);

    // drawing
    bool ensure_target();
    void discard_target();
    void paint();
    void layout();
    void layout_body(float x, float y, float w, float h);
    void layout_panel(float x, float y, float w, float h);
    void draw_widget(const Widget& w);
    void draw_ring(D2D1_POINT_2F c, float radius, float stroke, float fraction, D2D1_COLOR_F color, float thumb);
    void draw_text(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, D2D1_COLOR_F c,
                   DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool wrap = false);
    void draw_glyph(const std::wstring& g, const D2D1_RECT_F& r, float size, D2D1_COLOR_F c);
    ID2D1Bitmap* icon_bitmap(const std::string& key);
    void make_formats();

    // input
    int hit(float x, float y) const;
    void on_mouse_down(float x, float y);
    void on_mouse_move(float x, float y, bool left);
    void on_mouse_up(float x, float y);
    void on_wheel(float x, float y, int delta, bool fine);
    void on_key(WPARAM vk, bool shift);
    void move_focus(int dir);
    void ensure_visible(int index);
    void set_volume(const Widget& w, int volume, bool final);
    void toggle_mute(const Widget& w);
    void route_menu(const Widget& w, POINT screen);
    void output_menu(POINT screen);
    void open_panel(Panel p);
    void close_panel();
    void edit_profile(const arc::Profile* p);
    void save_editor(bool capture);
    void place_name_edit();
    void send(arc::Bytes msg);
    uint16_t shown_volume(const Widget& w) const;
    void drain_messages();
    void request_icons();
    void invalidate();
    void tick();
    bool animating() const;

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND name_edit_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    HFONT edit_font_ = nullptr;
    arc::Server* server_ = nullptr;
    WindowHost host_;
    float scale_ = 1.f;
    bool pt_ = true;
    bool reduce_motion_ = false;
    bool attached_ = false;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f_caption_, f_body_, f_title_, f_master_, f_pct_, f_small_;
    std::wstring icon_font_;
    std::map<std::string, Microsoft::WRL::ComPtr<ID2D1Bitmap>> bitmaps_;

    std::mutex in_mu_;
    std::deque<arc::Bytes> inbox_;
    bool posted_ = false;

    DesktopModel model_;
    std::string output_;           // the output shown ("" = default)
    size_t phones_ = 0;
    size_t streams_ = 0;
    std::vector<Widget> widgets_;
    int focus_ = -1;
    std::string focus_key_;
    int hover_ = -1;
    int drag_ = -1;
    std::string drag_key_;
    float drag_x_ = 0;
    uint16_t drag_from_ = 0;
    std::chrono::steady_clock::time_point last_sent_{};
    std::map<std::string, Override> overrides_;
    float scroll_ = 0, scroll_max_ = 0;    // card grid
    float pscroll_ = 0, pscroll_max_ = 0;  // open panel
    D2D1_RECT_F grid_clip_{};
    D2D1_RECT_F panel_clip_{};
    bool focus_visible_ = false;  // focus ring only after keyboard use
    std::map<std::string, float> arc_target_;
    std::vector<std::string> icons_requested_;
    Panel panel_ = Panel::None;
    std::chrono::steady_clock::time_point panel_at_{};
    Editor editor_;
    std::map<std::string, std::pair<float, std::chrono::steady_clock::time_point>> arcs_;  // animated arc start
    MixerUia* uia_ = nullptr;
    friend class MixerUia;
};

}  // namespace agent
