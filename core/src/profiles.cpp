#include "archon/profiles.hpp"

#include <algorithm>

namespace arc {

std::string profile_app_key(const SourceInfo& s) { return s.system_sounds ? "@system" : s.name; }

bool profile_matches(const Profile& p, const ProfileContext& ctx) {
    if (!p.automatic) return false;
    if (profile_specificity(p) == 0) return false;  // automatic needs at least one condition
    if (p.has_time()) {
        const bool all_day = p.start_min == p.end_min;
        const bool wraps = p.end_min < p.start_min;
        int day = ctx.weekday;
        bool in_window = false;
        if (all_day) {
            in_window = true;
        } else if (!wraps) {
            in_window = ctx.minute >= p.start_min && ctx.minute < p.end_min;
        } else if (ctx.minute >= p.start_min) {
            in_window = true;  // evening part: belongs to today
        } else if (ctx.minute < p.end_min) {
            in_window = true;  // after midnight: belongs to the day the window started
            day = (ctx.weekday + 6) % 7;
        }
        if (!in_window) return false;
        if (p.days != 0 && !(p.days & (1u << day))) return false;
    }
    if (!p.network.empty() && p.network != ctx.network) return false;
    if (!p.output_id.empty() && !ctx.outputs.count(p.output_id)) return false;
    return true;
}

int profile_specificity(const Profile& p) {
    return (p.has_time() ? 1 : 0) + (p.network.empty() ? 0 : 1) + (p.output_id.empty() ? 0 : 1);
}

int pick_profile(const std::vector<Profile>& profiles, const ProfileContext& ctx) {
    int best = -1;
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (!profile_matches(profiles[i], ctx)) continue;
        // The most specific wins; on a tie, the one listed first.
        if (best < 0 || profile_specificity(profiles[i]) > profile_specificity(profiles[size_t(best)])) best = int(i);
    }
    return best;
}

void profile_capture(Profile& p, const std::map<std::string, EndpointInfo>& endpoints,
                     const std::map<std::string, SourceInfo>& sources) {
    p.endpoints.clear();
    p.apps.clear();
    for (const auto& [id, e] : endpoints) {
        if (p.endpoints.size() >= kMaxProfileEndpoints) break;
        p.endpoints.push_back({e.id, e.name, e.volume, e.muted});
    }
    std::set<std::string> seen;
    for (const auto& [id, s] : sources) {
        if (s.tab || p.apps.size() >= kMaxProfileApps) continue;
        const std::string key = profile_app_key(s);
        if (key.empty() || !seen.insert(key).second) continue;  // one entry per app (several sessions)
        p.apps.push_back({key, s.volume, s.muted});
    }
}

void encode_profile_body(Writer& w, const Profile& p, uint8_t extra_flags) {
    const uint8_t flags = (p.active ? kProfileFlagActive : 0) | (p.automatic ? kProfileFlagAutomatic : 0) |
                          (p.restore ? kProfileFlagRestore : 0) | extra_flags;
    w.str(p.id, kMaxId).str(p.name, kMaxProfileName).u8(flags);
    w.u8(p.days).u16(p.start_min).u16(p.end_min);
    w.str(p.network, kMaxNetworkName).str(p.output_id, kMaxId).str(p.output_name, kMaxDisplayName);
    const size_t ne = std::min(p.endpoints.size(), kMaxProfileEndpoints);
    w.u16(uint16_t(ne));
    for (size_t i = 0; i < ne; ++i) {
        const ProfileEndpoint& e = p.endpoints[i];
        w.str(e.id, kMaxId).str(e.name, kMaxDisplayName).u16(std::min<uint16_t>(e.volume, kMaxVolume)).u8(e.muted ? 1 : 0);
    }
    const size_t na = std::min(p.apps.size(), kMaxProfileApps);
    w.u16(uint16_t(na));
    for (size_t i = 0; i < na; ++i) {
        const ProfileApp& a = p.apps[i];
        w.str(a.key, kMaxDisplayName).u16(std::min<uint16_t>(a.volume, kMaxVolume)).u8(a.muted ? 1 : 0);
    }
}

bool decode_profile_body(Reader& r, Profile& p, uint8_t* extra_flags) {
    p = Profile{};
    p.id = r.str(kMaxId);
    p.name = r.str(kMaxProfileName);
    const uint8_t flags = r.u8();
    p.active = flags & kProfileFlagActive;
    p.automatic = flags & kProfileFlagAutomatic;
    p.restore = flags & kProfileFlagRestore;
    if (extra_flags) *extra_flags = flags;
    p.days = r.u8() & 0x7F;
    p.start_min = r.u16();
    p.end_min = r.u16();
    if (p.start_min >= 24 * 60 || p.end_min >= 24 * 60) return false;
    p.network = r.str(kMaxNetworkName);
    p.output_id = r.str(kMaxId);
    p.output_name = r.str(kMaxDisplayName);
    const uint16_t ne = r.u16();
    if (!r.ok() || ne > kMaxProfileEndpoints) return false;
    for (uint16_t i = 0; i < ne && r.ok(); ++i) {
        ProfileEndpoint e;
        e.id = r.str(kMaxId);
        e.name = r.str(kMaxDisplayName);
        e.volume = std::min<uint16_t>(r.u16(), kMaxVolume);
        e.muted = r.u8() != 0;
        p.endpoints.push_back(std::move(e));
    }
    const uint16_t na = r.u16();
    if (!r.ok() || na > kMaxProfileApps) return false;
    for (uint16_t i = 0; i < na && r.ok(); ++i) {
        ProfileApp a;
        a.key = r.str(kMaxDisplayName);
        a.volume = std::min<uint16_t>(r.u16(), kMaxVolume);
        a.muted = r.u8() != 0;
        p.apps.push_back(std::move(a));
    }
    return r.ok();
}

Bytes encode_profile(MsgType type, const Profile& p, uint8_t extra_flags) {
    Writer w;
    w.u8(uint8_t(type));
    encode_profile_body(w, p, extra_flags);
    return w.take();
}

Bytes encode_context(const std::string& network) {
    Writer w;
    w.u8(uint8_t(MsgType::Context)).str(network, kMaxNetworkName);
    return w.take();
}

bool decode_context(Reader& r, std::string& network) {
    network = r.str(kMaxNetworkName);
    return r.ok();  // tolerant: later versions may append fields
}

namespace {
constexpr uint8_t kMagic[4] = {'A', 'R', 'P', '1'};
}

Bytes serialize_profiles(const std::vector<Profile>& profiles) {
    Writer w;
    w.raw(kMagic, 4);
    const size_t n = std::min(profiles.size(), kMaxProfiles);
    w.u16(uint16_t(n));
    for (size_t i = 0; i < n; ++i) {
        Profile stored = profiles[i];
        stored.active = false;  // runtime state is not kept
        encode_profile_body(w, stored);
    }
    return w.take();
}

bool parse_profiles(const Bytes& data, std::vector<Profile>& out) {
    out.clear();
    if (data.size() < 6 || !std::equal(kMagic, kMagic + 4, data.begin())) return false;
    Reader r(data.data() + 4, data.size() - 4);
    const uint16_t n = r.u16();
    if (!r.ok() || n > kMaxProfiles) return false;
    for (uint16_t i = 0; i < n; ++i) {
        Profile p;
        if (!decode_profile_body(r, p)) {
            out.clear();
            return false;
        }
        p.active = false;
        out.push_back(std::move(p));
    }
    return r.at_end();
}

}  // namespace arc
