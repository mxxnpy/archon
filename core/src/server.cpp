#include "archon/server.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <tuple>

#include "archon/crypto.hpp"
#include "archon/frame.hpp"
#include "archon/net.hpp"
#include "archon/profiles.hpp"

namespace arc {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

static int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct Server::Conn {
    enum class St { AwaitHello, AwaitPairResponse, AwaitResumeConfirm, Ready };

    uint64_t id = 0;
    net::socket_t fd = net::kInvalidSocket;
    std::string address;
    St st = St::AwaitHello;
    FrameAssembler in;
    Bytes out;
    size_t out_pos = 0;
    Clock::time_point accepted;
    Clock::time_point deadline;  // handshake / close deadline
    Clock::time_point last_rx;
    bool closing = false;        // flush pending output, then close
    bool meters = false;         // subscribed to level meters
    bool dead = false;           // close immediately
    bool half_closed = false;    // FIN sent after the final error; waiting for the peer
    bool listed = false;         // currently reported to the observer as connected
    bool local = false;          // the agent's own window (no socket)
    bool streaming = false;      // 1.3: listening on the phone
    uint64_t stream_id = 0;      // unique per stream: a late failure of an old one cannot end a new one
    std::string stream_endpoint;

    Id16 controller_id{};
    std::string name;  // cleaned; bound into the handshake transcript

    // Pairing
    std::string code;
    Id16 sid{};
    std::unique_ptr<CpaceParty> cpace;
    // Resume
    std::unique_ptr<SessionKeys> keys;

    SecureChannel channel;

    bool pending() const { return st != St::Ready && !dead && !closing; }
    // Unauthenticated sockets that may be dropped to make room (pending or
    // closing after a handshake error).
    bool evictable() const { return st != St::Ready && !dead; }
};

struct Server::Impl {
    Server& s;
    net::Waker waker;
    net::socket_t listener = net::kInvalidSocket;

    std::mutex tasks_mu;
    std::deque<std::function<void()>> tasks;

    std::map<uint64_t, std::unique_ptr<Conn>> conns;
    uint64_t next_conn_id = 1;

    std::map<std::string, EndpointInfo> endpoints;
    std::map<std::string, SourceInfo> sources;

    std::map<std::string, uint16_t> slots;  // endpoint/source id -> meter slot
    std::set<uint16_t> used_slots;
    uint16_t next_slot = 1;
    bool metering = false;

    static constexpr size_t kMaxStreams = 2;
    uint64_t next_stream_id = 1;

    size_t stream_count() const {
        size_t n = 0;
        for (auto& [id, c] : conns) n += c->streaming ? 1 : 0;
        return n;
    }

    void stream_changed() {
        s.active_streams_ = stream_count();
        s.observer_.on_streams_changed(s.active_streams_);
    }

    void send_stream_state(Conn& c, bool active, const Key32* key, ErrorCode err) {
        StreamState st;
        st.active = active;
        if (active) {
            st.codec = kStreamCodecOpus;
            st.sample_rate = 48000;
            st.channels = 2;
            st.frame_ms = 20;
            st.key = *key;
        }
        st.error = uint16_t(err);
        Bytes msg = encode_stream_state(st);
        wipe(st.key.data(), st.key.size());
        queue_app(c, msg);  // sealed copy only; the plaintext holding the key is wiped
        wipe(msg.data(), msg.size());
    }

    // Ends c's stream: stop capture, undo "mute the PC", tell the phone (if it is
    // still there) why.
    void end_stream(Conn& c, ErrorCode reason, bool notify) {
        if (!c.streaming) return;
        c.streaming = false;
        if (s.stream_service_) s.stream_service_->stop(c.stream_id);
        release_stream_mute(c);
        if (notify) send_stream_state(c, false, nullptr, reason);
        log("stream stopped for " + c.name);
        stream_changed();
    }

    void start_stream(Conn& c, const StreamStart& req) {
        if (!s.stream_service_ || c.local) return send_stream_state(c, false, nullptr, ErrorCode::Busy);
        std::string ep = req.endpoint_id;
        if (ep.empty())
            for (auto& [id, e] : endpoints)
                if (e.is_default && !e.input) ep = id;
        auto it = endpoints.find(ep);
        if (req.udp_port == 0 || it == endpoints.end() || it->second.input)
            return send_stream_state(c, false, nullptr, ErrorCode::Protocol);
        if (c.streaming) end_stream(c, ErrorCode::None, false);  // restart (other output / port)
        if (stream_count() >= kMaxStreams) return send_stream_state(c, false, nullptr, ErrorCode::Busy);
        Key32 key = random_array<32>();
        // The key goes out first so the phone can open the first datagrams
        // (UDP can still win the race; the receiver keeps a few early ones).
        send_stream_state(c, true, &key, ErrorCode::None);
        std::string err;
        const uint64_t sid = next_stream_id++;
        if (!s.stream_service_->start(sid, ep, c.address, req.udp_port, key, &err)) {
            wipe(key.data(), key.size());
            log("stream could not start: " + err);
            return send_stream_state(c, false, nullptr, ErrorCode::Busy);
        }
        wipe(key.data(), key.size());
        c.streaming = true;
        c.stream_id = sid;
        c.stream_endpoint = ep;
        if (req.flags & kStreamFlagMutePc) hold_stream_mute(c, ep);
        log("stream started for " + c.name);
        stream_changed();
    }

    // Panic mute: devices this agent muted, so turning it off restores only those.
    bool panic = false;
    std::set<std::string> panic_muted;

    // "Mute the PC while I listen": endpoint -> the streams (conn ids) holding
    // that mute. The output comes back only when the last of them ends, and
    // not during a panic (it then belongs to the panic). A device the user
    // muted by hand is never claimed, so a stream never unmutes it.
    std::map<std::string, std::set<uint64_t>> stream_mutes;

    void hold_stream_mute(Conn& c, const std::string& ep) {
        auto it = endpoints.find(ep);
        if (it == endpoints.end()) return;
        auto held = stream_mutes.find(ep);
        if (!it->second.muted) {
            stream_mutes[ep].insert(c.id);
            request_mute(ep, true);
        } else if (held != stream_mutes.end() && !held->second.empty()) {
            held->second.insert(c.id);  // another stream already muted it: share
        } else if (panic_muted.erase(ep)) {
            stream_mutes[ep].insert(c.id);  // the stream keeps it muted; the panic gets it back when it ends
        }
    }

    void release_stream_mute(Conn& c) {
        auto held = stream_mutes.find(c.stream_endpoint);
        if (held == stream_mutes.end() || held->second.erase(c.id) == 0 || !held->second.empty()) return;
        stream_mutes.erase(held);
        auto it = endpoints.find(c.stream_endpoint);
        if (it == endpoints.end() || !it->second.muted) return;
        if (panic)
            panic_muted.insert(c.stream_endpoint);  // stays muted until the panic ends
        else
            request_mute(c.stream_endpoint, false);
    }

