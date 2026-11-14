// Protocol 1.5: profiles — rules (days and hours, network, output present),
// wire and storage encoding, and the agent's engine end to end (save from the
// mixer, apply by hand, automatic on a network, restore when it ends, new apps,
// panic wins, persistence).

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "archon/client.hpp"
#include "archon/profiles.hpp"
#include "archon/server.hpp"
#include "test.hpp"

using namespace arc;
using namespace std::chrono_literals;

namespace {

bool eventually(const std::function<bool()>& f, int ms = 4000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (f()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return f();
}

ProfileContext ctx(int weekday, int hh, int mm, const std::string& net = "", std::set<std::string> outs = {}) {
    ProfileContext c;
    c.weekday = weekday;
    c.minute = hh * 60 + mm;
    c.network = net;
    c.outputs = std::move(outs);
    return c;
}

class Store : public TrustStore {
public:
    std::mutex mu;
    std::map<Id16, PairedController> items;
    std::optional<PairedController> find(const Id16& id) override {
        std::lock_guard<std::mutex> lk(mu);
        auto it = items.find(id);
        if (it == items.end()) return std::nullopt;
        return it->second;
    }
    bool put(const PairedController& c) override {
        std::lock_guard<std::mutex> lk(mu);
        items[c.id] = c;
        return true;
    }
    bool remove(const Id16& id) override {
        std::lock_guard<std::mutex> lk(mu);
        return items.erase(id) > 0;
    }
    std::vector<PairedController> list() override {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<PairedController> v;
        for (auto& [k, c] : items) v.push_back(c);
        return v;
    }
};

class Profiles : public ProfileStore {
public:
    std::mutex mu;
    Bytes blob;
    int saves = 0;
    bool load(Bytes& d) override {
        std::lock_guard<std::mutex> lk(mu);
        d = blob;
        return !blob.empty();
    }
    bool save(const Bytes& d) override {
        std::lock_guard<std::mutex> lk(mu);
        blob = d;
        ++saves;
        return true;
    }
};

// A mixer that applies what it is told and reports it back, like the OS does.
class Backend : public AudioBackend {
public:
    std::mutex mu;
    AudioSink* sink = nullptr;
    std::map<std::string, EndpointInfo> eps;
    std::map<std::string, SourceInfo> srcs;
    Backend() {
        eps["OUT1"] = EndpointInfo{"OUT1", "Speakers", 5000, false, true};
        add_source("S1", "Spotify", 8000);
        add_source("S2", "Discord", 9000);
    }
    void add_source(const std::string& id, const std::string& name, uint16_t vol) {
        SourceInfo s;
        s.id = id;
        s.endpoint_id = "OUT1";
        s.name = name;
        s.volume = vol;
        srcs[id] = s;
    }
    bool start(AudioSink& snk, std::string*) override {
        std::lock_guard<std::mutex> lk(mu);
        sink = &snk;
        for (auto& [id, e] : eps) sink->endpoint_upsert(e);
        for (auto& [id, s] : srcs) sink->source_upsert(s);
        return true;
    }
    void stop() override {
        std::lock_guard<std::mutex> lk(mu);
        sink = nullptr;
    }
    void set_endpoint_volume(const std::string& id, uint16_t v) override {
        std::lock_guard<std::mutex> lk(mu);
        if (!eps.count(id)) return;
        eps[id].volume = v;
        if (sink) sink->endpoint_upsert(eps[id]);
    }
    void set_endpoint_mute(const std::string& id, bool m) override {
        std::lock_guard<std::mutex> lk(mu);
        if (!eps.count(id)) return;
        eps[id].muted = m;
        if (sink) sink->endpoint_upsert(eps[id]);
    }
    void set_source_volume(const std::string& id, uint16_t v) override {
        std::lock_guard<std::mutex> lk(mu);
        if (!srcs.count(id)) return;
        srcs[id].volume = v;
        if (sink) sink->source_upsert(srcs[id]);
    }
    void set_source_mute(const std::string& id, bool m) override {
        std::lock_guard<std::mutex> lk(mu);
        if (!srcs.count(id)) return;
        srcs[id].muted = m;
        if (sink) sink->source_upsert(srcs[id]);
    }
    bool icon_png(const std::string&, Bytes&) override { return false; }
    // The user (or an app) changes something on the PC.
    void user_sets_endpoint(const std::string& id, uint16_t v) { set_endpoint_volume(id, v); }
    void plug(const std::string& id, const std::string& name) {
        std::lock_guard<std::mutex> lk(mu);
        eps[id] = EndpointInfo{id, name, 3000, false, false};
        if (sink) sink->endpoint_upsert(eps[id]);
    }
    void unplug(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu);
        eps.erase(id);
        if (sink) sink->endpoint_removed(id);
    }
    void app_starts(const std::string& id, const std::string& name, uint16_t vol) {
        std::lock_guard<std::mutex> lk(mu);
        add_source(id, name, vol);
        if (sink) sink->source_upsert(srcs[id]);
    }
    uint16_t ep_volume(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu);
        return eps[id].volume;
    }
    bool ep_muted(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu);
        return eps[id].muted;
    }
    SourceInfo src(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu);
        return srcs[id];
    }
};

