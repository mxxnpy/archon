// 1.4: browser tabs as sources — JSON reader, the TabSources bridge between
// the extension and the server, and the SOURCE wire fields.

#include <map>
#include <string>
#include <vector>

#include "../../core/tests/test.hpp"
#include "archon/bytes.hpp"
#include "archon/protocol.hpp"
#include "mini_json.hpp"
#include "tab_sources.hpp"

using namespace agent;

namespace {

// 1x1 transparent PNG.
const char kPngB64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

class Inner : public arc::AudioBackend {
public:
    arc::AudioSink* sink = nullptr;
    std::vector<std::pair<std::string, uint16_t>> volumes;
    std::vector<std::pair<std::string, bool>> mutes;
    std::vector<std::pair<std::string, std::string>> routes;
    bool start(arc::AudioSink& s, std::string*) override {
        sink = &s;
        arc::EndpointInfo e;
        e.id = "OUT1";
        e.name = "Speakers";
        e.is_default = true;
        s.endpoint_upsert(e);
        e.id = "OUT2";
        e.name = "Headset";
        e.is_default = false;
        s.endpoint_upsert(e);
        return true;
    }
    void stop() override {}
    void set_endpoint_volume(const std::string&, uint16_t) override {}
    void set_endpoint_mute(const std::string&, bool) override {}
    void set_source_volume(const std::string& id, uint16_t v) override { volumes.emplace_back(id, v); }
    void set_source_mute(const std::string& id, bool m) override { mutes.emplace_back(id, m); }
    bool icon_png(const std::string&, arc::Bytes&) override { return false; }
    void set_source_route(const std::string& id, const std::string& ep) override { routes.emplace_back(id, ep); }
    uint32_t features() const override { return arc::kFeatureInputs; }
};

class Sink : public arc::AudioSink {
public:
    std::map<std::string, arc::SourceInfo> sources;
    std::vector<std::string> removed;
    int upserts = 0;
    void endpoint_upsert(const arc::EndpointInfo&) override {}
    void endpoint_removed(const std::string&) override {}
    void source_upsert(const arc::SourceInfo& s) override {
        sources[s.id] = s;
        ++upserts;
    }
    void source_removed(const std::string& id) override {
        sources.erase(id);
        removed.push_back(id);
    }
    void meters(const std::vector<std::pair<std::string, uint8_t>>&) override {}
    std::vector<arc::SourceInfo> tabs() const {
        std::vector<arc::SourceInfo> v;
        for (auto& [id, s] : sources)
            if (s.tab) v.push_back(s);
        return v;
    }
};

class Link : public TabLink {
public:
    std::vector<std::string> sent;
    void send(const std::string& j) override { sent.push_back(j); }
};

std::string tabs_msg(const std::string& items) { return "{\"t\":\"tabs\",\"tabs\":[" + items + "]}"; }

std::string tab_json(int id, const std::string& title, bool audible = true, int volume = 10000,
                     const std::string& icon = "") {
    return "{\"id\":" + std::to_string(id) + ",\"title\":" + json::quote(title) +
           ",\"host\":\"youtube.com\",\"audible\":" + (audible ? "true" : "false") +
           ",\"muted\":false,\"volume\":" + std::to_string(volume) + ",\"icon\":" + json::quote(icon) + "}";
}

struct Rig {
    Inner inner;
    Sink sink;
    std::map<uint32_t, std::string> exes{{4242, "C:\\Program Files\\BraveSoftware\\Brave-Browser\\Application\\brave.exe"}};
    TabSources tabs{inner, [this](uint32_t pid) { return exes.count(pid) ? exes[pid] : std::string(); }};
    Link link;
    uint64_t conn = 0;
    Rig() {
        std::string err;
        tabs.start(sink, &err);
        conn = tabs.connect(&link);
    }
    void browser_session(const std::string& endpoint) {
        arc::SourceInfo s;
        s.id = "S-brave";
        s.name = "Brave";
        s.pid = 4242;
        s.endpoint_id = endpoint;
        inner.sink->source_upsert(s);
    }
};

}  // namespace

