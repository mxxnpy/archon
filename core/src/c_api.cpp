#include "archon/archon.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "archon/client.hpp"

namespace {

struct Bridge final : arc::ClientListener {
    arc_event_cb cb;
    void* user;

    Bridge(arc_event_cb c, void* u) : cb(c), user(u) {}

    static arc_event blank(arc_event_type t) {
        arc_event e;
        std::memset(&e, 0, sizeof e);
        e.type = t;
        e.message = e.id = e.endpoint_id = e.name = e.icon_key = e.route = "";
        return e;
    }

    void on_status(arc::ClientStatus s, arc::ErrorCode err, const std::string& msg) override {
        arc_event e = blank(ARC_EV_STATUS);
        e.status = arc_status(int(s));
        e.error = arc_error(int(err));
        e.message = msg.c_str();
        cb(user, &e);
    }

    void on_event(const arc::ControllerEvent& ev) override {
        using K = arc::ControllerEvent::Kind;
        switch (ev.kind) {
            case K::Paired: {
                const arc::Bytes rec = arc::serialize_pairing(ev.pairing);
                arc_event e = blank(ARC_EV_PAIRED);
                e.name = ev.pairing.agent_name.c_str();
                e.data = rec.data();
                e.data_len = rec.size();
                cb(user, &e);
                return;
            }
            case K::StateReset: {
                arc_event e = blank(ARC_EV_STATE_RESET);
                cb(user, &e);
                return;
            }
            case K::StateSynced: {
                arc_event e = blank(ARC_EV_STATE_SYNCED);
                cb(user, &e);
                return;
            }
            case K::Endpoint: {
                arc_event e = blank(ARC_EV_ENDPOINT);
                e.id = ev.endpoint.id.c_str();
                e.name = ev.endpoint.name.c_str();
                e.volume = ev.endpoint.volume;
                e.muted = ev.endpoint.muted;
                e.is_default = ev.endpoint.is_default;
                e.meter = ev.endpoint.meter;
                e.input = ev.endpoint.input ? 1 : 0;
                e.is_default_input = ev.endpoint.is_default_input ? 1 : 0;
                cb(user, &e);
                return;
            }
            case K::Source: {
                arc_event e = blank(ARC_EV_SOURCE);
                e.id = ev.source.id.c_str();
                e.endpoint_id = ev.source.endpoint_id.c_str();
                e.name = ev.source.name.c_str();
                e.icon_key = ev.source.icon_key.c_str();
                e.pid = ev.source.pid;
                e.volume = ev.source.volume;
                e.muted = ev.source.muted;
                e.playing = ev.source.playing;
                e.system_sounds = ev.source.system_sounds;
                e.meter = ev.source.meter;
                e.route = ev.source.route.c_str();
                cb(user, &e);
                return;
            }
            case K::EndpointRemoved:
            case K::SourceRemoved: {
                arc_event e = blank(ev.kind == K::EndpointRemoved ? ARC_EV_ENDPOINT_REMOVED : ARC_EV_SOURCE_REMOVED);
                e.id = ev.id.c_str();
                cb(user, &e);
                return;
            }
            case K::Icon: {
                arc_event e = blank(ARC_EV_ICON);
                e.icon_key = ev.id.c_str();
                e.data = ev.data.data();
                e.data_len = ev.data.size();
                cb(user, &e);
                return;
            }
            case K::AgentInfo: {
                arc_event e = blank(ARC_EV_AGENT_INFO);
                e.features = ev.agent.features;
                e.panic = ev.agent.panic ? 1 : 0;
                e.message = ev.agent.os.c_str();
                cb(user, &e);
                return;
            }
            case K::Meters: {
                std::vector<uint8_t> packed;
                packed.reserve(ev.meters.size() * 3);
                for (const auto& m : ev.meters) {
                    packed.push_back(uint8_t(m.slot >> 8));
                    packed.push_back(uint8_t(m.slot));
                    packed.push_back(m.level);
                }
                arc_event e = blank(ARC_EV_METERS);
                e.data = packed.data();
                e.data_len = packed.size();
                cb(user, &e);
                return;
            }
            default:
                return;  // CodeRequired/Ready/Error are reported through status
        }
    }
};

}  // namespace

struct arc_client {
    std::unique_ptr<Bridge> bridge;
    std::unique_ptr<arc::Client> client;
};

static std::string s(const char* p) { return p ? std::string(p) : std::string(); }

