#pragma once
// 1.5: profiles. A profile is a saved mixer state (output volumes and mutes,
// per-app volume and mute) that can be applied by hand from the phone or
// automatically when its conditions hold: days and hours, the PC's network,
// and/or a given output being present (e.g. headphones plugged in). The
// agent's server runs the rules; this file holds the model, the rules and the
// wire/storage encoding, platform-free.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "archon/bytes.hpp"
#include "archon/protocol.hpp"

namespace arc {

constexpr size_t kMaxProfiles = 32;
constexpr size_t kMaxProfileEndpoints = 64;
constexpr size_t kMaxProfileApps = 256;
constexpr size_t kMaxProfileName = 64;
constexpr size_t kMaxNetworkName = 128;

struct ProfileEndpoint {
    std::string id;
    std::string name;  // to find the device again if its id changes
    uint16_t volume = 0;
    bool muted = false;
    bool operator==(const ProfileEndpoint& o) const {
        return id == o.id && name == o.name && volume == o.volume && muted == o.muted;
    }
};

struct ProfileApp {
    std::string key;  // the app's name; "@system" = system sounds
    uint16_t volume = 0;
    bool muted = false;
    bool operator==(const ProfileApp& o) const { return key == o.key && volume == o.volume && muted == o.muted; }
};

struct Profile {
    std::string id;
    std::string name;
    // Conditions (all that are set must hold). `automatic` off = manual only.
    bool automatic = false;
    uint8_t days = 0;        // bit0 Sunday .. bit6 Saturday; 0 = every day
    uint16_t start_min = 0;  // minutes after midnight; start == end = all day
    uint16_t end_min = 0;    // may be < start (a window across midnight)
    std::string network;     // the PC's network name; empty = any
    std::string output_id;   // an output that must be present (headphones); empty = any
    std::string output_name;
    bool restore = false;    // when the conditions stop holding, put back what it changed
    // Settings.
    std::vector<ProfileEndpoint> endpoints;
    std::vector<ProfileApp> apps;
    // Runtime (not stored): applied automatically and still in force.
    bool active = false;

    bool has_time() const { return days != 0 || start_min != end_min; }
    bool operator==(const Profile& o) const {
        return id == o.id && name == o.name && automatic == o.automatic && days == o.days &&
               start_min == o.start_min && end_min == o.end_min && network == o.network &&
               output_id == o.output_id && output_name == o.output_name && restore == o.restore &&
               endpoints == o.endpoints && apps == o.apps && active == o.active;
    }
};

// What the rules look at.
struct ProfileContext {
    int weekday = 0;  // 0 Sunday .. 6 Saturday (local time)
    int minute = 0;   // minutes after local midnight
    std::string network;
    std::set<std::string> outputs;  // ids of the outputs present now
};

// The key a source is remembered by in a profile.
std::string profile_app_key(const SourceInfo& s);

// True when `p` is automatic and every condition it sets holds in `ctx`.
bool profile_matches(const Profile& p, const ProfileContext& ctx);
// More conditions = more specific (wins over a broader profile).
int profile_specificity(const Profile& p);
// Index of the automatic profile that should be in force, or -1.
int pick_profile(const std::vector<Profile>& profiles, const ProfileContext& ctx);

// The current mixer as a profile's settings (outputs and microphones, apps by
// name; browser tabs are not kept: they come and go).
void profile_capture(Profile& p, const std::map<std::string, EndpointInfo>& endpoints,
                     const std::map<std::string, SourceInfo>& sources);

// Wire (PROFILE / SAVE_PROFILE bodies) and storage.
void encode_profile_body(Writer& w, const Profile& p, uint8_t extra_flags = 0);
bool decode_profile_body(Reader& r, Profile& p, uint8_t* extra_flags = nullptr);
Bytes serialize_profiles(const std::vector<Profile>& profiles);
bool parse_profiles(const Bytes& data, std::vector<Profile>& out);

constexpr uint8_t kProfileFlagActive = 1;     // PROFILE: applied automatically and in force
constexpr uint8_t kProfileFlagAutomatic = 2;
constexpr uint8_t kProfileFlagRestore = 4;
constexpr uint8_t kProfileFlagCapture = 8;    // SAVE_PROFILE: take the settings from the mixer now

// Whole messages: PROFILE / SAVE_PROFILE, and CONTEXT (the PC's network now).
Bytes encode_profile(MsgType type, const Profile& p, uint8_t extra_flags = 0);
Bytes encode_context(const std::string& network);
bool decode_context(Reader& r, std::string& network);

}  // namespace arc
