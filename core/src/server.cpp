#include "archon/server.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <set>
#include <tuple>

#include "archon/crypto.hpp"
#include "archon/frame.hpp"
#include "archon/net.hpp"

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

    void queue_app(Conn& c, const Bytes& msg) {
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
    }

    void send_dump(Conn& c) {
        queue_app(c, encode_simple(MsgType::StateReset));
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
                if (decode_set_volume(r, id, vol)) s.backend_.set_endpoint_volume(id, vol);
                return;
            case MsgType::SetEndpointMute:
                if (decode_set_mute(r, id, muted)) s.backend_.set_endpoint_mute(id, muted);
                return;
            case MsgType::SetSourceVolume:
                if (decode_set_volume(r, id, vol)) s.backend_.set_source_volume(id, vol);
                return;
            case MsgType::SetSourceMute:
                if (decode_set_mute(r, id, muted)) s.backend_.set_source_mute(id, muted);
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
            for (auto& [id, c] : conns)
                if (!c->dead && c->out_pos < c->out.size()) flush_conn(*c);
            reap();
        }
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
        if (it != impl_->endpoints.end() && it->second == e) return;
        impl_->endpoints[e.id] = e;
        impl_->broadcast(encode_endpoint(e));
    });
}

void Server::endpoint_removed(const std::string& id) {
    post([this, id] {
        impl_->free_slot(id);
        if (impl_->endpoints.erase(id) == 0) return;
        impl_->broadcast(encode_id(MsgType::EndpointRemoved, id));
    });
}

void Server::source_upsert(const SourceInfo& src) {
    post([this, src = SourceInfo(src)]() mutable {
        src.meter = impl_->slot_for(src.id);
        auto it = impl_->sources.find(src.id);
        if (it != impl_->sources.end() && it->second == src) return;
        impl_->sources[src.id] = src;
        impl_->broadcast(encode_source(src));
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
        }
    });
}

}  // namespace arc