extern "C" {

void arc_generate_controller_id(uint8_t out_id[16]) { arc::random_bytes(out_id, 16); }

arc_client* arc_client_create(const uint8_t controller_id[16], const char* name, arc_event_cb cb, void* user) {
    if (!controller_id || !cb) return nullptr;
    arc::Id16 id;
    std::memcpy(id.data(), controller_id, 16);
    auto* c = new arc_client;
    c->bridge = std::make_unique<Bridge>(cb, user);
    c->client = std::make_unique<arc::Client>(id, arc::sanitize_utf8(s(name)), *c->bridge);
    return c;
}

void arc_client_destroy(arc_client* c) {
    if (!c) return;
    c->client.reset();  // joins the worker thread before the bridge goes away
    delete c;
}

void arc_client_pair(arc_client* c, const char* host, uint16_t port) {
    if (c) c->client->pair(s(host), port ? port : ARC_DEFAULT_PORT);
}

int arc_client_resume(arc_client* c, const char* host, uint16_t port, const uint8_t* record, size_t len) {
    if (!c || !record) return 0;
    auto rec = arc::deserialize_pairing(record, len);
    if (!rec) return 0;
    c->client->resume(s(host), port ? port : ARC_DEFAULT_PORT, *rec);
    arc::wipe(rec->pair_key.data(), rec->pair_key.size());
    return 1;
}

int arc_client_resume_multi(arc_client* c, const char* const* hosts, const uint16_t* ports, size_t count,
                            const uint8_t* record, size_t len) {
    if (!c || !record || !hosts || !ports || count == 0) return 0;
    auto rec = arc::deserialize_pairing(record, len);
    if (!rec) return 0;
    std::vector<std::pair<std::string, uint16_t>> addrs;
    for (size_t i = 0; i < count; ++i)
        if (hosts[i] && *hosts[i]) addrs.emplace_back(hosts[i], uint16_t(ports[i] ? ports[i] : ARC_DEFAULT_PORT));
    if (addrs.empty()) return 0;
    c->client->resume(addrs, *rec);
    arc::wipe(rec->pair_key.data(), rec->pair_key.size());
    return 1;
}

void arc_client_submit_code(arc_client* c, const char* code) {
    if (c) c->client->submit_code(s(code));
}

void arc_client_disconnect(arc_client* c) {
    if (c) c->client->disconnect();
}

void arc_client_set_endpoint_volume(arc_client* c, const char* id, uint16_t v) {
    if (c && id) c->client->set_endpoint_volume(id, v);
}
void arc_client_set_endpoint_mute(arc_client* c, const char* id, int m) {
    if (c && id) c->client->set_endpoint_mute(id, m != 0);
}
void arc_client_set_source_volume(arc_client* c, const char* id, uint16_t v) {
    if (c && id) c->client->set_source_volume(id, v);
}
void arc_client_set_source_mute(arc_client* c, const char* id, int m) {
    if (c && id) c->client->set_source_mute(id, m != 0);
}
void arc_client_request_icon(arc_client* c, const char* key) {
    if (c && key) c->client->request_icon(key);
}
void arc_client_set_meters(arc_client* c, int on) {
    if (c) c->client->set_meters(on != 0);
}

void arc_client_set_source_route(arc_client* c, const char* source_id, const char* endpoint_id) {
    if (c && source_id) c->client->set_source_route(source_id, endpoint_id ? endpoint_id : "");
}

void arc_client_panic_mute(arc_client* c, int on) {
    if (c) c->client->panic_mute(on != 0);
}

void arc_client_refresh(arc_client* c) {
    if (c) c->client->refresh();
}

int arc_record_info(const uint8_t* record, size_t len, uint8_t out_agent_id[16], char* out_name, size_t cap) {
    if (!record) return 0;
    auto rec = arc::deserialize_pairing(record, len);
    if (!rec) return 0;
    if (out_agent_id) std::memcpy(out_agent_id, rec->agent_id.data(), 16);
    if (out_name && cap > 0) {
        size_t n = std::min(cap - 1, rec->agent_name.size());
        while (n > 0 && n < rec->agent_name.size() && (uint8_t(rec->agent_name[n]) & 0xC0) == 0x80) --n;
        std::memcpy(out_name, rec->agent_name.data(), n);
        out_name[n] = 0;
    }
    arc::wipe(rec->pair_key.data(), rec->pair_key.size());
    return 1;
}

}  // extern "C"
