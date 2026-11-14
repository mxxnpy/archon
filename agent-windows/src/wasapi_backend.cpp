#include "wasapi_backend.hpp"

#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <initguid.h>  // define PKEY_Device_FriendlyName in this TU
#include <functiondiscoverykeys_devpkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <future>
#include <set>

#include "app_routing.hpp"
#include "archon/crypto.hpp"
#include "win_util.hpp"

using Microsoft::WRL::ComPtr;

namespace agent {

namespace {

// GUID passed as event context for changes we make (diagnostics only).
const GUID kEventContext = {0x6f1c2a1e, 0x3b7d, 0x4c55, {0x9a, 0x11, 0x2e, 0x5f, 0x77, 0x01, 0xfa, 0xde}};

uint16_t to_level(float scalar) {
    const float v = std::clamp(scalar, 0.0f, 1.0f);
    return uint16_t(std::lround(v * 10000.0f));
}

float to_scalar(uint16_t level) { return float(std::min<uint16_t>(level, 10000)) / 10000.0f; }

// Minimal IUnknown implementation for notification sinks.
template <class I>
class Sink : public I {
public:
    explicit Sink(WasapiBackend* b) : backend_(b) {}
    virtual ~Sink() = default;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(I)) {
            *out = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

protected:
    WasapiBackend* backend_;

private:
    std::atomic<ULONG> ref_{1};
};

class DeviceNotifier final : public Sink<IMMNotificationClient> {
public:
    using Sink::Sink;
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return dirty(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return dirty(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return dirty(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override {
        return (flow == eRender || flow == eCapture) ? dirty() : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
        // Fires often for unrelated properties; only names matter to us.
        if (key.fmtid == PKEY_Device_FriendlyName.fmtid && key.pid == PKEY_Device_FriendlyName.pid) return dirty();
        return S_OK;
    }

private:
    HRESULT dirty() {
        backend_->mark_devices_dirty();
        return S_OK;
    }
};

class EndpointCallback final : public Sink<IAudioEndpointVolumeCallback> {
public:
    using Sink::Sink;
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        backend_->mark_sessions_dirty();
        return S_OK;
    }
};

class SessionNotifier final : public Sink<IAudioSessionNotification> {
public:
    using Sink::Sink;
    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl*) override {
        backend_->mark_sessions_dirty();
        return S_OK;
    }
};

class SessionEvents final : public Sink<IAudioSessionEvents> {
public:
    using Sink::Sink;
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return dirty(); }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return dirty(); }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float*, DWORD, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override { return dirty(); }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override { return dirty(); }

private:
    HRESULT dirty() {
        backend_->mark_sessions_dirty();
        return S_OK;
    }
};

struct SessionEntry {
    ComPtr<IAudioSessionControl2> ctl;
    ComPtr<ISimpleAudioVolume> vol;
    SessionEvents* events = nullptr;  // owned reference
    ComPtr<IAudioMeterInformation> meter;  // lazily acquired while metering
    std::string source_id;
    bool system = false;
    DWORD pid = 0;
};

struct EndpointEntry {
    std::string id;
    std::string name;
    bool input = false;  // capture device (microphone): no sessions
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioEndpointVolume> vol;
    EndpointCallback* vol_cb = nullptr;
    ComPtr<IAudioSessionManager2> mgr;
    SessionNotifier* notifier = nullptr;
    ComPtr<IAudioMeterInformation> meter;
    std::map<std::wstring, SessionEntry> sessions;
};

struct ProcInfo {
    std::string name;
    std::wstring path;
    std::string icon_key;
    std::string route;  // output the app is pinned to ("" = default)
    bool route_read = false;
};

void release_session(SessionEntry& s) {
    if (s.events) {
        s.ctl->UnregisterAudioSessionNotification(s.events);
        s.events->Release();
        s.events = nullptr;
    }
}

void release_endpoint(EndpointEntry& e) {
    for (auto& [k, s] : e.sessions) release_session(s);
    e.sessions.clear();
    if (e.notifier && e.mgr) {
        e.mgr->UnregisterSessionNotification(e.notifier);
        e.notifier->Release();
        e.notifier = nullptr;
    }
    if (e.vol_cb) {
        e.vol->UnregisterControlChangeNotify(e.vol_cb);
        e.vol_cb->Release();
        e.vol_cb = nullptr;
    }
}

std::string device_name(IMMDevice* dev) {
    ComPtr<IPropertyStore> props;
    std::string name;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = to_utf8(v.pwszVal);
        PropVariantClear(&v);
    }
    return name.empty() ? "Audio device" : name;
}