TEST(json_parses_objects_arrays_escapes_and_rejects_garbage) {
    json::Value v;
    // (JSON escapes spelled with \\u: MSVC rejects surrogate UCNs even in raw strings.)
    REQUIRE(json::parse("{\"t\":\"tabs\",\"n\":-1.5e2,\"ok\":true,\"z\":null,\"a\":[1,\"x\\u00e9\\ud83d\\ude00\\n\"]}", v));
    CHECK(v["t"].string_or() == "tabs");
    CHECK(v["n"].number_or(0) == -150);
    CHECK(v["ok"].bool_or(false));
    CHECK(v["a"].arr.size() == 2);
    CHECK(v["a"].arr[1].str == "x\xC3\xA9\xF0\x9F\x98\x80\n");
    CHECK(v["missing"]["deeper"].string_or("d") == "d");
    for (const char* bad : {"", "{", "{\"a\":1,}", "[1 2]", "{\"a\":1} x", "\"\\x\"", "\"\\ud800\"", "\"a\nb\"",
                            "01", "1.", "-", "tru", "{\"a\" 1}", "[[[[[[[[[[[1]]]]]]]]]]]"})
        CHECK(!json::parse(bad, v));
    json::Limits lim;
    lim.max_string = 4;
    CHECK(!json::parse("\"12345\"", v, lim));
    CHECK(json::quote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
    json::Value back;
    REQUIRE(json::parse(json::quote("tab \"quoted\" \xE2\x99\xAB"), back));
    CHECK(back.str == "tab \"quoted\" \xE2\x99\xAB");
}

TEST(tabs_appear_as_sources_on_their_browsers_output) {
    Rig r;
    REQUIRE(r.conn != 0);
    r.browser_session("OUT2");  // Brave plays on the headset
    r.tabs.message(r.conn, R"({"t":"host","exe":"C:\\Program Files\\BraveSoftware\\Brave-Browser\\Application\\BRAVE.EXE"})");
    r.tabs.message(r.conn, R"({"t":"hello","browser":"brave","version":"1.0.0"})");
    r.tabs.message(r.conn, tabs_msg(tab_json(7, "Lo-fi radio") + "," + tab_json(9, "", false, 4000)));
    auto tabs = r.sink.tabs();
    REQUIRE(tabs.size() == 2);
    const arc::SourceInfo& a = r.sink.sources["tab:1:7"];
    CHECK(a.tab && a.name == "Lo-fi radio" && a.detail == "youtube.com" && a.playing);
    CHECK(a.group == "Brave");
    CHECK(a.endpoint_id == "OUT2");  // same output as the browser's audio session
    const arc::SourceInfo& b = r.sink.sources["tab:1:9"];
    CHECK(b.name == "youtube.com");  // no title: the site
    CHECK(b.volume == 4000 && !b.playing);
    CHECK(r.tabs.tab_count() == 2);
    CHECK(r.tabs.features() & arc::kFeatureTabs);
    CHECK(r.tabs.features() & arc::kFeatureInputs);

    // The browser's session moves to the speakers: its tabs follow.
    r.browser_session("OUT1");
    CHECK(r.sink.sources["tab:1:7"].endpoint_id == "OUT1");

    // A tab closes; the browser goes away.
    r.tabs.message(r.conn, tabs_msg(tab_json(7, "Lo-fi radio")));
    CHECK(!r.sink.sources.count("tab:1:9"));
    r.tabs.disconnect(r.conn);
    CHECK(r.sink.tabs().empty());
}

TEST(tabs_without_a_known_browser_use_the_default_output) {
    Rig r;
    r.tabs.message(r.conn, R"({"t":"hello","browser":"firefox"})");
    r.tabs.message(r.conn, tabs_msg(tab_json(1, "Podcast")));
    const arc::SourceInfo& s = r.sink.sources["tab:1:1"];
    CHECK(s.endpoint_id == "OUT1");
    CHECK(s.group == "Firefox");
}

TEST(tab_volume_and_mute_go_to_the_extension_apps_to_the_os) {
    Rig r;
    r.tabs.message(r.conn, tabs_msg(tab_json(7, "Video")));
    r.tabs.set_source_volume("tab:1:7", 2500);
    r.tabs.set_source_mute("tab:1:7", true);
    REQUIRE(r.link.sent.size() == 2);
    json::Value v;
    REQUIRE(json::parse(r.link.sent[0], v));
    CHECK(v["t"].string_or() == "volume" && v["id"].number_or(0) == 7 && v["volume"].number_or(0) == 2500);
    REQUIRE(json::parse(r.link.sent[1], v));
    CHECK(v["t"].string_or() == "mute" && v["id"].number_or(0) == 7 && v["muted"].bool_or(false));
    CHECK(r.sink.sources["tab:1:7"].volume == 2500);  // optimistic
    CHECK(r.sink.sources["tab:1:7"].muted);
    CHECK(r.inner.volumes.empty() && r.inner.mutes.empty());

    r.tabs.set_source_volume("S-app", 5000);
    r.tabs.set_source_mute("S-app", true);
    r.tabs.set_source_route("S-app", "OUT2");
    r.tabs.set_source_route("tab:1:7", "OUT2");  // tabs follow their browser: ignored
    CHECK(r.inner.volumes.size() == 1 && r.inner.mutes.size() == 1);
    CHECK(r.inner.routes.size() == 1 && r.inner.routes[0].first == "S-app");
    r.tabs.set_source_volume("tab:1:99", 100);  // unknown tab: nothing sent
    CHECK(r.link.sent.size() == 2);
}

TEST(tab_favicon_is_served_as_an_icon) {
    Rig r;
    r.tabs.message(r.conn, tabs_msg(tab_json(3, "Song", true, 10000, kPngB64)));
    const std::string key = r.sink.sources["tab:1:3"].icon_key;
    REQUIRE(!key.empty());
    CHECK(key.rfind("tab:1:3:", 0) == 0);
    arc::Bytes png;
    REQUIRE(r.tabs.icon_png(key, png));
    CHECK(png.size() > 8 && png[1] == 'P');
    // Not base64 / not a PNG / oversized: no icon, the tab still shows.
    r.tabs.message(r.conn, tabs_msg(tab_json(3, "Song", true, 10000, "!!!!") + "," +
                                    tab_json(4, "B", true, 10000, "aGVsbG8gd29ybGQh") + "," +
                                    tab_json(5, "C", true, 10000, std::string(70000, 'A'))));
    CHECK(r.sink.sources["tab:1:3"].icon_key.empty());
    CHECK(r.sink.sources["tab:1:4"].icon_key.empty());
    CHECK(r.sink.sources.count("tab:1:5") && r.sink.sources["tab:1:5"].icon_key.empty());
    CHECK(!r.tabs.icon_png(key, png));
}

TEST(tab_messages_from_a_hostile_page_or_extension_are_bounded) {
    Rig r;
    // Garbage, wrong shapes and bad ids are ignored.
    for (const char* bad : {"not json", "[]", "{\"t\":\"tabs\"}", "{\"t\":\"tabs\",\"tabs\":{}}", "{\"t\":42}"})
        r.tabs.message(r.conn, bad);
    r.tabs.message(r.conn, tabs_msg("{\"id\":-1},{\"id\":1.5},{\"id\":\"7\"},{\"title\":\"no id\"},5"));
    CHECK(r.sink.tabs().empty());
    // Too many tabs: capped.
    std::string many;
    for (int i = 0; i < 150; ++i) many += (i ? "," : "") + tab_json(i, "t");
    r.tabs.message(r.conn, tabs_msg(many));
    CHECK(r.tabs.tab_count() == TabSources::kMaxTabsPerLink);
    // Long titles are cut on a UTF-8 boundary; control characters become spaces.
    std::string title;
    for (int i = 0; i < 200; ++i) title += "\xC3\xA9";  // 400 bytes of 'é'
    r.tabs.message(r.conn, tabs_msg(tab_json(1, title + "\x07")));
    const std::string& name = r.sink.sources["tab:1:1"].name;
    CHECK(name.size() <= arc::kMaxDisplayName);
    CHECK(name.size() % 2 == 0);  // never half a character
    r.tabs.message(r.conn, tabs_msg(tab_json(1, "bell\x07")));
    CHECK(r.sink.sources["tab:1:1"].name == "bell ");
    // Volume outside 0..10000 is clamped.
    r.tabs.message(r.conn, tabs_msg("{\"id\":2,\"volume\":99999},{\"id\":3,\"volume\":-5}"));
    CHECK(r.sink.sources["tab:1:2"].volume == 10000);
    CHECK(r.sink.sources["tab:1:3"].volume == 0);
    // A bogus browser name does not leak into the group.
    r.tabs.message(r.conn, R"({"t":"hello","browser":"<script>"})");
    CHECK(r.sink.sources["tab:1:2"].group == "Browser");
    // Connection limit.
    std::vector<Link> extra(TabSources::kMaxLinks);
    size_t ok = 1;
    for (auto& l : extra) ok += r.tabs.connect(&l) != 0;
    CHECK(ok == TabSources::kMaxLinks);
}

TEST(source_wire_carries_tab_fields_and_old_agents_still_decode) {
    arc::SourceInfo s;
    s.id = "tab:1:7";
    s.endpoint_id = "OUT1";
    s.name = "Lo-fi radio";
    s.volume = 6000;
    s.tab = true;
    s.group = "Brave";
    s.detail = "youtube.com";
    arc::Bytes msg = arc::encode_source(s);
    arc::Reader r(msg.data() + 1, msg.size() - 1);
    arc::SourceInfo d;
    REQUIRE(arc::decode_source(r, d));
    CHECK(d == s);
    // 1.2 agent: no group/detail, no tab bit.
    arc::SourceInfo app;
    app.id = "S1";
    app.name = "Spotify";
    app.route = "OUT2";
    arc::Bytes full = arc::encode_source(app);
    arc::Bytes old(full.begin(), full.end() - 4);  // drop the two empty 1.4 strings
    arc::Reader r2(old.data() + 1, old.size() - 1);
    arc::SourceInfo d2;
    REQUIRE(arc::decode_source(r2, d2));
    CHECK(d2.route == "OUT2" && !d2.tab && d2.group.empty() && d2.detail.empty());
}