class Listener : public ClientListener {
public:
    std::mutex mu;
    ClientStatus status = ClientStatus::Idle;
    uint32_t features = 0;
    std::map<std::string, Profile> profiles;
    std::string network;
    void on_status(ClientStatus s, ErrorCode, const std::string&) override {
        std::lock_guard<std::mutex> lk(mu);
        status = s;
    }
    void on_event(const ControllerEvent& ev) override {
        std::lock_guard<std::mutex> lk(mu);
        using K = ControllerEvent::Kind;
        if (ev.kind == K::StateReset) profiles.clear();
        if (ev.kind == K::AgentInfo) features = ev.agent.features;
        if (ev.kind == K::Profile) profiles[ev.profile.id] = ev.profile;
        if (ev.kind == K::ProfileRemoved) profiles.erase(ev.id);
        if (ev.kind == K::Context) network = ev.message;
    }
    bool live() {
        std::lock_guard<std::mutex> lk(mu);
        return status == ClientStatus::Connected && features != 0;
    }
    std::optional<Profile> named(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& [id, p] : profiles)
            if (p.name == name) return p;
        return std::nullopt;
    }
};

ClientTiming quick() {
    ClientTiming t;
    t.backoff_min_ms = 50;
    t.backoff_max_ms = 200;
    return t;
}

struct Rig {
    Backend be;
    Store store;
    Profiles saved;
    ServerObserver obs;
    ServerConfig cfg;
    std::unique_ptr<Server> srv;
    bool started = false;
    explicit Rig(bool with_profiles = true) {
        cfg.agent_id = random_array<16>();
        cfg.agent_name = "PC";
        cfg.loopback_only = true;
        cfg.port = 0;
        srv = std::make_unique<Server>(cfg, be, store, obs);
        if (with_profiles) srv->set_profile_store(&saved);
        std::string err;
        started = srv->start(&err);
    }
    ~Rig() { srv->stop(); }
    PairingRecord add_phone(const Id16& cid) {
        PairedController pc;
        pc.id = cid;
        pc.name = "phone";
        pc.key = random_array<32>();
        store.put(pc);
        PairingRecord rec;
        rec.agent_id = cfg.agent_id;
        rec.agent_name = "PC";
        rec.pair_key = pc.key;
        return rec;
    }
};

struct Phone {
    Id16 cid = random_array<16>();
    Listener l;
    Client c;
    explicit Phone(Rig& rig) : c(cid, "phone", l, quick()) { c.resume("127.0.0.1", rig.srv->port(), rig.add_phone(cid)); }
};

}  // namespace