    // Mutes this server asked for and the OS has not confirmed yet. The OS
    // applies them asynchronously, so until the echo arrives (or 1 s passes)
    // the requested value is the truth for every decision (panic, streams).
    struct PendingMute {
        bool muted;
        Clock::time_point until;
        std::optional<bool> reported;  // a contradicting OS report we held back
    };
    std::map<std::string, PendingMute> pending_mute;

    void request_mute(const std::string& id, bool muted) {
        auto it = endpoints.find(id);
        if (it == endpoints.end()) return;
        s.backend_.set_endpoint_mute(id, muted);
        pending_mute[id] = PendingMute{muted, Clock::now() + std::chrono::seconds(1), std::nullopt};
        if (it->second.muted != muted) {
            it->second.muted = muted;  // optimistic: every controller sees it at once
            broadcast(encode_endpoint(it->second));
        }
    }

    // Pairing policy
    uint64_t pairing_conn = 0;
    std::deque<Clock::time_point> failures;
    Clock::time_point locked_until{};
    int lockouts = 0;                                    // consecutive, for escalation
    std::map<std::string, Clock::time_point> last_pair_request;  // by address
    std::deque<Clock::time_point> pair_requests;         // last minute, all sources

    Clock::time_point last_refused_log{};
    Clock::time_point last_auth_log{};

    // Revoked in this run: denied even if the store failed to persist the removal.
    std::set<Id16> revoked;

    // Icons are fetched off the network thread (the OS can take a while).
    std::thread icon_thread;
    std::mutex icon_mu;
    std::condition_variable icon_cv;
    std::deque<std::pair<uint64_t, std::string>> icon_jobs;
    bool icon_stop = false;

    explicit Impl(Server& srv) : s(srv) {
        lockouts = s.cfg_.initial_lockouts;
        const int64_t left = s.cfg_.locked_until_unix - unix_now();
        if (left > 0) locked_until = Clock::now() + std::chrono::seconds(std::min<int64_t>(left, 24 * 3600));
    }

    void log(const std::string& line) { s.observer_.on_log(line); }

    // ---- output ---------------------------------------------------------------

    void queue_raw(Conn& c, const Bytes& payload) {
        if (c.dead) return;
        append_frame(c.out, payload);
        if (c.out.size() - c.out_pos > s.cfg_.max_output_buffer) {
            log("controller " + c.address + " is not reading; disconnecting");
            c.dead = true;
        }
    }

    // The agent's own window (see LocalView).
    std::unique_ptr<Conn> local_conn;
    LocalView* local_view = nullptr;

    void queue_app(Conn& c, const Bytes& msg) {
        if (c.local) {
            if (local_view) local_view->on_message(msg);
            return;
        }
        if (c.st != Conn::St::Ready || c.closing) return;
        queue_raw(c, c.channel.seal(msg));
    }

    void send_error_and_close(Conn& c, ErrorCode code, const std::string& text) {
        ErrorMsg e{code, text};
        const Bytes msg = encode_error(e);
        if (c.st == Conn::St::Ready)
            queue_raw(c, c.channel.seal(msg));  // authenticated (e.g. revoked)
        else
            queue_raw(c, msg);  // plaintext: controllers treat it as unverified
        c.closing = true;
        // Authenticated links get time to read a final ERROR (e.g. revoked); a
        // failed handshake only needs a moment and must not hold a slot.
        c.deadline = Clock::now() + (c.st == Conn::St::Ready ? milliseconds(5000) : milliseconds(1000));
    }

    void broadcast(const Bytes& msg) {
        for (auto& [id, c] : conns) queue_app(*c, msg);
        if (local_conn) queue_app(*local_conn, msg);
    }

    AgentInfo agent_info() const {
        AgentInfo a;
        a.features = s.backend_.features() | kFeaturePanic | (s.stream_service_ ? kFeatureStream : 0u) |
                     (s.profile_store_ ? kFeatureProfiles : 0u);
        a.panic = panic;
        a.os = s.backend_.os_name();
        return a;
    }

    // A request the OS never confirmed: trust the last thing it reported (the
    // user may have changed it by hand on the PC while ours was in flight).
    void expire_pending_mutes(Clock::time_point now) {
        for (auto it = pending_mute.begin(); it != pending_mute.end();) {
            if (now < it->second.until) {
                ++it;
                continue;
            }
            auto ep = endpoints.find(it->first);
            if (it->second.reported && ep != endpoints.end() && ep->second.muted != *it->second.reported) {
                ep->second.muted = *it->second.reported;
                if (!ep->second.muted) panic_muted.erase(ep->first);
                broadcast(encode_endpoint(ep->second));
            }
            it = pending_mute.erase(it);
        }
    }

    void set_panic(bool on) {
        if (on) {
            std::vector<std::string> ids;
            for (auto& [id, e] : endpoints)
                if (!e.muted && !panic_muted.count(id)) ids.push_back(id);
            for (const std::string& id : ids) {
                panic_muted.insert(id);
                request_mute(id, true);
            }
        } else {
            const std::set<std::string> ours = std::move(panic_muted);
            panic_muted.clear();
            for (const std::string& id : ours) {
                auto it = endpoints.find(id);
                if (it != endpoints.end() && it->second.muted) request_mute(id, false);
            }
        }
        if (panic == on) return;
        panic = on;
        broadcast(encode_agent_info(agent_info()));
        s.observer_.on_panic_changed(on);
        log(on ? "panic mute on" : "panic mute off");
    }

    // ---- profiles (1.5) -----------------------------------------------------------

    std::vector<Profile> profiles;
    std::string network;           // the PC's network now
    std::string auto_active;       // id of the automatic profile in force ("" = none)
    bool profiles_dirty_eval = false;
    Clock::time_point next_profile_eval{};
    // What an automatic profile changed, to put it back when it ends ("restore").
    struct Change {
        uint16_t before_volume, set_volume;
        bool before_muted, set_muted;
    };
    std::map<std::string, Change> ep_changes;   // endpoint id
    std::map<std::string, Change> app_changes;  // source id
    std::set<std::string> app_applied;          // sources the profile in force already handled

    bool profiles_on() const { return s.profile_store_ != nullptr; }

    void load_profiles() {
        if (!profiles_on()) return;
        Bytes data;
        if (s.profile_store_->load(data) && !parse_profiles(data, profiles)) {
            log("profiles file unreadable; starting with none");
            profiles.clear();
        }
        next_profile_eval = Clock::now() + std::chrono::milliseconds(1500);  // after the first device report
    }

    void save_profiles() {
        if (profiles_on() && !s.profile_store_->save(serialize_profiles(profiles))) log("could not save profiles");
    }

    Profile* find_profile(const std::string& id) {
        for (Profile& p : profiles)
            if (p.id == id) return &p;
        return nullptr;
    }

    ProfileContext profile_context() const {
        ProfileContext ctx;
        const std::time_t t = std::time(nullptr);
        std::tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        ctx.weekday = lt.tm_wday;
        ctx.minute = lt.tm_hour * 60 + lt.tm_min;
        ctx.network = network;
        for (auto& [id, e] : endpoints)
            if (!e.input) ctx.outputs.insert(id);
        return ctx;
    }

