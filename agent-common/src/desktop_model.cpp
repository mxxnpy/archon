#include "desktop_model.hpp"

#include <algorithm>
#include <cstdio>

namespace agent {

namespace {
std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
}  // namespace

bool DesktopModel::apply(const arc::Bytes& msg) {
    std::vector<arc::ControllerEvent> evs;
    if (!arc::ControllerSession::decode_app(msg, evs)) return false;
    bool changed = false;
    for (const arc::ControllerEvent& ev : evs) changed = apply(ev) || changed;
    return changed;
}

void DesktopModel::want_icon(const std::string& key) {
    if (key.empty() || icons.count(key)) return;
    if (std::find(icons_wanted.begin(), icons_wanted.end(), key) == icons_wanted.end()) icons_wanted.push_back(key);
}

bool DesktopModel::apply(const arc::ControllerEvent& ev) {
    using K = arc::ControllerEvent::Kind;
    switch (ev.kind) {
        case K::StateReset:
            synced = false;
            endpoints.clear();
            sources.clear();
            profiles.clear();
            levels.clear();
            return true;
        case K::StateSynced:
            synced = true;
            return true;
        case K::Endpoint:
            endpoints[ev.endpoint.id] = ev.endpoint;
            return true;
        case K::EndpointRemoved:
            return endpoints.erase(ev.id) > 0;
        case K::Source:
            sources[ev.source.id] = ev.source;
            want_icon(ev.source.icon_key);
            return true;
        case K::SourceRemoved:
            return sources.erase(ev.id) > 0;
        case K::Icon:
            icons[ev.id] = ev.data;  // empty = no icon (stop asking)
            icons_wanted.erase(std::remove(icons_wanted.begin(), icons_wanted.end(), ev.id), icons_wanted.end());
            return true;
        case K::Meters: {
            bool any = false;
            for (const arc::MeterLevel& m : ev.meters) {
                auto& v = levels[m.slot];
                any = any || v != m.level;
                v = m.level;
            }
            return any;
        }
        case K::AgentInfo:
            features = ev.agent.features;
            panic = ev.agent.panic;
            return true;
        case K::Profile:
            profiles[ev.profile.id] = ev.profile;
            return true;
        case K::ProfileRemoved:
            return profiles.erase(ev.id) > 0;
        case K::Context:
            network = ev.message;
            return true;
        default:
            return false;
    }
}

std::vector<const arc::EndpointInfo*> DesktopModel::outputs() const {
    std::vector<const arc::EndpointInfo*> v;
    for (auto& [id, e] : endpoints)
        if (!e.input) v.push_back(&e);
    std::sort(v.begin(), v.end(), [](auto* a, auto* b) {
        if (a->is_default != b->is_default) return a->is_default;
        return lower(a->name) < lower(b->name);
    });
    return v;
}

const arc::EndpointInfo* DesktopModel::microphone() const {
    const arc::EndpointInfo* any = nullptr;
    for (auto& [id, e] : endpoints) {
        if (!e.input) continue;
        if (e.is_default_input) return &e;
        if (!any || lower(e.name) < lower(any->name)) any = &e;
    }
    return any;
}

const arc::EndpointInfo* DesktopModel::output_or_default(const std::string& preferred) const {
    auto it = endpoints.find(preferred);
    if (it != endpoints.end() && !it->second.input) return &it->second;
    const auto outs = outputs();
    return outs.empty() ? nullptr : outs.front();
}

std::vector<const arc::SourceInfo*> DesktopModel::apps_on(const std::string& endpoint_id) const {
    std::vector<const arc::SourceInfo*> v;
    for (auto& [id, s] : sources)
        if (!s.tab && s.endpoint_id == endpoint_id) v.push_back(&s);
    std::sort(v.begin(), v.end(), [](auto* a, auto* b) {
        if (a->system_sounds != b->system_sounds) return !a->system_sounds;
        if (a->playing != b->playing) return a->playing;
        return lower(a->name) < lower(b->name);
    });
    return v;
}

std::vector<const arc::SourceInfo*> DesktopModel::tabs_on(const std::string& endpoint_id) const {
    std::vector<const arc::SourceInfo*> v;
    for (auto& [id, s] : sources)
        if (s.tab && s.endpoint_id == endpoint_id) v.push_back(&s);
    std::sort(v.begin(), v.end(), [](auto* a, auto* b) {
        if (a->playing != b->playing) return a->playing;
        if (a->group != b->group) return a->group < b->group;
        return lower(a->name) < lower(b->name);
    });
    return v;
}

std::vector<const arc::Profile*> DesktopModel::sorted_profiles() const {
    std::vector<const arc::Profile*> v;
    for (auto& [id, p] : profiles) v.push_back(&p);
    std::sort(v.begin(), v.end(), [](auto* a, auto* b) {
        if (a->active != b->active) return a->active;
        return lower(a->name) < lower(b->name);
    });
    return v;
}

const arc::Profile* DesktopModel::active_profile() const {
    for (auto& [id, p] : profiles)
        if (p.active) return &p;
    return nullptr;
}

float DesktopModel::level(uint16_t slot) const {
    auto it = levels.find(slot);
    return it == levels.end() ? 0.f : float(it->second) / 255.f;
}

std::string profile_summary(const arc::Profile& p, bool pt) {
    if (!p.automatic) return pt ? "Só manual" : "Manual only";
    auto clock = [](int m) {
        char b[8];
        std::snprintf(b, sizeof b, "%02d:%02d", m / 60, m % 60);
        return std::string(b);
    };
    std::vector<std::string> parts;
    if (p.has_time()) {
        static const char* pt_days[] = {"dom", "seg", "ter", "qua", "qui", "sex", "sáb"};
        static const char* en_days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        std::string d;
        if (p.days == 0) d = pt ? "todo dia" : "every day";
        else if (p.days == 0b0111110) d = pt ? "seg a sex" : "Mon to Fri";
        else if (p.days == 0b1000001) d = pt ? "fim de semana" : "weekends";
        else
            for (int i = 0; i < 7; ++i)
                if (p.days & (1 << i)) d += (d.empty() ? "" : ", ") + std::string(pt ? pt_days[i] : en_days[i]);
        if (p.start_min != p.end_min) d += " " + clock(p.start_min) + "\xE2\x80\x93" + clock(p.end_min);
        parts.push_back(d);
    }
    if (!p.network.empty()) parts.push_back((pt ? "rede " : "network ") + p.network);
    if (!p.output_id.empty()) parts.push_back((pt ? "com " : "with ") + p.output_name);
    std::string out;
    for (const std::string& s : parts) out += (out.empty() ? "" : " \xC2\xB7 ") + s;
    return out;
}

}  // namespace agent