TEST(v15_rules_days_hours_network_output) {
    Profile work;
    work.automatic = true;
    work.days = 0b0111110;  // Monday..Friday
    work.start_min = 9 * 60;
    work.end_min = 18 * 60;
    CHECK(profile_matches(work, ctx(1, 9, 0)));    // Monday 09:00
    CHECK(profile_matches(work, ctx(5, 17, 59)));  // Friday 17:59
    CHECK(!profile_matches(work, ctx(5, 18, 0)));  // end is exclusive
    CHECK(!profile_matches(work, ctx(6, 10, 0)));  // Saturday
    CHECK(!profile_matches(work, ctx(1, 8, 59)));

    Profile night;  // 22:00 -> 07:00, Friday and Saturday nights
    night.automatic = true;
    night.days = (1 << 5) | (1 << 6);
    night.start_min = 22 * 60;
    night.end_min = 7 * 60;
    CHECK(profile_matches(night, ctx(5, 23, 0)));   // Friday 23:00
    CHECK(profile_matches(night, ctx(6, 3, 0)));    // Saturday 03:00 belongs to Friday night
    CHECK(profile_matches(night, ctx(0, 6, 59)));   // Sunday 06:59 belongs to Saturday night
    CHECK(!profile_matches(night, ctx(1, 3, 0)));   // Monday 03:00 belongs to Sunday night: not listed
    CHECK(!profile_matches(night, ctx(5, 21, 59)));

    Profile home;
    home.automatic = true;
    home.network = "Casa 5G";
    CHECK(profile_matches(home, ctx(3, 12, 0, "Casa 5G")));
    CHECK(!profile_matches(home, ctx(3, 12, 0, "Escritorio")));

    Profile phones;
    phones.automatic = true;
    phones.output_id = "HEADSET";
    CHECK(profile_matches(phones, ctx(3, 12, 0, "", {"OUT1", "HEADSET"})));
    CHECK(!profile_matches(phones, ctx(3, 12, 0, "", {"OUT1"})));

    Profile manual = home;
    manual.automatic = false;
    CHECK(!profile_matches(manual, ctx(3, 12, 0, "Casa 5G")));
    Profile nothing;
    nothing.automatic = true;
    CHECK(!profile_matches(nothing, ctx(3, 12, 0)));  // automatic with no condition never fires

    // The most specific matching profile wins; on a tie, the first.
    Profile home_phones = home;
    home_phones.output_id = "HEADSET";
    std::vector<Profile> all{home, home_phones, phones};
    CHECK(pick_profile(all, ctx(3, 12, 0, "Casa 5G", {"HEADSET"})) == 1);
    CHECK(pick_profile(all, ctx(3, 12, 0, "Casa 5G", {"OUT1"})) == 0);
    CHECK(pick_profile(all, ctx(3, 12, 0, "Rua", {"HEADSET"})) == 2);
    CHECK(pick_profile(all, ctx(3, 12, 0, "Rua", {"OUT1"})) == -1);
}

TEST(v15_profile_wire_and_storage_roundtrip) {
    Profile p;
    p.id = "0123456789abcdef";
    p.name = "Trabalho";
    p.automatic = true;
    p.restore = true;
    p.days = 0b0111110;
    p.start_min = 540;
    p.end_min = 1080;
    p.network = "Casa 5G";
    p.output_id = "HEADSET";
    p.output_name = "Fone USB";
    p.endpoints = {{"OUT1", "Speakers", 3000, false}, {"MIC1", "Mic", 7000, true}};
    p.apps = {{"Spotify", 2000, false}, {"@system", 0, true}};
    p.active = true;
    Bytes msg = encode_profile(MsgType::Profile, p);
    Reader r(msg.data() + 1, msg.size() - 1);
    Profile d;
    uint8_t flags = 0;
    REQUIRE(decode_profile_body(r, d, &flags));
    CHECK(d == p);
    CHECK(!(flags & kProfileFlagCapture));

    std::vector<Profile> back;
    REQUIRE(parse_profiles(serialize_profiles({p, p}), back));
    REQUIRE(back.size() == 2);
    CHECK(!back[0].active);  // runtime state is not stored
    back[0].active = true;
    CHECK(back[0] == p);

    // Hostile or broken input.
    Bytes bad = serialize_profiles({p});
    bad[0] = 'X';
    CHECK(!parse_profiles(bad, back));
    Bytes cut = serialize_profiles({p});
    cut.resize(cut.size() - 3);
    CHECK(!parse_profiles(cut, back));
    Profile late = p;
    late.start_min = 24 * 60;  // 24:00 is not a time
    Bytes lm = encode_profile(MsgType::SaveProfile, late);
    Reader r2(lm.data() + 1, lm.size() - 1);
    CHECK(!decode_profile_body(r2, d));
    for (size_t n = 0; n + 1 < msg.size(); ++n) {  // every truncation is rejected
        Reader rt(msg.data() + 1, n);
        Profile t;
        CHECK(!decode_profile_body(rt, t));
    }
}

