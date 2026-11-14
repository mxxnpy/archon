#pragma once
// The desktop window's model: the mixer as the agent's local view reports it
// (same messages a phone gets), plus the derived lists the window draws.
// Platform-free; the Windows window uses it now, the Linux one later.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "archon/controller_session.hpp"
#include "archon/profiles.hpp"
#include "archon/protocol.hpp"

namespace agent {

class DesktopModel {
public:
    // Applies one plaintext message from Server's LocalView. Returns true when
    // something visible changed (redraw).
    bool apply(const arc::Bytes& msg);
    bool apply(const arc::ControllerEvent& ev);

    bool synced = false;
    uint32_t features = 0;
    bool panic = false;
    std::string network;
    std::map<std::string, arc::EndpointInfo> endpoints;
    std::map<std::string, arc::SourceInfo> sources;
    std::map<std::string, arc::Profile> profiles;
    std::map<uint16_t, uint8_t> levels;  // meter slot -> peak 0..255
    std::vector<std::string> icons_wanted;  // icon keys seen but not fetched yet (the window asks for them)
    std::map<std::string, arc::Bytes> icons;  // icon key -> PNG

    // Outputs, the default first then by name.
    std::vector<const arc::EndpointInfo*> outputs() const;
    // The default microphone (or any microphone).
    const arc::EndpointInfo* microphone() const;
    // The output to show: `preferred` if it still exists, else the default.
    const arc::EndpointInfo* output_or_default(const std::string& preferred) const;
    // Apps on an output (not tabs): playing first, system sounds last, then by name.
    std::vector<const arc::SourceInfo*> apps_on(const std::string& endpoint_id) const;
    // Browser tabs on an output: playing first, then browser and title.
    std::vector<const arc::SourceInfo*> tabs_on(const std::string& endpoint_id) const;
    // Profiles: in force first, then by name.
    std::vector<const arc::Profile*> sorted_profiles() const;
    const arc::Profile* active_profile() const;
    float level(uint16_t slot) const;  // 0..1 perceptual (sqrt) scale, as sent

    bool can_route() const { return features & arc::kFeatureRouting; }
    bool can_profiles() const { return features & arc::kFeatureProfiles; }

private:
    void want_icon(const std::string& key);
};

// Short summary of a profile's conditions in Portuguese or English
// ("seg a sex 09:00–18:00 · rede Casa · com Fone USB" / "Só manual").
std::string profile_summary(const arc::Profile& p, bool portuguese);

}  // namespace agent