std::string file_description(const std::wstring& path) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return {};
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    struct Lang {
        WORD lang, cp;
    }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) ||
        len < sizeof(Lang))
        return {};
    wchar_t sub[64];
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].cp);
    wchar_t* desc = nullptr;
    if (!VerQueryValueW(data.data(), sub, reinterpret_cast<void**>(&desc), &len) || !desc || len <= 1) return {};
    std::wstring d(desc);
    while (!d.empty() && iswspace(d.back())) d.pop_back();
    return to_utf8(d);
}

ProcInfo process_info(DWORD pid) {
    ProcInfo info;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        std::wstring buf(1024, L'\0');
        DWORD n = DWORD(buf.size());
        if (QueryFullProcessImageNameW(h, 0, buf.data(), &n)) info.path.assign(buf.data(), n);
        CloseHandle(h);
    }
    if (!info.path.empty()) {
        info.name = file_description(info.path);
        if (info.name.empty()) {
            std::wstring file = PathFindFileNameW(info.path.c_str());
            PathRemoveExtensionW(file.data());
            info.name = to_utf8(std::wstring(file.c_str()));
        }
        std::wstring lower = info.path;
        CharLowerBuffW(lower.data(), DWORD(lower.size()));
        const std::string u = to_utf8(lower);
        const arc::Key32 h32 = arc::kdf(arc::ByteView(nullptr, 0), {"ARC-icon", std::string_view(u)});
        info.icon_key = arc::to_hex(h32.data(), 8);
    }
    if (info.name.empty()) info.name = "PID " + std::to_string(pid);
    return info;
}

std::string session_display_name(IAudioSessionControl2* ctl) {
    LPWSTR raw = nullptr;
    std::string out;
    if (SUCCEEDED(ctl->GetDisplayName(&raw)) && raw) {
        std::wstring n = raw;
        CoTaskMemFree(raw);
        if (!n.empty() && n[0] == L'@') {
            wchar_t buf[512];
            if (SUCCEEDED(SHLoadIndirectString(n.c_str(), buf, 512, nullptr))) n = buf;
            else n.clear();
        }
        out = to_utf8(n);
    }
    return out;
}

}  // namespace

struct WasapiBackend::State {
    ComPtr<IMMDeviceEnumerator> enumerator;
    DeviceNotifier* device_notifier = nullptr;
    std::map<std::string, EndpointEntry> endpoints;
    std::string default_id;
    std::string default_input_id;
    std::map<DWORD, ProcInfo> procs;
    std::unique_ptr<AppRouting> routing;
};

WasapiBackend::WasapiBackend() { wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

WasapiBackend::~WasapiBackend() {
    stop();
    if (wake_) CloseHandle(wake_);
}

void WasapiBackend::mark_devices_dirty() {
    devices_dirty_ = true;
    SetEvent(wake_);
}

void WasapiBackend::mark_sessions_dirty() {
    sessions_dirty_ = true;
    SetEvent(wake_);
}

void WasapiBackend::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        tasks_.push_back(std::move(fn));
    }
    SetEvent(wake_);
}

bool WasapiBackend::start(arc::AudioSink& sink, std::string* err) {
    if (running_) return true;
    sink_ = &sink;
    running_ = true;
    devices_dirty_ = sessions_dirty_ = true;
    std::promise<std::string> started;
    auto fut = started.get_future();
    thread_ = std::thread([this, &started] { run(&started); });
    std::string e = fut.get();
    if (!e.empty()) {
        if (err) *err = e;
        running_ = false;
        thread_.join();
        return false;
    }
    return true;
}

void WasapiBackend::stop() {
    if (!running_.exchange(false)) return;
    SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    sink_ = nullptr;
}