    const EndpointInfo* endpoint_for(const ProfileEndpoint& pe) const {
        auto it = endpoints.find(pe.id);
        if (it != endpoints.end()) return &it->second;
        const EndpointInfo* by_name = nullptr;  // the device came back with another id: same name, if unique
        for (auto& [id, e] : endpoints)
            if (e.name == pe.name) {
                if (by_name) return nullptr;
                by_name = &e;
            }
        return by_name;
    }

    void set_endpoint(const EndpointInfo& e, uint16_t volume, bool muted, bool track) {
        if (track && !ep_changes.count(e.id)) ep_changes[e.id] = Change{e.volume, volume, e.muted, muted};
        if (e.volume != volume) s.backend_.set_endpoint_volume(e.id, volume);
        // A panic keeps everything muted: a profile never unmutes during one.
        if (e.muted != muted && !(panic && !muted)) request_mute(e.id, muted);
    }

    void set_app(const SourceInfo& src, const ProfileApp& a, bool track) {
        if (track && !app_changes.count(src.id)) app_changes[src.id] = Change{src.volume, a.volume, src.muted, a.muted};
        if (src.volume != a.volume) s.backend_.set_source_volume(src.id, a.volume);
        if (src.muted != a.muted) s.backend_.set_source_mute(src.id, a.muted);
    }

    void apply_profile(const Profile& p, bool track) {
        for (const ProfileEndpoint& pe : p.endpoints)
            if (const EndpointInfo* e = endpoint_for(pe)) set_endpoint(*e, pe.volume, pe.muted, track);
        for (auto& [id, src] : sources) {
            if (src.tab) continue;
            const std::string key = profile_app_key(src);
            for (const ProfileApp& a : p.apps)
                if (a.key == key) {
                    set_app(src, a, track);
                    if (track) app_applied.insert(id);
                    break;
                }
        }
        log("profile applied: " + p.name);
    }

    // Puts back what the ending profile changed, unless the user changed it since.
    void restore_changes() {
        for (auto& [id, ch] : ep_changes) {
            auto it = endpoints.find(id);
            if (it == endpoints.end()) continue;
            const EndpointInfo& e = it->second;
            if (e.volume == ch.set_volume && ch.before_volume != ch.set_volume)
                s.backend_.set_endpoint_volume(id, ch.before_volume);
            if (e.muted == ch.set_muted && ch.before_muted != ch.set_muted && !(panic && !ch.before_muted))
                request_mute(id, ch.before_muted);
        }
        for (auto& [id, ch] : app_changes) {
            auto it = sources.find(id);
            if (it == sources.end()) continue;
            if (it->second.volume == ch.set_volume && ch.before_volume != ch.set_volume)
                s.backend_.set_source_volume(id, ch.before_volume);
            if (it->second.muted == ch.set_muted && ch.before_muted != ch.set_muted)
                s.backend_.set_source_mute(id, ch.before_muted);
        }
    }

    void end_auto_profile() {
        Profile* old = find_profile(auto_active);
        if (old && old->restore) restore_changes();
        if (old) {
            old->active = false;
            broadcast(encode_profile(MsgType::Profile, *old));
        }
        auto_active.clear();
        ep_changes.clear();
        app_changes.clear();
        app_applied.clear();
    }

    void evaluate_profiles(Clock::time_point now) {
        if (!profiles_on() || next_profile_eval == Clock::time_point{}) return;
        if (!profiles_dirty_eval && now < next_profile_eval) return;
        profiles_dirty_eval = false;
        next_profile_eval = now + std::chrono::seconds(20);
        const int best = pick_profile(profiles, profile_context());
        const std::string want = best >= 0 ? profiles[size_t(best)].id : std::string();
        if (want == auto_active) return;  // edges only: the user stays free to change things meanwhile
        if (!auto_active.empty()) end_auto_profile();
        if (want.empty()) return;
        Profile& p = profiles[size_t(best)];
        auto_active = p.id;
        p.active = true;
        apply_profile(p, true);
        broadcast(encode_profile(MsgType::Profile, p));
    }

    // An app that starts while a profile is in force gets that profile's setting once.
    void profile_new_source(const SourceInfo& src) {
        if (auto_active.empty() || src.tab || app_applied.count(src.id)) return;
        const Profile* p = find_profile(auto_active);
        if (!p) return;
        const std::string key = profile_app_key(src);
        for (const ProfileApp& a : p->apps)
            if (a.key == key) {
                app_applied.insert(src.id);
                set_app(src, a, true);
                return;
            }
    }

    void on_save_profile(Reader& r) {
        Profile in;
        uint8_t flags = 0;
        if (!profiles_on() || !decode_profile_body(r, in, &flags)) return;
        if (in.name.empty()) return;
        if (in.automatic && profile_specificity(in) == 0) in.automatic = false;  // nothing to wait for
        if (in.output_id.empty()) in.output_name.clear();
        in.active = false;
        Profile* p = in.id.empty() ? nullptr : find_profile(in.id);
        if (!in.id.empty() && !p) return;  // unknown id (deleted meanwhile)
        if (!p) {
            if (profiles.size() >= kMaxProfiles) return;
            in.id = to_hex(random_array<8>());
            const bool empty = in.endpoints.empty() && in.apps.empty();
            profiles.push_back(in);
            p = &profiles.back();
            if (empty) profile_capture(*p, endpoints, sources);  // a new profile starts from the mixer as it is
        } else {
            const bool was_active = p->active;
            std::vector<ProfileEndpoint> eps = p->endpoints;
            std::vector<ProfileApp> apps = p->apps;
            *p = in;
            p->active = was_active;
            if (in.endpoints.empty() && in.apps.empty()) {  // a rules-only edit keeps the settings
                p->endpoints = std::move(eps);
                p->apps = std::move(apps);
            }
        }
        if (flags & kProfileFlagCapture) profile_capture(*p, endpoints, sources);
        save_profiles();
        broadcast(encode_profile(MsgType::Profile, *p));
        profiles_dirty_eval = true;
    }

    void on_delete_profile(const std::string& id) {
        auto it = std::find_if(profiles.begin(), profiles.end(), [&](const Profile& p) { return p.id == id; });
        if (it == profiles.end()) return;
        if (auto_active == id) end_auto_profile();
        profiles.erase(std::find_if(profiles.begin(), profiles.end(), [&](const Profile& p) { return p.id == id; }));
        save_profiles();
        broadcast(encode_id(MsgType::ProfileRemoved, id));
        profiles_dirty_eval = true;
    }

    void send_dump(Conn& c) {
        queue_app(c, encode_simple(MsgType::StateReset));
        queue_app(c, encode_agent_info(agent_info()));
        if (profiles_on()) {
            queue_app(c, encode_context(network));
            for (const Profile& p : profiles) queue_app(c, encode_profile(MsgType::Profile, p));
        }
        for (auto& [id, e] : endpoints) queue_app(c, encode_endpoint(e));
        for (auto& [id, src] : sources) queue_app(c, encode_source(src));
        queue_app(c, encode_simple(MsgType::StateSynced));
    }

