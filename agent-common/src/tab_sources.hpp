#pragma once
// 1.4: browser tabs as sources. Sits between the server and the platform audio
// backend: everything passes through unchanged, and tabs reported by the
// Archon browser extension are added as sources (flag `tab`). Volume and mute
// commands for a tab go back to the extension, which applies them inside the
// page (volume) or with the browser's own tab mute.
//
// Transport-agnostic: the platform layer (a named pipe on Windows) calls
// connect / message / disconnect for each browser connection and provides a
// TabLink to send JSON back. Thread-safe.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "archon/server.hpp"

namespace agent {

class TabLink {
public:
    virtual ~TabLink() = default;
    // Queues one JSON message to the extension. Must not block.
    virtual void send(const std::string& json) = 0;
};

class TabSources final : public arc::AudioBackend, public arc::AudioSink {
public:
    static constexpr size_t kMaxLinks = 8;
    static constexpr size_t kMaxTabsPerLink = 100;

    // `exe_of_pid` gives a process's executable path (to find the browser's own
    // audio session); may return "" when unknown.
    TabSources(arc::AudioBackend& inner, std::function<std::string(uint32_t)> exe_of_pid);

    // ---- browser side ------------------------------------------------------------
    // A browser connected; returns its connection id (0 = refused: too many).
    uint64_t connect(TabLink* link);
    // One JSON message from that browser ({"t":"host"|"hello"|"tabs", ...}).
    void message(uint64_t conn, const std::string& json);
    // The browser went away: its tabs disappear.
    void disconnect(uint64_t conn);
    size_t tab_count() const;
    size_t link_count() const;  // browsers connected now

    // ---- arc::AudioBackend (the server calls these) ---------------------------------
    bool start(arc::AudioSink& sink, std::string* err) override;
    void stop() override;
    void set_endpoint_volume(const std::string& id, uint16_t volume) override { inner_.set_endpoint_volume(id, volume); }
    void set_endpoint_mute(const std::string& id, bool muted) override { inner_.set_endpoint_mute(id, muted); }
    void set_source_volume(const std::string& id, uint16_t volume) override;
    void set_source_mute(const std::string& id, bool muted) override;
    bool icon_png(const std::string& key, arc::Bytes& png) override;
    void set_metering(bool on) override { inner_.set_metering(on); }
    void set_source_route(const std::string& source_id, const std::string& endpoint_id) override;
    uint32_t features() const override { return inner_.features() | (enabled_ ? arc::kFeatureTabs : 0u); }
    // Whether the browser bridge is installed (sets kFeatureTabs). Before start().
    void set_enabled(bool on) { enabled_ = on; }
    std::string os_name() const override { return inner_.os_name(); }

    // ---- arc::AudioSink (the platform backend reports here) ----------------------------
    void endpoint_upsert(const arc::EndpointInfo& e) override;
    void endpoint_removed(const std::string& id) override;
    void source_upsert(const arc::SourceInfo& s) override;
    void source_removed(const std::string& id) override;
    void meters(const std::vector<std::pair<std::string, uint8_t>>& levels) override;

    static bool is_tab_id(const std::string& id) { return id.rfind("tab:", 0) == 0; }

private:
    struct Tab {
        arc::SourceInfo info;
        arc::Bytes icon;  // PNG
    };
    struct Link {
        TabLink* link = nullptr;
        std::string browser;  // "brave", "chrome", ...
        std::string exe;      // browser executable (from the native host)
        size_t messages = 0;  // received so far ("host" is only taken as the first)
        std::map<int64_t, Tab> tabs;
    };
    struct AppSource {  // a platform source, to place tabs on their browser's output
        std::string exe;
        std::string endpoint_id;
        std::string name;
    };

    void refresh_link(uint64_t conn, Link& l);  // re-place every tab of l (browser output/name changed)
    arc::SourceInfo place(uint64_t conn, const Link& l, arc::SourceInfo s) const;
    std::string default_output() const;
    bool find_tab(const std::string& id, uint64_t& conn, int64_t& tab);

    arc::AudioBackend& inner_;
    std::function<std::string(uint32_t)> exe_of_pid_;
    arc::AudioSink* sink_ = nullptr;
    bool enabled_ = true;

    mutable std::mutex mu_;
    uint64_t next_conn_ = 1;
    std::map<uint64_t, Link> links_;
    std::map<std::string, AppSource> apps_;           // platform source id -> where it plays
    std::map<std::string, bool> endpoints_;           // output id -> is default (outputs only)
};

}  // namespace agent