void WasapiBackend::run(std::promise<std::string>* started) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    st_ = std::make_unique<State>();
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&st_->enumerator));
    if (FAILED(hr)) {
        started->set_value("cannot open Windows audio (MMDeviceEnumerator)");
        st_.reset();
        CoUninitialize();
        return;
    }
    st_->routing = std::make_unique<AppRouting>();
    routing_ok_ = st_->routing->available();
    st_->device_notifier = new DeviceNotifier(this);
    st_->enumerator->RegisterEndpointNotificationCallback(st_->device_notifier);
    // Report the full current state before declaring success.
    refresh_devices();
    refresh_sessions();
    publish();
    started->set_value({});

    while (running_) {
        // Periodic safety refresh in case a notification was missed.
        // 20 Hz while a controller shows level meters; otherwise sleep until notified.
        DWORD w = WaitForSingleObject(wake_, metering_ ? 50 : 5000);
        if (!running_) break;
        if (w == WAIT_OBJECT_0 && (devices_dirty_ || sessions_dirty_)) Sleep(15);  // coalesce bursts
        if (w == WAIT_TIMEOUT && !metering_) sessions_dirty_ = true;
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lk(mu_);
            tasks.swap(tasks_);
        }
        for (auto& t : tasks) t();
        bool changed = false;
        if (devices_dirty_.exchange(false)) {
            refresh_devices();
            sessions_dirty_ = true;
        }
        if (sessions_dirty_.exchange(false)) {
            refresh_sessions();
            changed = true;
        }
        if (changed) publish();
        if (metering_) sample_meters();
    }

    // Drain pending tasks (e.g. icon requests waiting on promises).
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& t : tasks_) t();
        tasks_.clear();
    }
    for (auto& [id, e] : st_->endpoints) release_endpoint(e);
    st_->endpoints.clear();
    st_->enumerator->UnregisterEndpointNotificationCallback(st_->device_notifier);
    st_->device_notifier->Release();
    st_->routing.reset();  // COM object: release on this (COM) thread
    st_.reset();
    CoUninitialize();
}

void WasapiBackend::refresh_devices() {
    State& s = *st_;
    std::set<std::string> present;
    for (EDataFlow flow : {eRender, eCapture}) {
        ComPtr<IMMDeviceCollection> coll;
        if (FAILED(s.enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) continue;
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            LPWSTR wid = nullptr;
            if (FAILED(coll->Item(i, &dev)) || FAILED(dev->GetId(&wid))) continue;
            const std::string id = to_utf8(wid);
            CoTaskMemFree(wid);
            present.insert(id);
            auto it = s.endpoints.find(id);
            if (it != s.endpoints.end()) {
                it->second.name = device_name(dev.Get());
                continue;
            }
            EndpointEntry e;
            e.id = id;
            e.dev = dev;
            e.name = device_name(dev.Get());
            e.input = flow == eCapture;
            if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &e.vol))) continue;
            // Apps are listed per output; microphones only need volume, mute and a meter.
            if (!e.input && FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &e.mgr)))
                continue;
            e.vol_cb = new EndpointCallback(this);
            e.vol->RegisterControlChangeNotify(e.vol_cb);
            if (e.mgr) {
                e.notifier = new SessionNotifier(this);
                e.mgr->RegisterSessionNotification(e.notifier);
            }
            s.endpoints.emplace(id, std::move(e));
        }
    }
    for (auto it = s.endpoints.begin(); it != s.endpoints.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        release_endpoint(it->second);
        it = s.endpoints.erase(it);
    }
    auto default_of = [&](EDataFlow flow) {
        std::string out;
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(s.enumerator->GetDefaultAudioEndpoint(flow, eConsole, &def))) {
            LPWSTR wid = nullptr;
            if (SUCCEEDED(def->GetId(&wid))) {
                out = to_utf8(wid);
                CoTaskMemFree(wid);
            }
        }
        return out;
    };
    s.default_id = default_of(eRender);
    s.default_input_id = default_of(eCapture);
}

void WasapiBackend::refresh_sessions() {
    State& s = *st_;
    std::set<DWORD> live_pids;
    for (auto& [eid, e] : s.endpoints) {
        if (!e.mgr) continue;  // microphones: no app sessions
        ComPtr<IAudioSessionEnumerator> en;
        std::set<std::wstring> seen;
        // Enumerating also arms OnSessionCreated notifications (documented quirk).
        if (SUCCEEDED(e.mgr->GetSessionEnumerator(&en))) {
            int count = 0;
            en->GetCount(&count);
            for (int i = 0; i < count; ++i) {
                ComPtr<IAudioSessionControl> c1;
                ComPtr<IAudioSessionControl2> ctl;
                if (FAILED(en->GetSession(i, &c1)) || FAILED(c1.As(&ctl))) continue;
                AudioSessionState state = AudioSessionStateInactive;
                ctl->GetState(&state);
                if (state == AudioSessionStateExpired) continue;
                LPWSTR inst = nullptr;
                if (FAILED(ctl->GetSessionInstanceIdentifier(&inst)) || !inst) continue;
                std::wstring key = inst;
                CoTaskMemFree(inst);
                seen.insert(key);
                if (e.sessions.count(key)) continue;
                SessionEntry se;
                se.ctl = ctl;
                if (FAILED(ctl.As(&se.vol))) continue;
                se.system = ctl->IsSystemSoundsSession() == S_OK;
                ctl->GetProcessId(&se.pid);
                se.source_id = eid + "|" + (se.system ? std::string("sys") : std::to_string(se.pid));
                se.events = new SessionEvents(this);
                ctl->RegisterAudioSessionNotification(se.events);
                e.sessions.emplace(key, std::move(se));
            }
        }
        for (auto it = e.sessions.begin(); it != e.sessions.end();) {
            if (seen.count(it->first)) {
                if (!it->second.system) live_pids.insert(it->second.pid);
                ++it;
                continue;
            }
            release_session(it->second);
            it = e.sessions.erase(it);
        }
    }
    // Forget processes that no longer own a session (PIDs get reused).
    for (auto it = s.procs.begin(); it != s.procs.end();)
        it = live_pids.count(it->first) ? std::next(it) : s.procs.erase(it);
    // Re-read each app's pinned output on refresh: it can also change in
    // Windows Settings (App volume and device preferences).
    for (auto& [pid, p] : s.procs) p.route_read = false;
}

