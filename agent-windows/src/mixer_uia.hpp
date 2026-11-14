#pragma once
// UI Automation for the Archon window: every reachable widget (buttons, volume
// rings and cards, mute buttons, toggles, chips, rows) is an element with name,
// role, value and state, so Narrator / NVDA read and drive the mixer.

#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>

#include <map>
#include <string>
#include <vector>

namespace agent {

class MixerWindow;
class MixerElement;
struct Widget;

class MixerUia final : public IRawElementProviderSimple,
                       public IRawElementProviderFragment,
                       public IRawElementProviderFragmentRoot {
public:
    explicit MixerUia(MixerWindow* w);
    LRESULT return_root(HWND h, WPARAM wp, LPARAM lp);
    void disconnect();  // the window is going away
    void focus_changed(const std::string& key);
    void value_changed(const std::string& key, int volume);
    void structure_maybe_changed();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* r) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** r) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* r) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** r) override;
    // IRawElementProviderFragment
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection d, IRawElementProviderFragment** r) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** r) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* r) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** r) override;
    HRESULT STDMETHODCALLTYPE SetFocus() override;
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** r) override;
    // IRawElementProviderFragmentRoot
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** r) override;
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** r) override;

    // For elements.
    MixerWindow* window() const { return w_; }
    MixerElement* element(const std::string& key);
    std::vector<std::string> exposed() const;  // keys in reading order

private:
    ~MixerUia();
    LONG refs_ = 1;
    MixerWindow* w_;
    std::map<std::string, MixerElement*> elements_;
    std::vector<std::string> last_keys_;
};

class MixerElement final : public IRawElementProviderSimple,
                           public IRawElementProviderFragment,
                           public IInvokeProvider,
                           public IRangeValueProvider,
                           public IToggleProvider {
public:
    MixerElement(MixerUia* root, std::string key);
    void detach() { root_ = nullptr; }
    const std::string& key() const { return key_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* r) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** r) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* r) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** r) override;
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection d, IRawElementProviderFragment** r) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** r) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* r) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** r) override;
    HRESULT STDMETHODCALLTYPE SetFocus() override;
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** r) override;
    // IInvokeProvider
    HRESULT STDMETHODCALLTYPE Invoke() override;
    // IRangeValueProvider (volume in %)
    HRESULT STDMETHODCALLTYPE SetValue(double v) override;
    HRESULT STDMETHODCALLTYPE get_Value(double* r) override;
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* r) override;
    HRESULT STDMETHODCALLTYPE get_Maximum(double* r) override;
    HRESULT STDMETHODCALLTYPE get_Minimum(double* r) override;
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* r) override;
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* r) override;
    // IToggleProvider (mute, switches, day chips)
    HRESULT STDMETHODCALLTYPE Toggle() override;
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* r) override;

private:
    ~MixerElement() = default;
    const Widget* widget() const;
    LONG refs_ = 1;
    MixerUia* root_;
    std::string key_;
};

}  // namespace agent