    uint16_t slot_for(const std::string& id) {
        auto it = slots.find(id);
        if (it != slots.end()) return it->second;
        // Skip 0 ("no meter") and slots still owned by a live item after a wrap.
        while (next_slot == 0 || used_slots.count(next_slot)) ++next_slot;
        const uint16_t slot = next_slot++;
        slots[id] = slot;
        used_slots.insert(slot);
        return slot;
    }

    void free_slot(const std::string& id) {
        auto it = slots.find(id);
        if (it == slots.end()) return;
        used_slots.erase(it->second);
        slots.erase(it);
    }

    void update_metering() {
        bool any = false;
        for (auto& [id, c] : conns)
            if (c->meters && c->st == Conn::St::Ready && !c->closing && !c->dead) any = true;
        if (local_conn && local_conn->meters) any = true;
        if (any == metering) return;
        metering = any;
        s.backend_.set_metering(any);
    }

    // Authenticated links, not counting `except` nor older links of `same`
    // controller (a reconnect replaces them, so it must never be refused as busy).
    size_t ready_count(const Conn* except = nullptr, const Id16* same = nullptr) const {
        size_t n = 0;
        for (auto& [id, c] : conns) {
            if (c.get() == except || c->st != Conn::St::Ready || c->closing || c->dead) continue;
            if (same && c->controller_id == *same) continue;
            ++n;
        }
        return n;
    }

    void notify_controllers() {
        std::vector<ConnectedController> list;
        for (auto& [id, c] : conns) {
            if (c->st != Conn::St::Ready || c->closing || c->dead) continue;
            list.push_back(ConnectedController{c->id, c->controller_id, c->name, c->address});
        }
        s.observer_.on_controllers_changed(list);
    }

    // ---- pairing policy -------------------------------------------------------

    bool pairing_locked(Clock::time_point now) const { return now < locked_until; }

    void record_failure(Clock::time_point now) {
        failures.push_back(now);
        while (!failures.empty() && now - failures.front() > milliseconds(s.cfg_.failure_window_ms)) failures.pop_front();
        if (int(failures.size()) >= s.cfg_.max_failed_pairings) {
            // Each consecutive lockout doubles: 5 min, 10, 20 … capped at 24 h.
            const int64_t base = s.cfg_.lockout_ms;
            const int64_t ms = std::min<int64_t>(base << std::min(lockouts, 16), int64_t(24) * 3600 * 1000);
            ++lockouts;
            locked_until = now + milliseconds(ms);
            failures.clear();
            s.observer_.on_pairing_lockout(lockouts, unix_now() + ms / 1000);
            log("too many wrong pairing codes; pairing locked for " + std::to_string(ms / 1000) + " s");
        }
    }

    // Flood control for pairing popups. Returns false if this request must be refused.
    bool admit_pair_request(const std::string& addr, Clock::time_point now) {
        while (!pair_requests.empty() && now - pair_requests.front() > std::chrono::minutes(1)) pair_requests.pop_front();
        if (int(pair_requests.size()) >= s.cfg_.max_pair_requests_per_minute) return false;
        auto it = last_pair_request.find(addr);
        if (it != last_pair_request.end() && now - it->second < milliseconds(s.cfg_.pair_request_interval_ms))
            return false;
        if (last_pair_request.size() > 256) last_pair_request.clear();  // bounded memory
        last_pair_request[addr] = now;
        pair_requests.push_back(now);
        return true;
    }

    void finish_pairing(Conn& c, bool ok) {
        if (pairing_conn != c.id) return;
        pairing_conn = 0;
        if (!c.code.empty()) wipe(c.code.data(), c.code.size());
        c.code.clear();
        c.cpace.reset();
        s.observer_.on_pair_finished(c.id, ok, c.name);
    }

    // ---- protocol ---------------------------------------------------------------

    void become_ready(Conn& c, const Key32& key_a2c, const Key32& key_c2a) {
        // Concurrent handshakes may have filled the slots meanwhile.
        if (ready_count(&c, &c.controller_id) >= s.cfg_.max_controllers) {
            send_error_and_close(c, ErrorCode::Busy, "too many controllers");
            return;
        }
        c.channel.init(key_a2c, key_c2a);
        c.st = Conn::St::Ready;
        c.last_rx = Clock::now();
        // A controller reconnecting replaces its previous (probably dead) link.
        for (auto& [id, other] : conns) {
            if (other.get() != &c && other->st == Conn::St::Ready && other->controller_id == c.controller_id)
                other->dead = true;
        }
        c.listed = true;
        send_dump(c);
        log("controller connected: " + c.name + " (" + c.address + ")");
        notify_controllers();
    }

    void on_hello(Conn& c, Reader& r, Clock::time_point now) {
        Hello h;
        if (!decode_hello(r, h)) return send_error_and_close(c, ErrorCode::Protocol, "bad hello");
        if (h.version != kProtocolVersion)
            return send_error_and_close(c, ErrorCode::UnsupportedVersion, "protocol version 1 required");
        c.controller_id = h.controller_id;
        c.name = h.name;  // already cleaned by the decoder; used verbatim in the transcript

        if (ready_count(nullptr, &c.controller_id) >= s.cfg_.max_controllers)
            return send_error_and_close(c, ErrorCode::Busy, "too many controllers");

        HelloAck ack;
        ack.agent_id = s.cfg_.agent_id;
        ack.name = s.cfg_.agent_name;
        ack.mode = h.mode;

        if (h.mode == Mode::Pair) {
            if (pairing_locked(now))
                return send_error_and_close(c, ErrorCode::PairingLocked, "pairing locked, try later");
            if (pairing_conn != 0) return send_error_and_close(c, ErrorCode::Busy, "pairing in progress");
            if (!admit_pair_request(c.address, now))
                return send_error_and_close(c, ErrorCode::Busy, "too many pairing requests");
            c.code = generate_pairing_code();
            c.sid = random_array<16>();
            c.cpace = std::make_unique<CpaceParty>();
            cpace_start(cpace_generator(c.code, c.sid, s.cfg_.agent_id, c.controller_id), *c.cpace);
            ack.sid = c.sid;
            ack.y = c.cpace->y;
            queue_raw(c, encode_hello_ack(ack));
            c.st = Conn::St::AwaitPairResponse;
            c.deadline = now + milliseconds(s.cfg_.code_timeout_ms);
            pairing_conn = c.id;
            log("pairing requested by " + c.name + " (" + c.address + ")");
            s.observer_.on_pair_request(c.id, c.name.empty() ? std::string("?") : c.name, c.code);
            return;
        }

        if (revoked.count(c.controller_id)) return send_error_and_close(c, ErrorCode::UnknownController, "not paired");
        std::optional<PairedController> rec = s.store_.find(c.controller_id);
        if (!rec) return send_error_and_close(c, ErrorCode::UnknownController, "not paired");
        const Key32 nonce_s = random_array<32>();
        c.keys = std::make_unique<SessionKeys>(derive_resume_keys(rec->key, c.controller_id, s.cfg_.agent_id, h.nonce,
                                                                  nonce_s, c.name, s.cfg_.agent_name));
        wipe(rec->key.data(), rec->key.size());
        ack.nonce = nonce_s;
        ack.tag = c.keys->tag_agent;
        queue_raw(c, encode_hello_ack(ack));
        c.st = Conn::St::AwaitResumeConfirm;
        c.deadline = now + milliseconds(s.cfg_.handshake_timeout_ms);
    }

