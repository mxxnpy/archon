// archon-cli — reference controller on the command line.
//
//   archon-cli pair  <host[:port]> <record-file> [--code-from <file>]
//   archon-cli ls    <host[:port]> <record-file>
//   archon-cli vol   <host[:port]> <record-file> <source-or-endpoint-id> <0-100>
//   archon-cli mute  <host[:port]> <record-file> <source-or-endpoint-id> <on|off>
//
// The pairing record holds a secret key: keep the file private.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "archon/client.hpp"
#include "archon/crypto.hpp"

using namespace arc;
using namespace std::chrono_literals;

namespace {

struct State : ClientListener {
    std::mutex mu;
    std::condition_variable cv;
    ClientStatus status = ClientStatus::Idle;
    ErrorCode error = ErrorCode::None;
    std::string message;
    std::optional<PairingRecord> record;
    std::map<std::string, EndpointInfo> eps;
    std::map<std::string, SourceInfo> srcs;
    bool synced = false;

    void on_status(ClientStatus s, ErrorCode e, const std::string& m) override {
        std::lock_guard<std::mutex> lk(mu);
        status = s;
        error = e;
        message = m;
        cv.notify_all();
    }
    void on_event(const ControllerEvent& ev) override {
        std::lock_guard<std::mutex> lk(mu);
        using K = ControllerEvent::Kind;
        if (ev.kind == K::Paired) record = ev.pairing;
        if (ev.kind == K::StateReset) { eps.clear(); srcs.clear(); synced = false; }
        if (ev.kind == K::Endpoint) eps[ev.endpoint.id] = ev.endpoint;
        if (ev.kind == K::Source) srcs[ev.source.id] = ev.source;
        if (ev.kind == K::EndpointRemoved) eps.erase(ev.id);
        if (ev.kind == K::SourceRemoved) srcs.erase(ev.id);
        if (ev.kind == K::StateSynced) synced = true;
        cv.notify_all();
    }
    template <class P>
    bool wait(P p, std::chrono::seconds t) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, t, [&] { return p() || status == ClientStatus::Failed; }) && p();
    }
};

bool split_host(const std::string& in, std::string& host, uint16_t& port) {
    port = kDefaultPort;
    const auto c = in.rfind(':');
    if (c != std::string::npos && in.find(':') == c) {
        host = in.substr(0, c);
        const int p = std::atoi(in.c_str() + c + 1);
        if (p <= 0 || p > 65535) return false;
        port = uint16_t(p);
    } else {
        host = in;
    }
    return !host.empty();
}

Id16 controller_id(const std::string& record_file) {
    // Stable per record file so the PC recognizes this CLI on later runs.
    const std::string idf = record_file + ".id";
    Id16 id{};
    std::ifstream in(idf, std::ios::binary);
    if (in.read(reinterpret_cast<char*>(id.data()), 16)) return id;
    id = random_array<16>();
    std::ofstream(idf, std::ios::binary).write(reinterpret_cast<const char*>(id.data()), 16);
    return id;
}

int fail(State& st) {
    std::fprintf(stderr, "failed: %s %s\n", error_code_name(st.error), st.message.c_str());
    return 1;
}

int usage() {
    std::fprintf(stderr,
                 "usage:\n  archon-cli pair <host[:port]> <record-file> [--code-from <file>]\n"
                 "  archon-cli ls   <host[:port]> <record-file>\n"
                 "  archon-cli vol  <host[:port]> <record-file> <id> <0-100>\n"
                 "  archon-cli mute <host[:port]> <record-file> <id> <on|off>\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) return usage();
    const std::string cmd = argv[1], rec_file = argv[3];
    std::string host;
    uint16_t port;
    if (!split_host(argv[2], host, port)) return usage();

    State st;
    Client client(controller_id(rec_file), "archon-cli", st);

    if (cmd == "pair") {
        std::string code_from;
        if (argc >= 6 && std::strcmp(argv[4], "--code-from") == 0) code_from = argv[5];
        client.pair(host, port);
        if (!st.wait([&] { return st.status == ClientStatus::AwaitingCode; }, 10s)) return fail(st);
        std::string code;
        if (code_from.empty()) {
            std::printf("Code shown on the PC: ");
            std::fflush(stdout);
            std::getline(std::cin, code);
        } else {
            std::printf("waiting for the code in %s\n", code_from.c_str());
            for (int i = 0; i < 1200 && code.size() != 6; ++i) {
                std::ifstream in(code_from);
                std::getline(in, code);
                if (code.size() != 6) std::this_thread::sleep_for(100ms);
            }
        }
        client.submit_code(code);
        if (!st.wait([&] { return st.record.has_value() && st.status == ClientStatus::Connected; }, 15s)) return fail(st);
        const Bytes rec = serialize_pairing(*st.record);
        std::ofstream(rec_file, std::ios::binary).write(reinterpret_cast<const char*>(rec.data()), std::streamsize(rec.size()));
        std::printf("paired with %s; record saved to %s\n", st.record->agent_name.c_str(), rec_file.c_str());
        return 0;
    }

    std::ifstream in(rec_file, std::ios::binary);
    Bytes raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto rec = deserialize_pairing(raw.data(), raw.size());
    if (!rec) {
        std::fprintf(stderr, "no valid pairing record in %s (run pair first)\n", rec_file.c_str());
        return 1;
    }
    client.resume(host, port, *rec);
    if (!st.wait([&] { return st.synced; }, 10s)) return fail(st);

    if (cmd == "ls") {
        std::lock_guard<std::mutex> lk(st.mu);
        for (auto& [id, e] : st.eps) {
            std::printf("%s %-40s %5.1f%%%s\n    id: %s\n", e.is_default ? "*" : " ", e.name.c_str(), e.volume / 100.0,
                        e.muted ? " muted" : "", id.c_str());
            for (auto& [sid, s] : st.srcs) {
                if (s.endpoint_id != id) continue;
                std::printf("      %-30s %5.1f%%%s%s  id: %s\n", s.name.c_str(), s.volume / 100.0,
                            s.muted ? " muted" : "", s.playing ? " playing" : "", sid.c_str());
            }
        }
        return 0;
    }
    if ((cmd == "vol" || cmd == "mute") && argc >= 6) {
        const std::string id = argv[4];
        bool endpoint;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            endpoint = st.eps.count(id) > 0;
            if (!endpoint && !st.srcs.count(id)) {
                std::fprintf(stderr, "unknown id (see ls)\n");
                return 1;
            }
        }
        if (cmd == "vol") {
            const uint16_t v = uint16_t(std::max(0, std::min(100, std::atoi(argv[5]))) * 100);
            endpoint ? client.set_endpoint_volume(id, v) : client.set_source_volume(id, v);
            st.wait([&] { return endpoint ? st.eps[id].volume == v : st.srcs[id].volume == v; }, 5s);
        } else {
            const bool m = std::strcmp(argv[5], "on") == 0;
            endpoint ? client.set_endpoint_mute(id, m) : client.set_source_mute(id, m);
            st.wait([&] { return endpoint ? st.eps[id].muted == m : st.srcs[id].muted == m; }, 5s);
        }
        std::printf("ok\n");
        return 0;
    }
    return usage();
}