void WasapiBackend::publish() {
    State& s = *st_;
    std::map<std::string, arc::EndpointInfo> eps;
    std::map<std::string, arc::SourceInfo> srcs;
    std::map<std::string, int> group_count;

    for (auto& [eid, e] : s.endpoints) {
        arc::EndpointInfo info;
        info.id = eid;
        info.name = e.name;
        info.input = e.input;
        info.is_default = !e.input && eid == s.default_id;
        info.is_default_input = e.input && eid == s.default_input_id;
        float scalar = 0;
        BOOL mute = FALSE;
        e.vol->GetMasterVolumeLevelScalar(&scalar);
        e.vol->GetMute(&mute);
        info.volume = to_level(scalar);
        info.muted = mute != FALSE;
        eps[eid] = info;

        for (auto& [key, se] : e.sessions) {
            float v = 0;
            BOOL m = FALSE;
            se.vol->GetMasterVolume(&v);
            se.vol->GetMute(&m);
            AudioSessionState state = AudioSessionStateInactive;
            se.ctl->GetState(&state);
            auto it = srcs.find(se.source_id);
            if (it == srcs.end()) {
                arc::SourceInfo src;
                src.id = se.source_id;
                src.endpoint_id = eid;
                src.pid = se.system ? 0 : se.pid;
                src.system_sounds = se.system;
                src.volume = to_level(v);
                src.muted = m != FALSE;
                src.playing = state == AudioSessionStateActive;
                if (se.system) {
                    src.name = is_portuguese_ui() ? "Sons do sistema" : "System sounds";
                } else {
                    auto pit = s.procs.find(se.pid);
                    if (pit == s.procs.end()) pit = s.procs.emplace(se.pid, process_info(se.pid)).first;
                    const std::string display = session_display_name(se.ctl.Get());
                    src.name = display.empty() ? pit->second.name : display;
                    src.icon_key = pit->second.icon_key;
                    if (!pit->second.route_read && s.routing) {
                        pit->second.route = to_utf8(s.routing->get(se.pid));
                        pit->second.route_read = true;
                    }
                    src.route = pit->second.route;
                    if (!src.icon_key.empty()) {
                        std::lock_guard<std::mutex> lk(icon_mu_);
                        icon_paths_[src.icon_key] = pit->second.path;
                    }
                }
                srcs.emplace(src.id, src);
                group_count[se.source_id] = 1;
            } else {
                // Grouped sessions: muted only if all are; playing if any is.
                it->second.muted = it->second.muted && m != FALSE;
                it->second.playing = it->second.playing || state == AudioSessionStateActive;
                ++group_count[se.source_id];
            }
        }
    }

    if (!sink_) return;
    for (auto& [id, e] : eps) {
        auto it = sent_endpoints_.find(id);
        if (it == sent_endpoints_.end() || it->second != e) sink_->endpoint_upsert(e);
    }
    for (auto& [id, src] : srcs) {
        auto it = sent_sources_.find(id);
        if (it == sent_sources_.end() || it->second != src) sink_->source_upsert(src);
    }
    for (auto& [id, src] : sent_sources_)
        if (!srcs.count(id)) sink_->source_removed(id);
    for (auto& [id, e] : sent_endpoints_)
        if (!eps.count(id)) sink_->endpoint_removed(id);
    sent_endpoints_.swap(eps);
    sent_sources_.swap(srcs);
}

void WasapiBackend::set_endpoint_volume(const std::string& id, uint16_t volume) {
    post([this, id, volume] {
        auto it = st_->endpoints.find(id);
        if (it != st_->endpoints.end()) it->second.vol->SetMasterVolumeLevelScalar(to_scalar(volume), &kEventContext);
    });
}