    void on_pair_response(Conn& c, Reader& r, Clock::time_point now) {
        Key32 yc, tag;
        if (!decode_two_keys(r, yc, tag)) {
            finish_pairing(c, false);
            return send_error_and_close(c, ErrorCode::Protocol, "bad pair response");
        }
        Key32 k;
        bool ok = c.cpace && cpace_finish(*c.cpace, yc, k);
        std::unique_ptr<SessionKeys> keys;
        if (ok) {
            keys = std::make_unique<SessionKeys>(derive_pair_keys(k, c.sid, s.cfg_.agent_id, c.controller_id, c.cpace->y,
                                                                  yc, c.name, s.cfg_.agent_name));
            ok = ct_equal(keys->tag_controller, tag);
        }
        wipe(k.data(), k.size());
        if (!ok) {
            record_failure(now);
            log("wrong pairing code from " + c.name + " (" + c.address + ")");
            finish_pairing(c, false);
            return send_error_and_close(c, ErrorCode::WrongCode, "wrong code");
        }
        if (ready_count(nullptr, &c.controller_id) >= s.cfg_.max_controllers) {
            finish_pairing(c, false);
            return send_error_and_close(c, ErrorCode::Busy, "too many controllers");
        }
        PairedController pc;
        pc.id = c.controller_id;
        pc.name = c.name;
        pc.key = keys->pair_key;
        pc.paired_at = pc.last_seen = unix_now();
        const bool saved = s.store_.put(pc);
        wipe(pc.key.data(), pc.key.size());
        if (!saved) {
            finish_pairing(c, false);
            return send_error_and_close(c, ErrorCode::Busy, "cannot save pairing");
        }
        revoked.erase(c.controller_id);  // explicitly paired again on the PC
        if (lockouts != 0) {
            lockouts = 0;
            s.observer_.on_pairing_lockout(0, 0);
        }
        queue_raw(c, encode_pair_confirm(keys->tag_agent));
        finish_pairing(c, true);
        become_ready(c, keys->key_a2c, keys->key_c2a);
    }

    void on_resume_confirm(Conn& c, Reader& r) {
        Key32 tag;
        if (!decode_key(r, tag) || !c.keys || !ct_equal(c.keys->tag_controller, tag)) {
            if (Clock::now() - last_auth_log > std::chrono::minutes(1)) {
                last_auth_log = Clock::now();
                log("authentication failed for " + c.name + " (" + c.address + ") (logged at most once a minute)");
            }
            return send_error_and_close(c, ErrorCode::AuthFailed, "authentication failed");
        }
        // The pairing may have been revoked while the handshake was in flight.
        std::optional<PairedController> rec = s.store_.find(c.controller_id);
        if (!rec || revoked.count(c.controller_id))
            return send_error_and_close(c, ErrorCode::UnknownController, "not paired");
        rec->last_seen = unix_now();
        rec->name = c.name;
        s.store_.put(*rec);
        wipe(rec->key.data(), rec->key.size());
        std::unique_ptr<SessionKeys> keys = std::move(c.keys);
        become_ready(c, keys->key_a2c, keys->key_c2a);
    }

    void request_icon(uint64_t conn_id, const std::string& key) {
        {
            std::lock_guard<std::mutex> lk(icon_mu);
            if (icon_jobs.size() >= 64) return;  // bounded; clients retry on reconnect
            icon_jobs.emplace_back(conn_id, key);
        }
        icon_cv.notify_one();
    }

    void icon_loop() {
        for (;;) {
            std::pair<uint64_t, std::string> job;
            {
                std::unique_lock<std::mutex> lk(icon_mu);
                icon_cv.wait(lk, [&] { return icon_stop || !icon_jobs.empty(); });
                if (icon_stop) return;
                job = std::move(icon_jobs.front());
                icon_jobs.pop_front();
            }
            Bytes png;
            if (!s.backend_.icon_png(job.second, png) || png.size() > kMaxIconBytes) png.clear();
            s.post([this, conn_id = job.first, key = job.second, png = std::move(png)] {
                auto it = conns.find(conn_id);
                if (it != conns.end()) queue_app(*it->second, encode_icon(key, png.data(), png.size()));
            });
        }
    }

    // Tells the PC (on-screen notice) what a phone changed. Not for the local window.
    void remote_change(const Conn& c, ServerObserver::RemoteChange ch) {
        if (c.local) return;
        ch.controller = c.name;
        s.observer_.on_remote_change(ch);
    }
    void remote_endpoint(const Conn& c, const std::string& id, bool mute, uint16_t vol, bool muted) {
        auto it = endpoints.find(id);
        if (it == endpoints.end()) return;
        ServerObserver::RemoteChange ch;
        ch.kind = mute ? ServerObserver::RemoteChange::Kind::Mute : ServerObserver::RemoteChange::Kind::Volume;
        ch.target = it->second.name;
        ch.device = true;
        ch.input = it->second.input;
        ch.volume = mute ? it->second.volume : vol;
        ch.on = mute ? muted : it->second.muted;
        remote_change(c, ch);
    }
    void remote_source(const Conn& c, const std::string& id, bool mute, uint16_t vol, bool muted) {
        auto it = sources.find(id);
        if (it == sources.end()) return;
        ServerObserver::RemoteChange ch;
        ch.kind = mute ? ServerObserver::RemoteChange::Kind::Mute : ServerObserver::RemoteChange::Kind::Volume;
        ch.target = it->second.name;
        ch.system = it->second.system_sounds;
        ch.volume = mute ? it->second.volume : vol;
        ch.on = mute ? muted : it->second.muted;
        remote_change(c, ch);
    }