TEST(v15_capture_keeps_devices_and_apps_not_tabs) {
    std::map<std::string, EndpointInfo> eps{{"OUT1", EndpointInfo{"OUT1", "Speakers", 4000, true, true}}};
    std::map<std::string, SourceInfo> srcs;
    SourceInfo a;
    a.id = "S1";
    a.name = "Spotify";
    a.volume = 1234;
    srcs["S1"] = a;
    SourceInfo a2 = a;
    a2.id = "S1b";  // a second session of the same app
    srcs["S1b"] = a2;
    SourceInfo sys;
    sys.id = "SYS";
    sys.name = "System sounds";
    sys.system_sounds = true;
    srcs["SYS"] = sys;
    SourceInfo tab;
    tab.id = "tab:1:7";
    tab.name = "YouTube";
    tab.tab = true;
    srcs[tab.id] = tab;
    Profile p;
    profile_capture(p, eps, srcs);
    REQUIRE(p.endpoints.size() == 1);
    CHECK(p.endpoints[0].volume == 4000 && p.endpoints[0].muted);
    REQUIRE(p.apps.size() == 2);
    CHECK(p.apps[0].key == "@system" || p.apps[1].key == "@system");
    CHECK(p.apps[0].key == "Spotify" || p.apps[1].key == "Spotify");
}

TEST(v15_save_apply_and_persist_through_the_agent) {
    Rig rig;
    REQUIRE(rig.started);
    Phone ph(rig);
    REQUIRE(eventually([&] { return ph.l.live(); }));
    CHECK(ph.l.features & kFeatureProfiles);

    // Save the mixer as it is as "Normal".
    Profile p;
    p.name = "Normal";
    ph.c.save_profile(p, true);
    REQUIRE(eventually([&] { return ph.l.named("Normal").has_value(); }));
    const Profile normal = *ph.l.named("Normal");
    CHECK(normal.id.size() == 16);
    CHECK(normal.endpoints.size() == 1 && normal.apps.size() == 2);

    // Change things, then apply it by hand: everything goes back.
    rig.be.user_sets_endpoint("OUT1", 1000);
    rig.be.set_source_volume("S1", 100);
    rig.be.set_source_mute("S2", true);
    ph.c.apply_profile(normal.id);
    CHECK(eventually([&] { return rig.be.ep_volume("OUT1") == 5000; }));
    CHECK(eventually([&] { return rig.be.src("S1").volume == 8000 && !rig.be.src("S2").muted; }));

    // Rename (rules-only edit keeps the settings); persisted.
    Profile edit = normal;
    edit.name = "Normal (casa)";
    edit.endpoints.clear();
    edit.apps.clear();
    ph.c.save_profile(edit, false);
    REQUIRE(eventually([&] { return ph.l.named("Normal (casa)").has_value(); }));
    CHECK(ph.l.named("Normal (casa)")->apps.size() == 2);
    std::vector<Profile> stored;
    {
        std::lock_guard<std::mutex> lk(rig.saved.mu);
        REQUIRE(parse_profiles(rig.saved.blob, stored));
    }
    REQUIRE(stored.size() == 1);
    CHECK(stored[0].name == "Normal (casa)");

    // Delete.
    ph.c.delete_profile(normal.id);
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> lk(ph.l.mu);
        return ph.l.profiles.empty();
    }));
}

TEST(v15_automatic_on_a_network_restores_when_it_ends) {
    Rig rig;
    REQUIRE(rig.started);
    Phone ph(rig);
    REQUIRE(eventually([&] { return ph.l.live(); }));
    // "Escritório": everything quieter, Spotify muted; only on the office network.
    Profile office;
    office.name = "Escritorio";
    office.automatic = true;
    office.restore = true;
    office.network = "Office";
    office.endpoints = {{"OUT1", "Speakers", 2000, false}};
    office.apps = {{"Spotify", 8000, true}, {"Chrome", 3000, false}};
    ph.c.save_profile(office, false);
    REQUIRE(eventually([&] { return ph.l.named("Escritorio").has_value(); }));
    std::this_thread::sleep_for(100ms);
    CHECK(rig.be.ep_volume("OUT1") == 5000);  // not on that network yet

    rig.srv->network_changed("Office");
    CHECK(eventually([&] { return rig.be.ep_volume("OUT1") == 2000 && rig.be.src("S1").muted; }));
    CHECK(eventually([&] { return ph.l.named("Escritorio")->active; }));
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> lk(ph.l.mu);
        return ph.l.network == "Office";
    }));
    // An app that starts while the profile is in force gets its setting.
    rig.be.app_starts("S3", "Chrome", 10000);
    CHECK(eventually([&] { return rig.be.src("S3").volume == 3000; }));
    // The user mutes Discord by hand meanwhile: the profile does not fight it.
    rig.be.set_source_mute("S2", true);
    // Leaving the network: what the profile changed comes back; the user's own change stays.
    rig.srv->network_changed("Cafe");
    CHECK(eventually([&] { return rig.be.ep_volume("OUT1") == 5000 && !rig.be.src("S1").muted; }));
    CHECK(eventually([&] { return rig.be.src("S3").volume == 10000; }));
    CHECK(rig.be.src("S2").muted);
    CHECK(eventually([&] { return !ph.l.named("Escritorio")->active; }));
}

