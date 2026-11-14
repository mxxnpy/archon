// Adversarial: AudioSink callbacks from many threads while controllers churn,
// Server::stop() under traffic, meter-slot invariants.

#include <random>
#include <set>

#include "adv_common.hpp"

using namespace adv;

namespace {

struct Truth {
    std::map<std::string, EndpointInfo> endpoints;  // present ids only
    std::map<std::string, SourceInfo> sources;
};

// Each worker owns its ids, so the last operation per id is the expected state.
void hammer(Server& s, int t, std::atomic<bool>& go, Truth& truth, std::mutex& tmu) {
    std::mt19937 rng(unsigned(1000 + t));
    std::map<std::string, std::optional<EndpointInfo>> eps;
    std::map<std::string, std::optional<SourceInfo>> srcs;
    while (go) {
        const int k = int(rng() % 12);
        const std::string eid = "T" + std::to_string(t) + "-E" + std::to_string(k);
        const std::string sid = "T" + std::to_string(t) + "-S" + std::to_string(k);
        switch (rng() % 5) {
            case 0: {
                EndpointInfo e{eid, "ep", uint16_t(rng() % 10001), bool(rng() & 1), false};
                s.endpoint_upsert(e);
                eps[eid] = e;
                break;
            }
            case 1:
                s.endpoint_removed(eid);
                eps[eid] = std::nullopt;
                break;
            case 2: {
                SourceInfo src;
                src.id = sid;
                src.endpoint_id = eid;
                src.name = "app";
                src.volume = uint16_t(rng() % 10001);
                src.pid = rng();
                s.source_upsert(src);
                srcs[sid] = src;
                break;
            }
            case 3:
                s.source_removed(sid);
                srcs[sid] = std::nullopt;
                break;
            default:
                s.meters({{eid, uint8_t(rng())}, {sid, uint8_t(rng())}});
                break;
        }
    }
    std::lock_guard<std::mutex> lk(tmu);
    for (auto& [id, e] : eps)
        if (e) truth.endpoints[id] = *e;
    for (auto& [id, src] : srcs)
        if (src) truth.sources[id] = *src;
}

template <class M>
std::map<std::string, typename M::mapped_type> only_t(const M& m) {
    std::map<std::string, typename M::mapped_type> out;
    for (auto& [id, v] : m)
        if (!id.empty() && id[0] == 'T') out[id] = v;
    return out;
}

bool same_model(const Truth& t, Listener& l, std::string* why) {
    std::lock_guard<std::mutex> lk(l.mu);
    auto eps = only_t(l.endpoints);
    auto srcs = only_t(l.sources);
    if (eps.size() != t.endpoints.size() || srcs.size() != t.sources.size()) {
        *why = "sizes ep " + std::to_string(eps.size()) + "/" + std::to_string(t.endpoints.size()) + " src " +
               std::to_string(srcs.size()) + "/" + std::to_string(t.sources.size());
        return false;
    }
    for (auto& [id, e] : t.endpoints) {
        auto it = eps.find(id);
        if (it == eps.end() || it->second.volume != e.volume || it->second.muted != e.muted) {
            *why = "endpoint " + id;
            return false;
        }
    }
    for (auto& [id, s] : t.sources) {
        auto it = srcs.find(id);
        if (it == srcs.end() || it->second.volume != s.volume || it->second.pid != s.pid) {
            *why = "source " + id;
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(adv_sink_hammer_with_controller_churn_converges) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 watcher_id = random_array<16>(), churn_id = random_array<16>();
    const Key32 watcher_key = rig.provision(watcher_id), churn_key = rig.provision(churn_id);
    // A controller connected for the whole storm follows incremental updates.
    Listener watcher_l;
    Client watcher(watcher_id, "watcher", watcher_l, fast_timing());
    watcher.resume("127.0.0.1", rig.port, rig.record_for(watcher_key));
    REQUIRE(wait_for(watcher_l.mu, [&] { return watcher_l.status == ClientStatus::Connected && watcher_l.synced > 0; }));
    watcher.set_meters(true);

    Truth truth;
    std::mutex tmu;
    std::atomic<bool> go{true};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) workers.emplace_back([&, t] { hammer(*rig.server, t, go, truth, tmu); });
    // Controllers come and go during the storm.
    const auto t0 = SClock::now();
    int cycles = 0;
    while (ms_since(t0) < 1500) {
        Listener l;
        Client c(churn_id, "churn", l, fast_timing());
        c.resume("127.0.0.1", rig.port, rig.record_for(churn_key));
        c.set_meters(cycles % 2 == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(10 + (cycles * 13) % 60));
        ++cycles;
    }
    go = false;
    for (auto& w : workers) w.join();
    // Sentinel: once it arrives, every earlier post has been processed and sent.
    rig.server->endpoint_upsert(EndpointInfo{"ZZ-sentinel", "s", 1, false, false});
    REQUIRE(wait_for(watcher_l.mu, [&] { return watcher_l.endpoints.count("ZZ-sentinel") == 1; }, 8000));
    std::string why;
    CHECK(same_model(truth, watcher_l, &why));
    if (!why.empty()) std::printf("  incremental mismatch: %s\n", why.c_str());
    // A fresh controller's dump agrees too.
    Listener fresh_l;
    Client fresh(churn_id, "fresh", fresh_l, fast_timing());
    fresh.resume("127.0.0.1", rig.port, rig.record_for(churn_key));
    REQUIRE(wait_for(fresh_l.mu, [&] { return fresh_l.status == ClientStatus::Connected && fresh_l.synced > 0; }));
    why.clear();
    CHECK(same_model(truth, fresh_l, &why));
    if (!why.empty()) std::printf("  dump mismatch: %s\n", why.c_str());
    std::printf("  churn cycles: %d, watcher events: %d\n", cycles, watcher_l.events.load());
}

// Sink callbacks keep arriving from audio threads while the agent shuts down.
TEST(adv_stop_while_traffic_and_sink_callbacks_flow) {
    int slow_stops = 0;
    for (int rep = 0; rep < 15; ++rep) {
        auto rig = std::make_unique<AdvRig>();
        REQUIRE(rig->port != 0);
        const Id16 cid = random_array<16>();
        const Key32 key = rig->provision(cid);
        Listener l;
        Client c(cid, "phone", l, fast_timing());
        c.resume("127.0.0.1", rig->port, rig->record_for(key));
        c.set_meters(true);
        wait_for(l.mu, [&] { return l.status == ClientStatus::Connected; }, 2000);
        std::atomic<bool> go{true};
        Server* srv = rig->server.get();
        std::vector<std::thread> threads;
        for (int t = 0; t < 3; ++t) {
            threads.emplace_back([srv, &go, t] {
                uint16_t v = 0;
                while (go) {
                    srv->endpoint_upsert(EndpointInfo{"E1", "Speakers", uint16_t(v++ % 10000), false, true});
                    srv->meters({{"E1", uint8_t(v)}});
                    if (t == 0) srv->source_removed("nope");
                }
            });
        }
        for (int i = 0; i < 50; ++i) c.set_endpoint_volume("E1", uint16_t(i * 100));
        std::this_thread::sleep_for(std::chrono::milliseconds(10 + rep * 3));
        const auto t0 = SClock::now();
        srv->stop();
        if (ms_since(t0) > 2000) ++slow_stops;
        std::this_thread::sleep_for(5ms);  // callbacks after stop() returned
        go = false;
        for (auto& t : threads) t.join();
        c.disconnect();
        rig.reset();
    }
    CHECK(slow_stops == 0);
}

// Contract §5: meter_slot is stable while the item exists and never 0; two live
// items sharing a slot would show one app's level on another's meter.
TEST(adv_meter_slots_unique_after_counter_wrap) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    // E1 and E1|100 from the backend took allocations #1 and #2 (slots 1, 2).
    // Allocation #k gets slot ((k-1) mod 65535)+1, so 65533 short-lived sessions
    // (#3..#65535) bring the counter back to slot 1 for the next newcomer.
    for (int i = 0; i < 65533; ++i) {
        SourceInfo s;
        s.id = "tmp" + std::to_string(i);
        s.endpoint_id = "E1";
        rig.server->source_upsert(s);
        rig.server->source_removed(s.id);
        if ((i & 4095) == 0) std::this_thread::sleep_for(1ms);
    }
    for (int i = 0; i < 4; ++i) {
        SourceInfo s;
        s.id = "new" + std::to_string(i);
        s.endpoint_id = "E1";
        rig.server->source_upsert(s);
    }
    const Id16 cid = random_array<16>();
    const Key32 key = rig.provision(cid);
    Listener l;
    Client c(cid, "phone", l, fast_timing());
    c.resume("127.0.0.1", rig.port, rig.record_for(key));
    REQUIRE(wait_for(l.mu, [&] { return l.status == ClientStatus::Connected && l.sources.count("new3"); }, 8000));
    std::map<uint16_t, std::vector<std::string>> owners;
    {
        std::lock_guard<std::mutex> lk(l.mu);
        for (auto& [id, e] : l.endpoints) owners[e.meter].push_back(id);
        for (auto& [id, s] : l.sources) owners[s.meter].push_back(id);
    }
    int dup = 0;
    for (auto& [slot, ids] : owners) {
        if (slot == 0 || ids.size() > 1) {
            ++dup;
            std::printf("  slot %u shared by:", unsigned(slot));
            for (auto& id : ids) std::printf(" %s", id.c_str());
            std::printf("\n");
        }
    }
    CHECK(dup == 0);
}