    void on_app(Conn& c, const Bytes& msg) {
        Reader r(msg.data() + 1, msg.size() - 1);
        std::string id;
        uint16_t vol = 0;
        bool muted = false;
        uint32_t seq = 0;
        switch (MsgType(msg[0])) {
            case MsgType::GetState:
                send_dump(c);
                return;
            case MsgType::SetEndpointVolume:
                if (decode_set_volume(r, id, vol)) {
                    s.backend_.set_endpoint_volume(id, vol);
                    remote_endpoint(c, id, false, vol, false);
                }
                return;
            case MsgType::SetEndpointMute:
                if (decode_set_mute(r, id, muted)) {
                    request_mute(id, muted);
                    remote_endpoint(c, id, true, 0, muted);
                }
                return;
            case MsgType::SetSourceVolume:
                if (decode_set_volume(r, id, vol)) {
                    s.backend_.set_source_volume(id, vol);
                    remote_source(c, id, false, vol, false);
                }
                return;
            case MsgType::SetSourceMute:
                if (decode_set_mute(r, id, muted)) {
                    s.backend_.set_source_mute(id, muted);
                    remote_source(c, id, true, 0, muted);
                }
                return;
            case MsgType::GetIcon:
                if (decode_id(r, id, kMaxIconKey)) request_icon(c.id, id);
                return;
            case MsgType::Ping:
                if (decode_seq(r, seq)) queue_app(c, encode_seq(MsgType::Pong, seq));
                return;
            case MsgType::SetMeters: {
                bool on = false;
                if (!decode_flag(r, on)) return;
                c.meters = on;
                update_metering();
                return;
            }
            case MsgType::SetSourceRoute: {
                std::string endpoint;
                if (!decode_set_route(r, id, endpoint)) return;
                if (!(s.backend_.features() & kFeatureRouting) || !sources.count(id)) return;
                if (!endpoint.empty()) {
                    auto it = endpoints.find(endpoint);
                    if (it == endpoints.end() || it->second.input) return;  // outputs only
                }
                s.backend_.set_source_route(id, endpoint);
                return;
            }
            case MsgType::PanicMute: {
                bool on = false;
                if (!decode_flag(r, on)) return;
                set_panic(on);
                ServerObserver::RemoteChange ch;
                ch.kind = ServerObserver::RemoteChange::Kind::Panic;
                ch.on = on;
                remote_change(c, ch);
                return;
            }
            case MsgType::StreamStart: {
                StreamStart req;
                if (decode_stream_start(r, req)) start_stream(c, req);
                return;
            }
            case MsgType::StreamStop:
                end_stream(c, ErrorCode::None, true);
                return;
            case MsgType::SaveProfile:
                on_save_profile(r);
                return;
            case MsgType::DeleteProfile:
                if (decode_id(r, id, kMaxId)) on_delete_profile(id);
                return;
            case MsgType::ApplyProfile:
                if (decode_id(r, id, kMaxId))
                    if (const Profile* p = find_profile(id)) {
                        apply_profile(*p, false);  // by hand: nothing to restore later
                        ServerObserver::RemoteChange ch;
                        ch.kind = ServerObserver::RemoteChange::Kind::Profile;
                        ch.target = p->name;
                        remote_change(c, ch);
                    }
                return;
            default:
                return;  // unknown: ignore (forward compatibility)
        }
    }

    void on_frame(Conn& c, const Bytes& payload, Clock::time_point now) {
        if (c.st == Conn::St::Ready) {
            Bytes msg;
            if (!c.channel.open(payload.data(), payload.size(), msg) || msg.empty()) {
                log("bad encrypted frame from " + c.address);
                c.dead = true;
                return;
            }
            on_app(c, msg);
            return;
        }
        Reader r(payload.data() + 1, payload.size() - 1);
        const MsgType t = MsgType(payload[0]);
        if (c.st == Conn::St::AwaitHello && t == MsgType::Hello) return on_hello(c, r, now);
        if (c.st == Conn::St::AwaitPairResponse && t == MsgType::PairResponse) return on_pair_response(c, r, now);
        if (c.st == Conn::St::AwaitResumeConfirm && t == MsgType::ResumeConfirm) return on_resume_confirm(c, r);
        finish_pairing(c, false);
        send_error_and_close(c, ErrorCode::Protocol, "unexpected message");
    }

    // ---- socket plumbing -----------------------------------------------------------

    // Makes room for a new socket by evicting the oldest unauthenticated
    // handshake (never an authenticated controller, never the pairing in progress).
    // Picks the unauthenticated socket to drop: already-failed (closing) ones
    // first, then silent ones (no HELLO yet), from the address holding the most
    // sockets, oldest first. A phone with a single in-progress handshake is
    // therefore never the victim of a flood from other addresses.
    bool evict_oldest_pending(const std::string* same_address) {
        std::map<std::string, size_t> per_addr;
        for (auto& [id, c] : conns)
            if (c->evictable()) ++per_addr[c->address];
        Conn* best = nullptr;
        auto rank = [&](const Conn* c) {
            return std::make_tuple(c->closing ? 0 : 1, c->st == Conn::St::AwaitHello ? 0 : 1,
                                   -int64_t(per_addr[c->address]), c->accepted.time_since_epoch().count());
        };
        for (auto& [id, c] : conns) {
            if (!c->evictable() || c->id == pairing_conn) continue;
            if (same_address && c->address != *same_address) continue;
            if (!best || rank(c.get()) < rank(best)) best = c.get();
        }
        if (!best) return false;
        best->dead = true;
        return true;
    }

    void accept_all(Clock::time_point now) {
        for (;;) {
            std::string peer;
            net::socket_t fd = net::accept_tcp(listener, &peer);
            if (fd == net::kInvalidSocket) return;
            if (s.cfg_.lan_only && !net::is_local_network_address(peer)) {
                if (now - last_refused_log > std::chrono::minutes(1)) {
                    last_refused_log = now;
                    log("refused connection from non-local address " + peer + " (logged at most once a minute)");
                }
                net::close_socket(fd);
                continue;
            }
            size_t pending = 0, from_peer = 0, live = 0;
            for (auto& [id, c] : conns) {
                if (c->dead) continue;
                ++live;
                if (c->evictable()) {
                    ++pending;
                    if (c->address == peer) ++from_peer;
                }
            }
            if (from_peer >= s.cfg_.max_pending_per_address) evict_oldest_pending(&peer);
            else if (pending >= s.cfg_.max_pending_handshakes) evict_oldest_pending(nullptr);
            if (live >= s.cfg_.max_sockets && !evict_oldest_pending(nullptr)) {
                net::close_socket(fd);
                continue;
            }
            auto c = std::make_unique<Conn>();
            c->id = next_conn_id++;
            c->fd = fd;
            c->address = peer;
            c->accepted = now;
            c->deadline = now + milliseconds(std::min(s.cfg_.hello_timeout_ms, s.cfg_.handshake_timeout_ms));
            c->last_rx = now;
            conns.emplace(c->id, std::move(c));
        }
    }

    void read_conn(Conn& c, Clock::time_point now) {
        uint8_t buf[16384];
        for (int round = 0; round < 8 && !c.dead; ++round) {
            long n = net::recv_some(c.fd, buf, sizeof buf);
            if (n == net::kWouldBlock) return;
            if (n <= 0) {
                c.dead = true;
                return;
            }
            // Closing: keep draining (and ignoring) input so closing the socket
            // later doesn't turn into an RST that discards our final error frame.
            if (c.closing) continue;
            c.last_rx = now;
            c.in.feed(buf, size_t(n));
            Bytes payload;
            for (;;) {
                FrameAssembler::Result res = c.in.next(payload);
                if (res == FrameAssembler::Result::NeedMore) break;
                if (res == FrameAssembler::Result::Error) {
                    finish_pairing(c, false);
                    c.dead = true;
                    return;
                }
                on_frame(c, payload, now);
                if (c.dead || c.closing) return;
            }
        }
    }