TEST(v15_headphones_plugged_in_switch_profile_and_panic_wins) {
    Rig rig;
    REQUIRE(rig.started);
    Phone ph(rig);
    REQUIRE(eventually([&] { return ph.l.live(); }));
    Profile phones;
    phones.name = "Fones";
    phones.automatic = true;
    phones.output_id = "HEADSET";
    phones.output_name = "Fone USB";
    phones.endpoints = {{"HEADSET", "Fone USB", 2500, false}, {"OUT1", "Speakers", 5000, true}};
    ph.c.save_profile(phones, false);
    REQUIRE(eventually([&] { return ph.l.named("Fones").has_value(); }));

    rig.be.plug("HEADSET", "Fone USB");
    CHECK(eventually([&] { return rig.be.ep_volume("HEADSET") == 2500 && rig.be.ep_muted("OUT1"); }));
    rig.be.unplug("HEADSET");
    CHECK(eventually([&] { return !ph.l.named("Fones")->active; }));

    // During a panic a profile never unmutes anything.
    rig.srv->panic_mute(true);
    CHECK(eventually([&] { return rig.be.ep_muted("OUT1"); }));
    Profile loud;
    loud.name = "Alto";
    loud.endpoints = {{"OUT1", "Speakers", 9000, false}};
    ph.c.save_profile(loud, false);
    REQUIRE(eventually([&] { return ph.l.named("Alto").has_value(); }));
    ph.c.apply_profile(ph.l.named("Alto")->id);
    CHECK(eventually([&] { return rig.be.ep_volume("OUT1") == 9000; }));
    std::this_thread::sleep_for(100ms);
    CHECK(rig.be.ep_muted("OUT1"));
}

TEST(v15_agent_without_profiles_ignores_them) {
    Rig rig(false);
    REQUIRE(rig.started);
    Phone ph(rig);
    REQUIRE(eventually([&] { return ph.l.live(); }));
    CHECK(!(ph.l.features & kFeatureProfiles));
    Profile p;
    p.name = "x";
    ph.c.save_profile(p, true);
    std::this_thread::sleep_for(150ms);
    std::lock_guard<std::mutex> lk(ph.l.mu);
    CHECK(ph.l.profiles.empty());
}

TEST(v15_profiles_survive_an_agent_restart) {
    Bytes blob;
    {
        Rig rig;
        REQUIRE(rig.started);
        Phone ph(rig);
        REQUIRE(eventually([&] { return ph.l.live(); }));
        Profile p;
        p.name = "Noite";
        p.automatic = true;
        p.days = 0x7F;
        p.start_min = 22 * 60;
        p.end_min = 7 * 60;
        ph.c.save_profile(p, false);
        REQUIRE(eventually([&] { return ph.l.named("Noite").has_value(); }));
        std::lock_guard<std::mutex> lk(rig.saved.mu);
        blob = rig.saved.blob;
    }
    Rig rig2;
    {
        std::lock_guard<std::mutex> lk(rig2.saved.mu);
        rig2.saved.blob = blob;
    }
    // The store was read at start(); restart with the blob in place.
    rig2.srv->stop();
    rig2.srv = std::make_unique<Server>(rig2.cfg, rig2.be, rig2.store, rig2.obs);
    rig2.srv->set_profile_store(&rig2.saved);
    std::string err2;
    REQUIRE(rig2.srv->start(&err2));
    Phone ph2(rig2);
    REQUIRE(eventually([&] { return ph2.l.live(); }));
    CHECK(eventually([&] { return ph2.l.named("Noite").has_value(); }));
    const Profile n = *ph2.l.named("Noite");
    CHECK(n.automatic && n.start_min == 22 * 60 && n.end_min == 7 * 60 && n.days == 0x7F);
}
