#include "mixer_uia.hpp"

#include <uiautomationcoreapi.h>

#include "mixer_window.hpp"

namespace agent {

namespace {

bool exposed_widget(const Widget& w) {
    // Everything the user can reach, plus headings and text lines; not what is scrolled away.
    return !w.clipped && !w.name.empty() && w.kind != Widget::Kind::ProfileRow;
}

VARIANT bstr(const std::wstring& s) {
    VARIANT v;
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(s.c_str());
    return v;
}

VARIANT i4(int x) {
    VARIANT v;
    v.vt = VT_I4;
    v.lVal = x;
    return v;
}

VARIANT boolean(bool b) {
    VARIANT v;
    v.vt = VT_BOOL;
    v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
    return v;
}

int key_hash(const std::string& k) {
    uint32_t h = 2166136261u;
    for (char c : k) h = (h ^ uint8_t(c)) * 16777619u;
    return int(h & 0x7fffffff);
}

}  // namespace

// ---- root -------------------------------------------------------------------------

MixerUia::MixerUia(MixerWindow* w) : w_(w) {}

MixerUia::~MixerUia() {
    for (auto& [k, e] : elements_) {
        e->detach();
        e->Release();
    }
}

LRESULT MixerUia::return_root(HWND h, WPARAM wp, LPARAM lp) { return UiaReturnRawElementProvider(h, wp, lp, this); }

void MixerUia::disconnect() {
    for (auto& [k, e] : elements_) UiaDisconnectProvider(e);
    UiaDisconnectProvider(this);
    w_ = nullptr;
    Release();
}

MixerElement* MixerUia::element(const std::string& key) {
    auto it = elements_.find(key);
    if (it != elements_.end()) return it->second;
    auto* e = new MixerElement(this, key);
    elements_[key] = e;
    return e;
}

std::vector<std::string> MixerUia::exposed() const {
    std::vector<std::string> keys;
    if (!w_) return keys;
    for (const Widget& w : w_->widgets())
        if (exposed_widget(w)) keys.push_back(w.key);
    return keys;
}

void MixerUia::focus_changed(const std::string& key) {
    if (!w_ || !UiaClientsAreListening()) return;
    UiaRaiseAutomationEvent(element(key), UIA_AutomationFocusChangedEventId);
}

void MixerUia::value_changed(const std::string& key, int volume) {
    if (!w_ || !UiaClientsAreListening()) return;
    VARIANT oldv, newv;
    oldv.vt = VT_EMPTY;
    newv.vt = VT_R8;
    newv.dblVal = double(volume) / 100.0;
    UiaRaiseAutomationPropertyChangedEvent(element(key), UIA_RangeValueValuePropertyId, oldv, newv);
}

void MixerUia::structure_maybe_changed() {
    if (!w_) return;
    std::vector<std::string> now = exposed();
    if (now == last_keys_) return;
    last_keys_ = std::move(now);
    if (UiaClientsAreListening()) UiaRaiseStructureChangedEvent(this, StructureChangeType_ChildrenInvalidated, nullptr, 0);
}

HRESULT MixerUia::QueryInterface(REFIID riid, void** out) {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
        *out = static_cast<IRawElementProviderSimple*>(this);
    else if (riid == __uuidof(IRawElementProviderFragment))
        *out = static_cast<IRawElementProviderFragment*>(this);
    else if (riid == __uuidof(IRawElementProviderFragmentRoot))
        *out = static_cast<IRawElementProviderFragmentRoot*>(this);
    else {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}
ULONG MixerUia::AddRef() { return ULONG(InterlockedIncrement(&refs_)); }
ULONG MixerUia::Release() {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return ULONG(n);
}

HRESULT MixerUia::get_ProviderOptions(ProviderOptions* r) {
    *r = ProviderOptions(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
    return S_OK;
}
HRESULT MixerUia::GetPatternProvider(PATTERNID, IUnknown** r) {
    *r = nullptr;
    return S_OK;
}
HRESULT MixerUia::GetPropertyValue(PROPERTYID id, VARIANT* r) {
    r->vt = VT_EMPTY;
    if (id == UIA_NamePropertyId) *r = bstr(L"Archon");
    else if (id == UIA_ControlTypePropertyId) *r = i4(UIA_PaneControlTypeId);
    return S_OK;
}
HRESULT MixerUia::get_HostRawElementProvider(IRawElementProviderSimple** r) {
    if (!w_) return UIA_E_ELEMENTNOTAVAILABLE;
    return UiaHostProviderFromHwnd(w_->hwnd(), r);
}
HRESULT MixerUia::Navigate(NavigateDirection d, IRawElementProviderFragment** r) {
    *r = nullptr;
    if (!w_) return UIA_E_ELEMENTNOTAVAILABLE;
    const auto keys = exposed();
    if (keys.empty()) return S_OK;
    if (d == NavigateDirection_FirstChild) *r = element(keys.front());
    else if (d == NavigateDirection_LastChild) *r = element(keys.back());
    if (*r) (*r)->AddRef();
    return S_OK;
}
HRESULT MixerUia::GetRuntimeId(SAFEARRAY** r) {
    *r = nullptr;  // the host HWND provides it
    return S_OK;
}
HRESULT MixerUia::get_BoundingRectangle(UiaRect* r) {
    *r = UiaRect{0, 0, 0, 0};  // the host HWND provides it
    return S_OK;
}
HRESULT MixerUia::GetEmbeddedFragmentRoots(SAFEARRAY** r) {
    *r = nullptr;
    return S_OK;
}
HRESULT MixerUia::SetFocus() { return S_OK; }
HRESULT MixerUia::get_FragmentRoot(IRawElementProviderFragmentRoot** r) {
    *r = this;
    AddRef();
    return S_OK;
}
HRESULT MixerUia::ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** r) {
    *r = nullptr;
    if (!w_) return UIA_E_ELEMENTNOTAVAILABLE;
    POINT p{LONG(x), LONG(y)};
    ScreenToClient(w_->hwnd(), &p);
    const float s = w_->dpi_scale();
    const auto& ws = w_->widgets();
    for (int i = int(ws.size()) - 1; i >= 0; --i) {
        const Widget& w = ws[size_t(i)];
        if (!exposed_widget(w)) continue;
        if (float(p.x) / s >= w.r.left && float(p.x) / s < w.r.right && float(p.y) / s >= w.r.top && float(p.y) / s < w.r.bottom) {
            *r = element(w.key);
            (*r)->AddRef();
            return S_OK;
        }
    }
    return S_OK;  // the root itself
}
HRESULT MixerUia::GetFocus(IRawElementProviderFragment** r) {
    *r = nullptr;
    if (!w_) return UIA_E_ELEMENTNOTAVAILABLE;
    const int f = w_->focus_index();
    if (f >= 0 && f < int(w_->widgets().size())) {
        *r = element(w_->widgets()[size_t(f)].key);
        (*r)->AddRef();
    }
    return S_OK;
}

// ---- elements ------------------------------------------------------------------------

MixerElement::MixerElement(MixerUia* root, std::string key) : root_(root), key_(std::move(key)) {}

const Widget* MixerElement::widget() const {
    if (!root_ || !root_->window()) return nullptr;
    for (const Widget& w : root_->window()->widgets())
        if (w.key == key_) return &w;
    return nullptr;
}

HRESULT MixerElement::QueryInterface(REFIID riid, void** out) {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
        *out = static_cast<IRawElementProviderSimple*>(this);
    else if (riid == __uuidof(IRawElementProviderFragment))
        *out = static_cast<IRawElementProviderFragment*>(this);
    else if (riid == __uuidof(IInvokeProvider))
        *out = static_cast<IInvokeProvider*>(this);
    else if (riid == __uuidof(IRangeValueProvider))
        *out = static_cast<IRangeValueProvider*>(this);
    else if (riid == __uuidof(IToggleProvider))
        *out = static_cast<IToggleProvider*>(this);
    else if (riid == __uuidof(IValueProvider))
        *out = static_cast<IValueProvider*>(this);
    else {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}
ULONG MixerElement::AddRef() { return ULONG(InterlockedIncrement(&refs_)); }
ULONG MixerElement::Release() {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return ULONG(n);
}

HRESULT MixerElement::get_ProviderOptions(ProviderOptions* r) {
    *r = ProviderOptions(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
    return S_OK;
}

namespace {
enum class Role { Volume, Toggle, Invoke, Text };
Role role_of(const Widget& w) {
    using K = Widget::Kind;
    switch (w.kind) {
        case K::MasterRing: case K::Card: return Role::Volume;
        case K::Mute: case K::Toggle: case K::DayChip: return Role::Toggle;
        case K::IconButton: return w.key == "panic" ? Role::Toggle : Role::Invoke;  // panic is on/off
        case K::Text: case K::Header: return w.key == "e:name" ? Role::Text : Role::Text;
        case K::TimeStepper: return Role::Volume;  // minutes, as a range
        default: return Role::Invoke;
    }
}
}  // namespace

HRESULT MixerElement::GetPatternProvider(PATTERNID id, IUnknown** r) {
    *r = nullptr;
    const Widget* w = widget();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    const Role role = role_of(*w);
    const bool combo = w->kind == Widget::Kind::OutputPill || w->kind == Widget::Kind::Choice;
    if ((id == UIA_RangeValuePatternId && role == Role::Volume) || (id == UIA_TogglePatternId && role == Role::Toggle) ||
        (id == UIA_InvokePatternId && role == Role::Invoke && w->invoke) || (id == UIA_ValuePatternId && combo)) {
        *r = static_cast<IRawElementProviderSimple*>(this);
        AddRef();
    }
    return S_OK;
}

HRESULT MixerElement::GetPropertyValue(PROPERTYID id, VARIANT* r) {
    r->vt = VT_EMPTY;
    const Widget* w = widget();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    using K = Widget::Kind;
    switch (id) {
        case UIA_NamePropertyId: *r = bstr(w->name); break;
        case UIA_AutomationIdPropertyId: *r = bstr(std::wstring(w->key.begin(), w->key.end())); break;
        case UIA_HelpTextPropertyId: {
            // Cards: playing and where it goes (or the site of a tab), read after the name.
            std::wstring help = w->sub;
            if (w->kind == K::Card && w->playing && !w->muted) help = help.empty() ? L"tocando" : L"tocando, " + help;
            if (!help.empty()) *r = bstr(help);
            break;
        }
        case UIA_ControlTypePropertyId: {
            int t = UIA_ButtonControlTypeId;
            if (w->kind == K::MasterRing || w->kind == K::Card || w->kind == K::TimeStepper) t = UIA_SliderControlTypeId;
            else if (w->kind == K::Mute || w->kind == K::Toggle || w->kind == K::DayChip || w->key == "panic")
                t = UIA_CheckBoxControlTypeId;
            else if (w->kind == K::Header) t = UIA_HeaderControlTypeId;
            else if (w->kind == K::Text) t = UIA_TextControlTypeId;
            else if (w->kind == K::OutputPill || w->kind == K::Choice) t = UIA_ComboBoxControlTypeId;
            *r = i4(t);
            break;
        }
        case UIA_IsKeyboardFocusablePropertyId: *r = boolean(w->focusable); break;
        case UIA_HasKeyboardFocusPropertyId: {
            const int f = root_->window()->focus_index();
            *r = boolean(f >= 0 && root_->window()->widgets()[size_t(f)].key == key_);
            break;
        }
        case UIA_IsEnabledPropertyId: *r = boolean(true); break;
        case UIA_IsOffscreenPropertyId: *r = boolean(w->clipped); break;
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId: *r = boolean(true); break;
        case UIA_ItemStatusPropertyId:
            if (w->kind == K::Card && w->muted) *r = bstr(L"mudo");
            else if (w->kind == K::Card && w->playing) *r = bstr(L"tocando");
            else if (w->kind == K::IconButton && w->on) *r = bstr(L"ativo");
            break;
        case UIA_ValueValuePropertyId:
            if (w->kind == K::OutputPill || w->kind == K::Choice) *r = bstr(w->sub);
            break;
        default: break;
    }
    return S_OK;
}

HRESULT MixerElement::get_HostRawElementProvider(IRawElementProviderSimple** r) {
    *r = nullptr;
    return S_OK;
}

HRESULT MixerElement::Navigate(NavigateDirection d, IRawElementProviderFragment** r) {
    *r = nullptr;
    if (!root_ || !root_->window()) return UIA_E_ELEMENTNOTAVAILABLE;
    if (d == NavigateDirection_Parent) {
        *r = root_;
        root_->AddRef();
        return S_OK;
    }
    if (d != NavigateDirection_NextSibling && d != NavigateDirection_PreviousSibling) return S_OK;
    const auto keys = root_->exposed();
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key_) {
            const size_t j = d == NavigateDirection_NextSibling ? i + 1 : i - 1;
            if (j < keys.size()) {
                *r = root_->element(keys[j]);
                (*r)->AddRef();
            }
            break;
        }
    return S_OK;
}

HRESULT MixerElement::GetRuntimeId(SAFEARRAY** r) {
    int ids[2] = {UiaAppendRuntimeId, key_hash(key_)};
    *r = SafeArrayCreateVector(VT_I4, 0, 2);
    if (!*r) return E_OUTOFMEMORY;
    for (LONG i = 0; i < 2; ++i) SafeArrayPutElement(*r, &i, &ids[i]);
    return S_OK;
}

HRESULT MixerElement::get_BoundingRectangle(UiaRect* r) {
    const Widget* w = widget();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    const float s = root_->window()->dpi_scale();
    POINT p{LONG(w->r.left * s), LONG(w->r.top * s)};
    ClientToScreen(root_->window()->hwnd(), &p);
    *r = UiaRect{double(p.x), double(p.y), double((w->r.right - w->r.left) * s), double((w->r.bottom - w->r.top) * s)};
    return S_OK;
}

HRESULT MixerElement::GetEmbeddedFragmentRoots(SAFEARRAY** r) {
    *r = nullptr;
    return S_OK;
}

HRESULT MixerElement::SetFocus() {
    if (!widget()) return UIA_E_ELEMENTNOTAVAILABLE;
    root_->window()->uia_focus(key_);
    return S_OK;
}

HRESULT MixerElement::get_FragmentRoot(IRawElementProviderFragmentRoot** r) {
    if (!root_) return UIA_E_ELEMENTNOTAVAILABLE;
    *r = root_;
    root_->AddRef();
    return S_OK;
}

HRESULT MixerElement::Invoke() {
    if (!widget()) return UIA_E_ELEMENTNOTAVAILABLE;
    root_->window()->uia_invoke(key_);
    return S_OK;
}

HRESULT MixerElement::SetValue(double v) {
    const Widget* w = widget();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    if (v < 0 || v > (w->kind == Widget::Kind::TimeStepper ? 1439 : 100)) return E_INVALIDARG;
    if (w->kind == Widget::Kind::TimeStepper) return E_NOTIMPL;  // minutes: changed with the arrow keys
    root_->window()->uia_set_volume(key_, v);
    return S_OK;
}

HRESULT MixerElement::get_Value(double* r) {
    const Widget* w = widget();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    *r = w->kind == Widget::Kind::TimeStepper ? double(w->minutes) : double((w->volume * 100 + 5000) / 10000);
    return S_OK;
}
HRESULT MixerElement::get_Maximum(double* r) {
    const Widget* w = widget();
    *r = w && w->kind == Widget::Kind::TimeStepper ? 1439 : 100;
    return S_OK;
}
HRESULT MixerElement::get_Minimum(double* r) {
    *r = 0;
    return S_OK;
}
HRESULT MixerElement::get_LargeChange(double* r) {
    *r = 10;
    return S_OK;
}
HRESULT MixerElement::get_SmallChange(double* r) {
    const Widget* w = widget();
    *r = w && w->kind == Widget::Kind::TimeStepper ? 15 : 5;
    return S_OK;
}

// IValueProvider (output and choice lists: the current choice, read-only text).
HRESULT MixerElement::SetValue(LPCWSTR) { return UIA_E_NOTSUPPORTED; }
HRESULT MixerElement::get_Value(BSTR* r) {
    const Widget* w = widget();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    *r = SysAllocString(w->sub.c_str());
    return S_OK;
}
HRESULT MixerElement::get_IsReadOnly(BOOL* r) {
    // Shared by IRangeValueProvider and IValueProvider: lists are chosen through their menu.
    const Widget* w = widget();
    *r = w && (w->kind == Widget::Kind::TimeStepper || w->kind == Widget::Kind::OutputPill || w->kind == Widget::Kind::Choice);
    return S_OK;
}

HRESULT MixerElement::Toggle() {
    if (!widget()) return UIA_E_ELEMENTNOTAVAILABLE;
    root_->window()->uia_toggle(key_);
    return S_OK;
}
HRESULT MixerElement::get_ToggleState(ToggleState* r) {
    const Widget* w = widget();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    *r = w->on ? ToggleState_On : ToggleState_Off;
    return S_OK;
}

}  // namespace agent