    void flush_conn(Conn& c) {
        while (c.out_pos < c.out.size()) {
            long n = net::send_some(c.fd, c.out.data() + c.out_pos, c.out.size() - c.out_pos);
            if (n == net::kWouldBlock) break;
            if (n <= 0) {
                c.dead = true;
                return;
            }
            c.out_pos += size_t(n);
        }
        if (c.out_pos == c.out.size()) {
            c.out.clear();
            c.out_pos = 0;
        } else if (c.out_pos > 64 * 1024) {
            // Partial write: drop the sent prefix so the buffer doesn't only grow.
            c.out.erase(c.out.begin(), c.out.begin() + std::ptrdiff_t(c.out_pos));
            c.out_pos = 0;
        }
    }

    void check_timeouts(Clock::time_point now) {
        for (auto& [id, c] : conns) {
            if (c->dead) continue;
            if (c->closing) {
                if (now >= c->deadline) c->dead = true;
                continue;
            }
            if (c->st != Conn::St::Ready) {
                if (now >= c->deadline) {
                    finish_pairing(*c, false);
                    send_error_and_close(*c, ErrorCode::Timeout, "handshake timeout");
                }
            } else if (now - c->last_rx > milliseconds(s.cfg_.idle_timeout_ms)) {
                log("controller timed out: " + c->name);
                c->dead = true;
            }
        }
    }

    void reap() {
        bool ready_changed = false;
        for (auto it = conns.begin(); it != conns.end();) {
            Conn& c = *it->second;
            // A link that starts closing (revoked, replaced, timed out) leaves the
            // connected list now, not when the socket is finally destroyed.
            if (c.listed && (c.closing || c.dead)) {
                c.listed = false;
                ready_changed = true;
            }
            if (!c.dead && c.closing && c.out_pos >= c.out.size() && !c.half_closed) {
                net::shutdown_send(c.fd);  // the error is out; wait for the peer to close
                c.half_closed = true;
            }
            if (!c.dead) {
                ++it;
                continue;
            }
            finish_pairing(c, false);
            end_stream(c, ErrorCode::ConnectionLost, false);  // the phone is gone: stop sending
            net::close_socket(c.fd);
            it = conns.erase(it);
        }
        if (ready_changed) notify_controllers();
        update_metering();
    }

    void run_tasks() {
        std::deque<std::function<void()>> local;
        {
            std::lock_guard<std::mutex> lk(tasks_mu);
            local.swap(tasks);
        }
        for (auto& fn : local) fn();
    }

    int next_timeout_ms(Clock::time_point now) const {
        Clock::time_point next = now + std::chrono::seconds(1);
        for (auto& [id, c] : conns)
            if ((c->st != Conn::St::Ready || c->closing) && c->deadline < next) next = c->deadline;
        auto ms = std::chrono::duration_cast<milliseconds>(next - now).count();
        return int(std::max<long long>(0, std::min<long long>(ms, 1000)));
    }

