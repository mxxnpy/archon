#include "archon/client.hpp"

#include <algorithm>
#include <chrono>

#include "archon/frame.hpp"

namespace arc {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

const char* client_status_name(ClientStatus s) {
    switch (s) {
        case ClientStatus::Idle: return "idle";
        case ClientStatus::Connecting: return "connecting";
        case ClientStatus::AwaitingCode: return "awaiting_code";
        case ClientStatus::Verifying: return "verifying";
        case ClientStatus::Connected: return "connected";
        case ClientStatus::Reconnecting: return "reconnecting";
        case ClientStatus::Failed: return "failed";
    }
    return "unknown";
}

// Errors after which retrying cannot succeed without user action.
static bool is_fatal(ErrorCode e) {
    switch (e) {
        case ErrorCode::NotRecognized:  // ask the user; never auto-delete
        case ErrorCode::UnknownController:
        case ErrorCode::AuthFailed:
        case ErrorCode::Revoked:
        case ErrorCode::WrongCode:
        case ErrorCode::PairingLocked:
        case ErrorCode::PairingRejected:
        case ErrorCode::UnsupportedVersion:
        case ErrorCode::Protocol:
            return true;
        default:
            return false;
    }
}

Client::Client(const Id16& controller_id, std::string controller_name, ClientListener& listener,
               ClientTiming timing)
    : controller_id_(controller_id),
      controller_name_(clean_device_name(controller_name)),
      listener_(listener),
      timing_(timing) {}

Client::~Client() { disconnect(); }

void Client::status(ClientStatus s, ErrorCode e, const std::string& m) { listener_.on_status(s, e, m); }

void Client::pair(const std::string& host, uint16_t port) { start({{host, port}}, Mode::Pair, nullptr); }

void Client::resume(const std::string& host, uint16_t port, const PairingRecord& record) {
    start({{host, port}}, Mode::Resume, &record);
}

void Client::resume(const std::vector<std::pair<std::string, uint16_t>>& addresses, const PairingRecord& record) {
    start(addresses, Mode::Resume, &record);
}

void Client::start(std::vector<std::pair<std::string, uint16_t>> addrs, Mode mode, const PairingRecord* record) {
    disconnect();
    if (addrs.empty()) return;
    addrs_ = std::move(addrs);
    attempt_ = 0;
    refused_.assign(addrs_.size(), ErrorCode::None);
    mode_ = mode;
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_panic_.reset();
    }
    if (record) record_ = *record;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cmds_.clear();
        pending_code_.clear();
    }
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void Client::disconnect() {
    if (!thread_.joinable()) return;
    stop_ = true;
    waker_.wake();
    thread_.join();
    wipe(record_.pair_key.data(), record_.pair_key.size());
}

void Client::submit_code(const std::string& code) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_code_ = code;
    }
    waker_.wake();
}

