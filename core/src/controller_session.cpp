#include "archon/controller_session.hpp"

namespace arc {

ControllerSession::ControllerSession(const Id16& controller_id, std::string controller_name)
    : controller_id_(controller_id), controller_name_(clean_device_name(controller_name)) {}

Bytes ControllerSession::begin_pair() {
    mode_ = Mode::Pair;
    state_ = State::AwaitHelloAck;
    Hello h;
    h.mode = Mode::Pair;
    h.controller_id = controller_id_;
    h.name = controller_name_;
    return encode_hello(h);
}

Bytes ControllerSession::begin_resume(const PairingRecord& record) {
    mode_ = Mode::Resume;
    state_ = State::AwaitHelloAck;
    expected_agent_ = record.agent_id;
    pair_key_ = record.pair_key;
    agent_name_ = record.agent_name;
    nonce_c_ = random_array<32>();
    Hello h;
    h.mode = Mode::Resume;
    h.controller_id = controller_id_;
    h.name = controller_name_;
    h.nonce = nonce_c_;
    return encode_hello(h);
}

void ControllerSession::fail(Output& out, ErrorCode reason) {
    state_ = State::Closed;
    pending_.reset();
    wipe(pair_key_.data(), pair_key_.size());
    out.close = true;
    if (out.close_reason == ErrorCode::None) out.close_reason = reason;
}

Bytes ControllerSession::submit_code(const std::string& code) {
    if (state_ != State::AwaitCode || !is_valid_pairing_code(code)) return {};
    const Key32 g = cpace_generator(code, sid_, agent_id_, controller_id_);
    CpaceParty me;
    cpace_start(g, me);
    Key32 k;
    if (!cpace_finish(me, ys_, k)) {
        state_ = State::Closed;
        return {};
    }
    pending_ = std::make_unique<SessionKeys>(
        derive_pair_keys(k, sid_, agent_id_, controller_id_, ys_, me.y, controller_name_, agent_name_));
    wipe(k.data(), k.size());
    state_ = State::AwaitPairConfirm;
    return encode_pair_response(me.y, pending_->tag_controller);
}

void ControllerSession::on_frame(const uint8_t* p, size_t n, Output& out) {
    if (state_ == State::Closed || state_ == State::Idle) return;
    if (state_ != State::Ready) {
        handle_plain(p, n, out);
        return;
    }
    Bytes msg;
    if (!channel_.open(p, n, msg) || msg.empty()) {
        // A forged/corrupted segment on the wire: drop the link and reconnect.
        // It says nothing about the pairing, so it must not look like a refusal.
        fail(out, ErrorCode::ConnectionLost);
        return;
    }
    handle_app(msg, out);
}

void ControllerSession::handle_plain(const uint8_t* p, size_t n, Output& out) {
    if (n == 0) return fail(out, ErrorCode::Protocol);
    Reader r(p + 1, n - 1);
    const MsgType t = MsgType(p[0]);

    if (t == MsgType::Error) {
        ControllerEvent ev{ControllerEvent::Kind::Error};
        ErrorMsg e;
        if (!decode_error(r, e)) return fail(out, ErrorCode::Protocol);
        // Plaintext = unauthenticated: anyone on the LAN can send it. Errors that
        // would invalidate a pairing are downgraded so apps never auto-delete keys.
        if (e.code == ErrorCode::UnknownController || e.code == ErrorCode::AuthFailed || e.code == ErrorCode::Revoked)
            e.code = ErrorCode::NotRecognized;
        ev.error = e.code;
        ev.message = e.message;
        out.events.push_back(std::move(ev));
        return fail(out, e.code);
    }

    switch (state_) {
        case State::AwaitHelloAck: {
            if (t != MsgType::HelloAck) return fail(out, ErrorCode::Protocol);
            HelloAck a;
            if (!decode_hello_ack(r, a) || a.mode != mode_) return fail(out, ErrorCode::Protocol);
            if (a.version != kProtocolVersion) return fail(out, ErrorCode::UnsupportedVersion);
            agent_id_ = a.agent_id;
            agent_name_ = a.name;
            if (mode_ == Mode::Pair) {
                sid_ = a.sid;
                ys_ = a.y;
                state_ = State::AwaitCode;
                out.events.push_back(ControllerEvent{ControllerEvent::Kind::CodeRequired});
                return;
            }
            // Not the paired PC (stale address, other PC, or a spoofer): try elsewhere.
            if (agent_id_ != expected_agent_) return fail(out, ErrorCode::WrongAgent);
            SessionKeys keys = derive_resume_keys(pair_key_, controller_id_, agent_id_, nonce_c_, a.nonce,
                                                  controller_name_, agent_name_);
            if (!ct_equal(keys.tag_agent, a.tag)) return fail(out, ErrorCode::WrongAgent);
            out.send.push_back(encode_resume_confirm(keys.tag_controller));
            channel_.init(keys.key_c2a, keys.key_a2c);
            state_ = State::Ready;
            out.events.push_back(ControllerEvent{ControllerEvent::Kind::Ready});
            return;
        }
        case State::AwaitPairConfirm: {
            if (t != MsgType::PairConfirm || !pending_) return fail(out, ErrorCode::Protocol);
            Key32 tag;
            if (!decode_key(r, tag)) return fail(out, ErrorCode::Protocol);
            if (!ct_equal(tag, pending_->tag_agent)) return fail(out, ErrorCode::AuthFailed);
            channel_.init(pending_->key_c2a, pending_->key_a2c);
            ControllerEvent paired{ControllerEvent::Kind::Paired};
            paired.pairing.agent_id = agent_id_;
            paired.pairing.agent_name = agent_name_;
            paired.pairing.pair_key = pending_->pair_key;
            pending_.reset();
            state_ = State::Ready;
            out.events.push_back(std::move(paired));
            out.events.push_back(ControllerEvent{ControllerEvent::Kind::Ready});
            return;
        }
        default:
            // AwaitCode: the agent must not speak until we answer.
            return fail(out, ErrorCode::Protocol);
    }
}

void ControllerSession::handle_app(const Bytes& msg, Output& out) {
    Reader r(msg.data() + 1, msg.size() - 1);
    using K = ControllerEvent::Kind;
    switch (MsgType(msg[0])) {
        case MsgType::StateReset:
            out.events.push_back(ControllerEvent{K::StateReset});
            return;
        case MsgType::StateSynced:
            out.events.push_back(ControllerEvent{K::StateSynced});
            return;
        case MsgType::Endpoint: {
            ControllerEvent ev{K::Endpoint};
            if (!decode_endpoint(r, ev.endpoint)) return fail(out, ErrorCode::Protocol);
            out.events.push_back(std::move(ev));
            return;
        }
        case MsgType::Source: {
            ControllerEvent ev{K::Source};
            if (!decode_source(r, ev.source)) return fail(out, ErrorCode::Protocol);
            out.events.push_back(std::move(ev));
            return;
        }
        case MsgType::EndpointRemoved:
        case MsgType::SourceRemoved: {
            ControllerEvent ev{MsgType(msg[0]) == MsgType::EndpointRemoved ? K::EndpointRemoved
                                                                           : K::SourceRemoved};
            if (!decode_id(r, ev.id, kMaxId)) return fail(out, ErrorCode::Protocol);
            out.events.push_back(std::move(ev));
            return;
        }
        case MsgType::Icon: {
            ControllerEvent ev{K::Icon};
            if (!decode_icon(r, ev.id, ev.data)) return fail(out, ErrorCode::Protocol);
            out.events.push_back(std::move(ev));
            return;
        }
        case MsgType::AgentInfo: {
            ControllerEvent ev{K::AgentInfo};
            if (!decode_agent_info(r, ev.agent)) return fail(out, ErrorCode::Protocol);
            out.events.push_back(std::move(ev));
            return;
        }
        case MsgType::Meters: {
            ControllerEvent ev{K::Meters};
            if (!decode_meters(r, ev.meters)) return fail(out, ErrorCode::Protocol);
            out.events.push_back(std::move(ev));
            return;
        }
        case MsgType::Pong: {
            ControllerEvent ev{K::Pong};
            if (!decode_seq(r, ev.seq)) return fail(out, ErrorCode::Protocol);
            out.events.push_back(std::move(ev));
            return;
        }
        case MsgType::Error: {
            ControllerEvent ev{K::Error};
            ErrorMsg e;
            if (!decode_error(r, e)) return fail(out, ErrorCode::Protocol);
            ev.error = e.code;
            ev.message = e.message;
            out.events.push_back(std::move(ev));
            return fail(out, e.code);
        }
        default:
            return;  // unknown inside the channel: ignore (protocol §2)
    }
}

Bytes ControllerSession::app(const Bytes& plain_message) {
    if (state_ != State::Ready) return {};
    return channel_.seal(plain_message);
}

}  // namespace arc