    void loop() {
        std::vector<net::PollItem> items;
        std::vector<Conn*> order;
        while (s.running_.load()) {
            items.clear();
            order.clear();
            items.push_back({waker.fd(), true, false});
            items.push_back({listener, true, false});
            for (auto& [id, c] : conns) {
                const bool pending_out = c->out_pos < c->out.size();
                items.push_back({c->fd, true, pending_out});
                order.push_back(c.get());
            }
            net::poll(items, next_timeout_ms(Clock::now()));
            const Clock::time_point now = Clock::now();
            if (items[0].readable) waker.drain();
            run_tasks();
            if (items[1].readable) accept_all(now);
            for (size_t i = 0; i < order.size(); ++i) {
                Conn& c = *order[i];
                const net::PollItem& it = items[i + 2];
                if (it.failed) c.dead = true;
                if (!c.dead && it.readable) read_conn(c, now);
            }
            check_timeouts(now);
            expire_pending_mutes(now);
            evaluate_profiles(now);
            for (auto& [id, c] : conns)
                if (!c->dead && c->out_pos < c->out.size()) flush_conn(*c);
            reap();
        }
        // Server::stop() normally ended the streams already; if its wait timed
        // out, end them here so capture stops and "mute the PC" is undone.
        for (auto& [id, c] : conns) end_stream(*c, ErrorCode::None, false);
        for (auto& [id, c] : conns) net::close_socket(c->fd);
        conns.clear();
    }
};

Server::Server(ServerConfig cfg, AudioBackend& backend, TrustStore& store, ServerObserver& observer)
    : cfg_(std::move(cfg)), backend_(backend), store_(store), observer_(observer) {
    // Same normalization the controller applies after decoding (transcript binding).
    cfg_.agent_name = clean_device_name(cfg_.agent_name);
}

Server::~Server() { stop(); }

bool Server::start(std::string* err) {
    if (running_.load()) return true;
    if (!net::init()) {
        if (err) *err = "network init failed";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(life_mu_);
        impl_ = std::make_unique<Impl>(*this);
    }
    if (!impl_->waker.ok()) {
        if (err) *err = "cannot create wake socket";
        impl_.reset();
        return false;
    }
    impl_->load_profiles();
    impl_->listener = net::listen_tcp(cfg_.port, cfg_.loopback_only, &port_, err);
    if (impl_->listener == net::kInvalidSocket) {
        impl_.reset();
        return false;
    }
    running_ = true;
    impl_->icon_thread = std::thread([this] { impl_->icon_loop(); });
    thread_ = std::thread([this] { impl_->loop(); });
    if (!backend_.start(*this, err)) {
        stop();
        return false;
    }
    return true;
}

void Server::stop() {
    if (!impl_) return;
    // End streams first, while the backend can still undo "mute the PC".
    if (running_ && active_streams_ > 0) {
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        post([this, done] {
            for (auto& [id, c] : impl_->conns) impl_->end_stream(*c, ErrorCode::None, false);
            done->set_value();
        });
        fut.wait_for(std::chrono::seconds(1));
    }
    backend_.stop();  // no new OS callbacks after this
    {
        std::lock_guard<std::mutex> lk(impl_->icon_mu);
        impl_->icon_stop = true;
    }
    impl_->icon_cv.notify_all();
    if (impl_->icon_thread.joinable()) impl_->icon_thread.join();
    running_ = false;
    impl_->waker.wake();
    if (thread_.joinable()) thread_.join();
    net::close_socket(impl_->listener);
    std::unique_ptr<Impl> dying;
    {
        std::lock_guard<std::mutex> lk(life_mu_);
        dying = std::move(impl_);  // from here post() sees no Impl and drops the task
    }
    dying.reset();  // queued tasks are destroyed with it (never leaked)
}

void Server::post(std::function<void()> fn) {
    std::lock_guard<std::mutex> life(life_mu_);
    Impl* impl = impl_.get();
    if (!impl || !running_.load()) return;
    {
        std::lock_guard<std::mutex> lk(impl->tasks_mu);
        impl->tasks.push_back(std::move(fn));
    }
    impl->waker.wake();
}

void Server::reject_pairing(uint64_t conn_id) {
    post([this, conn_id] {
        auto it = impl_->conns.find(conn_id);
        if (it == impl_->conns.end() || impl_->pairing_conn != conn_id) return;
        impl_->finish_pairing(*it->second, false);
        impl_->send_error_and_close(*it->second, ErrorCode::PairingRejected, "rejected on the computer");
    });
}

void Server::revoke(const Id16& controller_id) {
    post([this, controller_id] {
        impl_->revoked.insert(controller_id);  // denied even if the store write fails
        if (!store_.remove(controller_id)) impl_->log("could not persist pairing removal; denied for this run");
        for (auto& [id, c] : impl_->conns) {
            if (c->controller_id == controller_id && !c->dead)
                impl_->send_error_and_close(*c, ErrorCode::Revoked, "pairing removed on the computer");
        }
    });
}

void Server::stream_failed(uint64_t stream_id, ErrorCode reason) {
    post([this, stream_id, reason] {
        for (auto& [cid, c] : impl_->conns)
            if (c->streaming && c->stream_id == stream_id) return impl_->end_stream(*c, reason, true);
    });
}

void Server::attach_local(LocalView* view) {
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    {
        std::lock_guard<std::mutex> lk(life_mu_);
        if (!impl_ || !running_.load()) return;
    }
    post([this, view, done] {
        Impl& m = *impl_;
        m.local_view = view;
        if (view) {
            if (!m.local_conn) {
                m.local_conn = std::make_unique<Conn>();
                m.local_conn->local = true;
                m.local_conn->st = Conn::St::Ready;
                m.local_conn->name = "this PC";
            }
            m.send_dump(*m.local_conn);
        } else if (m.local_conn) {
            m.local_conn.reset();
            m.update_metering();
        }
        done->set_value();
    });
    // Detaching waits so no callback reaches a view being destroyed.
    if (!view) fut.wait_for(std::chrono::seconds(2));
}

void Server::local_command(Bytes msg) {
    if (msg.empty()) return;
    post([this, msg = std::move(msg)] {
        if (impl_->local_conn) impl_->on_app(*impl_->local_conn, msg);
    });
}

void Server::network_changed(const std::string& name) {
    post([this, name] {
        if (impl_->network == name) return;
        impl_->network = name;
        if (!impl_->profiles_on()) return;
        impl_->broadcast(encode_context(name));
        impl_->profiles_dirty_eval = true;
    });
}

void Server::panic_mute(bool on) {
    post([this, on] { impl_->set_panic(on); });
}

void Server::panic_toggle() {
    post([this] { impl_->set_panic(!impl_->panic); });
}

void Server::reset_pairing_lockout() {
    post([this] {
        // Lift the current lock but keep the escalation count: if an attacker is
        // still guessing, the next lockout is longer, not back to the minimum.
        // A successful pairing resets the count.
        impl_->locked_until = {};
        impl_->failures.clear();
        observer_.on_pairing_lockout(impl_->lockouts, 0);
        impl_->log("pairing lockout cleared on the PC");
    });
}

void Server::endpoint_upsert(const EndpointInfo& e) {
    post([this, e = EndpointInfo(e)]() mutable {
        e.meter = impl_->slot_for(e.id);
        auto it = impl_->endpoints.find(e.id);
        const bool is_new = it == impl_->endpoints.end();
        // A report older than a mute we asked for must not undo it in our model.
        auto pm = impl_->pending_mute.find(e.id);
        if (pm != impl_->pending_mute.end()) {
            if (e.muted != pm->second.muted && Clock::now() < pm->second.until) {
                pm->second.reported = e.muted;  // applied when the request expires unconfirmed
                e.muted = pm->second.muted;
            } else {
                impl_->pending_mute.erase(pm);
            }
        }
        if (!is_new && it->second == e) return;
        // Unmuted by hand during a panic: forget it (PANIC_MUTE 0 must not touch it again).
        if (!e.muted && impl_->panic_muted.count(e.id)) impl_->panic_muted.erase(e.id);
        // Same for a stream's "mute the PC": the user took it back.
        if (!e.muted) impl_->stream_mutes.erase(e.id);
        impl_->endpoints[e.id] = e;
        impl_->broadcast(encode_endpoint(e));
        // A device plugged in during a panic is muted too.
        if (is_new && impl_->panic && !e.muted) {
            impl_->panic_muted.insert(e.id);
            impl_->request_mute(e.id, true);
        }
        if (is_new && !e.input) impl_->profiles_dirty_eval = true;  // headphones plugged in?
    });
}

void Server::endpoint_removed(const std::string& id) {
    post([this, id] {
        impl_->pending_mute.erase(id);
        impl_->stream_mutes.erase(id);
        for (auto& [cid, c] : impl_->conns)
            if (c->streaming && c->stream_endpoint == id) impl_->end_stream(*c, ErrorCode::ConnectionLost, true);
        impl_->free_slot(id);
        if (impl_->endpoints.erase(id) == 0) return;
        impl_->broadcast(encode_id(MsgType::EndpointRemoved, id));
        impl_->profiles_dirty_eval = true;
    });
}

void Server::source_upsert(const SourceInfo& src) {
    post([this, src = SourceInfo(src)]() mutable {
        src.meter = impl_->slot_for(src.id);
        auto it = impl_->sources.find(src.id);
        if (it != impl_->sources.end() && it->second == src) return;
        const bool is_new = it == impl_->sources.end();
        impl_->sources[src.id] = src;
        impl_->broadcast(encode_source(src));
        if (is_new) impl_->profile_new_source(src);
    });
}

void Server::source_removed(const std::string& id) {
    post([this, id] {
        impl_->free_slot(id);
        if (impl_->sources.erase(id) == 0) return;
        impl_->broadcast(encode_id(MsgType::SourceRemoved, id));
    });
}

void Server::meters(const std::vector<std::pair<std::string, uint8_t>>& levels) {
    post([this, levels] {
        if (!impl_->metering) return;
        std::vector<MeterLevel> out;
        out.reserve(levels.size());
        for (auto& [id, level] : levels) {
            auto it = impl_->slots.find(id);
            if (it != impl_->slots.end()) out.push_back(MeterLevel{it->second, level});
        }
        for (size_t start = 0; start < out.size(); start += kMaxMetersPerMessage) {
            std::vector<MeterLevel> chunk(out.begin() + std::ptrdiff_t(start),
                                          out.begin() + std::ptrdiff_t(std::min(out.size(), start + kMaxMetersPerMessage)));
            const Bytes msg = encode_meters(chunk);
            for (auto& [cid, c] : impl_->conns) {
                // Meters are disposable: skip a frame for a slow reader instead of queueing.
                if (!c->meters || c->out.size() - c->out_pos > 64 * 1024) continue;
                impl_->queue_app(*c, msg);
            }
            if (impl_->local_conn && impl_->local_conn->meters) impl_->queue_app(*impl_->local_conn, msg);
        }
    });
}

}  // namespace arc