void WasapiBackend::set_endpoint_mute(const std::string& id, bool muted) {
    post([this, id, muted] {
        auto it = st_->endpoints.find(id);
        if (it != st_->endpoints.end()) it->second.vol->SetMute(muted, &kEventContext);
    });
}

void WasapiBackend::set_source_volume(const std::string& id, uint16_t volume) {
    post([this, id, volume] {
        for (auto& [eid, e] : st_->endpoints)
            for (auto& [k, se] : e.sessions)
                if (se.source_id == id) se.vol->SetMasterVolume(to_scalar(volume), &kEventContext);
        sessions_dirty_ = true;
    });
}

void WasapiBackend::set_source_mute(const std::string& id, bool muted) {
    post([this, id, muted] {
        for (auto& [eid, e] : st_->endpoints)
            for (auto& [k, se] : e.sessions)
                if (se.source_id == id) se.vol->SetMute(muted, &kEventContext);
        sessions_dirty_ = true;
    });
}

bool WasapiBackend::icon_png(const std::string& key, arc::Bytes& png) {
    std::wstring path;
    {
        std::lock_guard<std::mutex> lk(icon_mu_);
        auto c = icon_cache_.find(key);
        if (c != icon_cache_.end()) {
            png = c->second;
            return !png.empty();
        }
        auto p = icon_paths_.find(key);
        if (p == icon_paths_.end()) return false;
        path = p->second;
    }
    if (!running_) return false;
    // WIC needs COM: run the extraction on the worker thread and wait briefly.
    auto result = std::make_shared<std::promise<arc::Bytes>>();
    auto fut = result->get_future();
    post([path, result] {
        arc::Bytes out;
        HICON icon = nullptr;
        if (SHDefExtractIconW(path.c_str(), 0, 0, &icon, nullptr, 64) == S_OK && icon) {
            icon_to_png(icon, out);
            DestroyIcon(icon);
        }
        result->set_value(std::move(out));
    });
    if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) return false;
    png = fut.get();
    if (png.size() > arc::kMaxIconBytes) png.clear();
    std::lock_guard<std::mutex> lk(icon_mu_);
    icon_cache_[key] = png;
    return !png.empty();
}

void WasapiBackend::set_source_route(const std::string& id, const std::string& endpoint_id) {
    post([this, id, endpoint_id] {
        if (!st_->routing || !st_->routing->available()) return;
        DWORD pid = 0;
        for (auto& [eid, e] : st_->endpoints)
            for (auto& [k, se] : e.sessions)
                if (se.source_id == id && !se.system) pid = se.pid;
        if (!pid) return;  // system sounds follow the default output
        if (!st_->routing->set(pid, to_wide(endpoint_id))) {
            log_line("could not route " + id + " (error " + std::to_string(st_->routing->last_error()) + ")");
            return;
        }
        auto pit = st_->procs.find(pid);
        if (pit != st_->procs.end()) {
            pit->second.route = endpoint_id;
            pit->second.route_read = true;
        }
        // The session moves to the new device shortly after; refresh now and
        // again when Windows reports the new session.
        sessions_dirty_ = true;
    });
}

uint32_t WasapiBackend::features() const {
    return arc::kFeatureInputs | (routing_ok_ ? arc::kFeatureRouting : 0u);
}

void WasapiBackend::set_metering(bool on) {
    metering_ = on;
    SetEvent(wake_);
}

void WasapiBackend::sample_meters() {
    auto level = [](IAudioMeterInformation* m) -> uint8_t {
        float peak = 0;
        if (!m || FAILED(m->GetPeakValue(&peak))) return 0;
        // Perceptual curve: sqrt lifts quiet signals so the meter is readable.
        return uint8_t(std::lround(std::sqrt(std::clamp(peak, 0.0f, 1.0f)) * 255.0f));
    };
    std::vector<std::pair<std::string, uint8_t>> out;
    std::map<std::string, uint8_t> by_source;
    for (auto& [eid, e] : st_->endpoints) {
        if (!e.meter) e.dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, &e.meter);
        out.emplace_back(eid, level(e.meter.Get()));
        for (auto& [k, se] : e.sessions) {
            if (!se.meter) se.ctl.As(&se.meter);
            uint8_t& v = by_source[se.source_id];
            v = std::max(v, level(se.meter.Get()));
        }
    }
    bool silent = true;
    for (auto& [id, v] : by_source) {
        out.emplace_back(id, v);
        if (v) silent = false;
    }
    for (auto& [id, v] : out)
        if (v) silent = false;
    // While everything is silent send one all-zero frame, then nothing.
    if (silent && meters_were_silent_) return;
    meters_were_silent_ = silent;
    if (sink_) sink_->meters(out);
}

}  // namespace agent