void Client::enqueue(MsgType type, const std::string& id, Bytes msg, bool coalesce) {
    // Offline commands are dropped, never replayed later: on reconnect the
    // PC's state dump is the truth (it may have changed meanwhile). The meter
    // subscription is a flag re-sent on every Ready, so it needs no queue.
    if (!live_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        bool replaced = false;
        if (coalesce) {
            for (Cmd& c : cmds_) {
                if (c.type == type && c.id == id) {
                    c.msg = std::move(msg);
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) {
            if (cmds_.size() >= 256) cmds_.pop_front();  // bounded while offline
            cmds_.push_back(Cmd{type, id, std::move(msg)});
        }
    }
    waker_.wake();
}

void Client::set_endpoint_volume(const std::string& id, uint16_t v) {
    enqueue(MsgType::SetEndpointVolume, id, encode_set_volume(MsgType::SetEndpointVolume, id, v), true);
}
void Client::set_endpoint_mute(const std::string& id, bool m) {
    enqueue(MsgType::SetEndpointMute, id, encode_set_mute(MsgType::SetEndpointMute, id, m), true);
}
void Client::set_source_volume(const std::string& id, uint16_t v) {
    enqueue(MsgType::SetSourceVolume, id, encode_set_volume(MsgType::SetSourceVolume, id, v), true);
}
void Client::set_source_mute(const std::string& id, bool m) {
    enqueue(MsgType::SetSourceMute, id, encode_set_mute(MsgType::SetSourceMute, id, m), true);
}
void Client::set_source_route(const std::string& id, const std::string& endpoint) {
    enqueue(MsgType::SetSourceRoute, id, encode_set_route(id, endpoint), true);
}
void Client::panic_mute(bool on) {
    bool live_now;
    {
        // Decided under mu_: the worker flips live_ and drains the pending
        // request under the same lock, so a request can't slip between them.
        std::lock_guard<std::mutex> lk(mu_);
        live_now = live_;
        if (!live_now) {
            pending_panic_ = on;  // sent on the next Ready if still fresh, then forgotten
            pending_panic_at_ = std::chrono::steady_clock::now();
        }
    }
    if (live_now) enqueue(MsgType::PanicMute, {}, encode_flag(MsgType::PanicMute, on), true);
    else waker_.wake();
}
bool Client::stream_start(uint8_t flags, const std::string& endpoint_id) {
    if (!live_) return false;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lk(receiver_mu_);
        if (!receiver_) {
            relay_.target = &listener_;
            receiver_ = std::make_unique<StreamReceiver>(relay_);
        }
        std::string err;
        port = receiver_->open(&err);
    }
    if (!port) return false;
    StreamStart req;
    req.udp_port = port;
    req.flags = flags;
    req.endpoint_id = endpoint_id;
    enqueue(MsgType::StreamStart, {}, encode_stream_start(req), true);
    return true;
}

void Client::stream_stop() {
    {
        std::lock_guard<std::mutex> lk(receiver_mu_);
        if (receiver_) receiver_->stop();
    }
    enqueue(MsgType::StreamStop, {}, encode_simple(MsgType::StreamStop), true);
}

void Client::request_icon(const std::string& key) {
    enqueue(MsgType::GetIcon, key, encode_id(MsgType::GetIcon, key), true);
}
void Client::set_meters(bool on) {
    meters_ = on;
    enqueue(MsgType::SetMeters, {}, encode_flag(MsgType::SetMeters, on), true);
}

void Client::refresh() { enqueue(MsgType::GetState, {}, encode_simple(MsgType::GetState), true); }

bool Client::wait_interruptible(int ms) {
    const auto until = Clock::now() + milliseconds(ms);
    while (!stop_) {
        const auto left = std::chrono::duration_cast<milliseconds>(until - Clock::now()).count();
        if (left <= 0) return true;
        std::vector<net::PollItem> items{{waker_.fd(), true, false}};
        net::poll(items, int(left));
        if (items[0].readable) waker_.drain();
    }
    return false;
}

void Client::run() {
    int backoff = timing_.backoff_min_ms;
    bool first = true;
    while (!stop_) {
        status(first ? ClientStatus::Connecting : ClientStatus::Reconnecting);
        first = false;
        bool ever_ready = false;
        const bool retry = run_connection(ever_ready);
        if (stop_ || !retry) break;
        if (ever_ready) backoff = timing_.backoff_min_ms;
        if (!wait_interruptible(backoff)) break;
        backoff = std::min(backoff * 2, timing_.backoff_max_ms);
    }
    if (stop_) status(ClientStatus::Idle);
}

bool Client::run_connection(bool& ever_ready) {
    std::string err;
    // Plain variables (not structured bindings): the lambdas below capture them.
    const std::string host = addrs_[attempt_ % addrs_.size()].first;
    const uint16_t port = addrs_[attempt_ % addrs_.size()].second;
    ++attempt_;
    net::socket_t fd = net::connect_tcp(host, port, timing_.connect_timeout_ms, &err);
    if (fd == net::kInvalidSocket) {
        if (mode_ == Mode::Pair) {
            status(ClientStatus::Failed, ErrorCode::ConnectFailed, err);
            return false;
        }
        status(ClientStatus::Reconnecting, ErrorCode::ConnectFailed, err);
        return true;
    }

    ControllerSession session(controller_id_, controller_name_);
    Bytes out;
    size_t out_pos = 0;
    append_frame(out, mode_ == Mode::Pair ? session.begin_pair() : session.begin_resume(record_));

    FrameAssembler in;
    auto deadline = Clock::now() + milliseconds(timing_.handshake_timeout_ms);
    auto last_rx = Clock::now();
    auto next_ping = Clock::now();
    auto next_batch = Clock::now();
    uint32_t ping_seq = 0;
    bool retry = mode_ == Mode::Resume;
    ErrorCode reason = ErrorCode::ConnectionLost;
    std::string reason_text;
    bool done = false;

    auto handle_output = [&](ControllerSession::Output& o) {
        for (Bytes& b : o.send) append_frame(out, b);
        for (ControllerEvent& ev : o.events) {
            switch (ev.kind) {
                case ControllerEvent::Kind::CodeRequired:
                    deadline = Clock::now() + milliseconds(timing_.code_timeout_ms);
                    status(ClientStatus::AwaitingCode);
                    break;
                case ControllerEvent::Kind::Paired:
                    // From now on reconnections use the new pairing.
                    record_ = ev.pairing;
                    mode_ = Mode::Resume;
                    retry = true;
                    break;
                case ControllerEvent::Kind::Ready:
                    ever_ready = true;
                    last_rx = Clock::now();
                    attempt_ = (attempt_ + addrs_.size() - 1) % addrs_.size();  // stick to what worked
                    refused_.assign(addrs_.size(), ErrorCode::None);
                    // Re-subscribe on every (re)connection: the agent forgets per link.
                    if (meters_) append_frame(out, session.app(encode_flag(MsgType::SetMeters, true)));
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        live_ = true;
                        // A panic asked for before the link was up goes now, but only
                        // if recent: an old request must not undo a newer one made
                        // elsewhere (hotkey, another phone) while we were away.
                        if (pending_panic_ && Clock::now() - pending_panic_at_ < std::chrono::seconds(10))
                            append_frame(out, session.app(encode_flag(MsgType::PanicMute, *pending_panic_)));
                        pending_panic_.reset();
                    }
                    status(ClientStatus::Connected, ErrorCode::None, host + "|" + std::to_string(port));
                    break;
                case ControllerEvent::Kind::Error:
                    reason = ev.error;
                    reason_text = ev.message;
                    break;
                case ControllerEvent::Kind::StreamState: {
                    // The key arrived inside the encrypted channel: accept audio
                    // only from this PC's address, sealed with it.
                    std::unique_ptr<StreamReceiver> ended;
                    {
                        std::lock_guard<std::mutex> lk(receiver_mu_);
                        if (receiver_) {
                            if (ev.stream.active) receiver_->start(ev.stream.key, host);
                            else ended = std::move(receiver_);  // stream over: release the UDP port
                        }
                    }
                    ended.reset();  // joins its reader outside receiver_mu_ (its callback may call stream_stop)
                    wipe(ev.stream.key.data(), ev.stream.key.size());  // the app never sees the key
                    break;
                }
                default:
                    break;
            }
            listener_.on_event(ev);
        }
        if (o.close) {
            if (o.close_reason != ErrorCode::None) reason = o.close_reason;
            done = true;
        }
    };

    while (!stop_ && !done) {
        const auto now = Clock::now();
        const bool ready = session.state() == ControllerSession::State::Ready;

        // Code typed by the user.
        std::string code;
        std::deque<Cmd> cmds;
        {
            std::lock_guard<std::mutex> lk(mu_);
            code.swap(pending_code_);
            // Take commands only when the previous ones left the socket, so a
            // slow reader lets coalescing collapse slider drags (latest wins).
            if (ready && out_pos >= out.size() && now >= next_batch && !cmds_.empty()) {
                cmds.swap(cmds_);
                next_batch = now + milliseconds(timing_.command_interval_ms);
            }
        }
        if (!code.empty() && session.state() == ControllerSession::State::AwaitCode) {
            Bytes resp = session.submit_code(code);
            wipe(code.data(), code.size());
            if (!resp.empty()) {
                append_frame(out, resp);
                deadline = now + milliseconds(timing_.handshake_timeout_ms);
                status(ClientStatus::Verifying);
            } else {
                status(ClientStatus::AwaitingCode, ErrorCode::WrongCode, "the code has 6 digits");
            }
        }
        for (Cmd& c : cmds) append_frame(out, session.app(c.msg));

        if (ready && now >= next_ping) {
            append_frame(out, session.app(encode_seq(MsgType::Ping, ++ping_seq)));
            next_ping = now + milliseconds(timing_.ping_interval_ms);
        }
        if (!ready && now >= deadline) {
            reason = ErrorCode::Timeout;
            break;
        }
        if (ready && now - last_rx > milliseconds(timing_.idle_timeout_ms)) {
            reason = ErrorCode::ConnectionLost;
            reason_text = "no answer from the computer";
            break;
        }

        // Flush.
        while (out_pos < out.size()) {
            long n = net::send_some(fd, out.data() + out_pos, out.size() - out_pos);
            if (n == net::kWouldBlock) break;
            if (n <= 0) {
                done = true;
                break;
            }
            out_pos += size_t(n);
        }
        if (out_pos == out.size()) {
            out.clear();
            out_pos = 0;
        }
        if (done) break;

        std::vector<net::PollItem> items{{waker_.fd(), true, false}, {fd, true, out_pos < out.size()}};
        int wait_ms = 500;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!cmds_.empty()) {
                const auto left = std::chrono::duration_cast<milliseconds>(next_batch - Clock::now()).count();
                wait_ms = int(std::max<long long>(1, std::min<long long>(left, 500)));
            }
        }
        net::poll(items, wait_ms);
        if (items[0].readable) waker_.drain();
        if (items[1].failed) break;
        if (!items[1].readable) continue;

        uint8_t buf[16384];
        long n = net::recv_some(fd, buf, sizeof buf);
        if (n == net::kWouldBlock) continue;
        if (n <= 0) break;
        last_rx = Clock::now();
        in.feed(buf, size_t(n));
        Bytes payload;
        for (;;) {
            auto res = in.next(payload);
            if (res == FrameAssembler::Result::NeedMore) break;
            if (res == FrameAssembler::Result::Error) {
                // Framing bytes are not authenticated: once the channel is up, a
                // broken length prefix is an injected or damaged stream, not a
                // verdict from the PC. Reconnect instead of giving up.
                reason = session.state() == ControllerSession::State::Ready ? ErrorCode::ConnectionLost
                                                                            : ErrorCode::Protocol;
                done = true;
                break;
            }
            ControllerSession::Output o;
            session.on_frame(payload.data(), payload.size(), o);
            handle_output(o);
            if (done) break;
        }
        // Flush anything produced by the frames (e.g. RESUME_CONFIRM) before
        // a possible close.
        while (out_pos < out.size()) {
            long w = net::send_some(fd, out.data() + out_pos, out.size() - out_pos);
            if (w <= 0) break;
            out_pos += size_t(w);
        }
    }
    live_ = false;
    net::close_socket(fd);
    {
        // The agent stops streaming when the link closes; a new stream must be
        // asked for (with a new port). Released outside the lock, as above.
        std::unique_ptr<StreamReceiver> ended;
        {
            std::lock_guard<std::mutex> lk(receiver_mu_);
            ended = std::move(receiver_);
        }
    }
    if (stop_) return false;
    {
        // Commands made while the link was down are stale: the state dump sent on
        // reconnect is the truth (the PC may have changed meanwhile). Keep only
        // the meter subscription, which is re-sent on Ready anyway.
        std::lock_guard<std::mutex> lk(mu_);
        cmds_.clear();
    }

    // Before the secure channel is up nothing the peer says is authenticated:
    // any refusal (not recognized, busy, version, malformed reply…) may come from
    // a spoofer advertised first. Only give up once every known address refused.
    if (mode_ == Mode::Resume && !ever_ready && is_fatal(reason)) {
        refused_[(attempt_ + addrs_.size() - 1) % addrs_.size()] = reason;
        bool all = true, any_not_recognized = false;
        for (ErrorCode e : refused_) {
            all = all && e != ErrorCode::None;
            any_not_recognized = any_not_recognized || e == ErrorCode::NotRecognized;
        }
        if (!all) {  // some address hasn't refused us (maybe just offline): keep trying
            status(ClientStatus::Reconnecting, reason, reason_text);
            return true;
        }
        // A spoofer can pick any refusal ("update the app"…); the real PC's
        // "not recognized" is the one the user can act on (pair again).
        if (any_not_recognized && reason != ErrorCode::NotRecognized) {
            reason = ErrorCode::NotRecognized;
            reason_text = "the computer did not recognize this phone";
        }
    }
    if (is_fatal(reason)) {
        status(ClientStatus::Failed, reason, reason_text);
        return false;
    }
    if (!retry) {
        status(ClientStatus::Failed, reason, reason_text);
        return false;
    }
    status(ClientStatus::Reconnecting, reason, reason_text);
    return true;
}

}  // namespace arc
