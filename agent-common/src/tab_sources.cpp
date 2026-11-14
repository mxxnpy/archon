#include "tab_sources.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "mini_json.hpp"

namespace agent {

namespace {

constexpr size_t kMaxIconB64 = (arc::kMaxIconBytes + 2) / 3 * 4;

void put_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

// Next code point of UTF-8 text; invalid bytes give U+FFFD (one byte consumed).
uint32_t next_cp(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || i + size_t(len) > s.size()) {
        ++i;
        return 0xFFFD;
    }
    uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < len; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + size_t(k)]);
        if ((cc & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    static const uint32_t min_for[5] = {0, 0, 0x80, 0x800, 0x10000};
    i += size_t(len);
    if (cp < min_for[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;  // overlong / invalid
    return cp;
}

// Text from web pages, made safe to show: valid UTF-8, control characters
// (C0, DEL, C1) as spaces, no direction overrides that could make a title read
// backwards, cut at `max` bytes without splitting a character.
std::string clean_text(const std::string& s, size_t max) {
    std::string out;
    out.reserve(std::min(s.size(), max));
    for (size_t i = 0; i < s.size() && out.size() <= max;) {
        const uint32_t cp = next_cp(s, i);
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) {
            out += ' ';
        } else if (cp == 0x200E || cp == 0x200F || cp == 0x061C || (cp >= 0x202A && cp <= 0x202E) ||
                   (cp >= 0x2066 && cp <= 0x2069)) {
            continue;  // bidi controls: dropped
        } else {
            put_utf8(out, cp);
        }
    }
    if (out.size() > max) {
        size_t cut = max;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    return out;
}

bool base64_decode(const std::string& in, arc::Bytes& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    if (in.size() % 4 != 0) return false;
    for (size_t i = 0; i < in.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = in[i + size_t(k)];
            if (c == '=' && i + 4 == in.size() && k >= 2) {
                v[k] = 0;
                ++pad;
            } else {
                if (pad) return false;  // data after padding
                v[k] = val(c);
                if (v[k] < 0) return false;
            }
        }
        const uint32_t n = uint32_t(v[0]) << 18 | uint32_t(v[1]) << 12 | uint32_t(v[2]) << 6 | uint32_t(v[3]);
        out.push_back(uint8_t(n >> 16));
        if (pad < 2) out.push_back(uint8_t(n >> 8));
        if (pad < 1) out.push_back(uint8_t(n));
    }
    return true;
}

bool is_png(const arc::Bytes& b) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return b.size() > 8 && std::equal(sig, sig + 8, b.begin());
}

std::string fnv_hex(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 8; ++i) out += hex[(h >> (60 - 4 * i)) & 15];
    return out;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string pretty_browser(const std::string& b) {
    if (b == "brave") return "Brave";
    if (b == "chrome") return "Chrome";
    if (b == "edge") return "Edge";
    if (b == "firefox") return "Firefox";
    if (b == "opera") return "Opera";
    return "Browser";
}

std::string tab_id(uint64_t conn, int64_t tab) { return "tab:" + std::to_string(conn) + ":" + std::to_string(tab); }

// A JSON number that is a whole tab id.
bool whole(const json::Value& v, int64_t& out) {
    if (v.type != json::Value::Type::Number) return false;
    const double d = v.num;
    if (d < 0 || d > 9007199254740991.0 || std::floor(d) != d) return false;
    out = int64_t(d);
    return true;
}

}  // namespace

TabSources::TabSources(arc::AudioBackend& inner, std::function<std::string(uint32_t)> exe_of_pid)
    : inner_(inner), exe_of_pid_(std::move(exe_of_pid)) {}

// ---- browser side ----------------------------------------------------------------

uint64_t TabSources::connect(TabLink* link) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!link || links_.size() >= kMaxLinks) return 0;
    const uint64_t id = next_conn_++;
    links_[id].link = link;
    return id;
}

void TabSources::disconnect(uint64_t conn) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = links_.find(conn);
    if (it == links_.end()) return;
    if (sink_)
        for (auto& [tid, t] : it->second.tabs) sink_->source_removed(t.info.id);
    links_.erase(it);
}

size_t TabSources::link_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return links_.size();
}

size_t TabSources::tab_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (auto& [id, l] : links_) n += l.tabs.size();
    return n;
}

std::string TabSources::default_output() const {
    for (auto& [id, is_default] : endpoints_)
        if (is_default) return id;
    return endpoints_.empty() ? std::string() : endpoints_.begin()->first;
}

arc::SourceInfo TabSources::place(uint64_t, const Link& l, arc::SourceInfo s) const {
    // On the browser's own output (its audio session), named like it; else the default output.
    const AppSource* app = nullptr;
    if (!l.exe.empty()) {
        const std::string want = lower(l.exe);
        for (auto& [id, a] : apps_)
            if (!a.exe.empty() && lower(a.exe) == want) {
                app = &a;
                break;
            }
    }
    s.endpoint_id = app ? app->endpoint_id : default_output();
    s.group = clean_text(app && !app->name.empty() ? app->name : pretty_browser(l.browser), arc::kMaxGroup);
    return s;
}

void TabSources::refresh_link(uint64_t conn, Link& l) {
    for (auto& [tid, t] : l.tabs) {
        arc::SourceInfo placed = place(conn, l, t.info);
        if (placed != t.info) {
            t.info = placed;
            if (sink_) sink_->source_upsert(t.info);
        }
    }
}

void TabSources::message(uint64_t conn, const std::string& text) {
    json::Limits lim;
    lim.max_string = 512 * 1024;  // an oversized icon is dropped per tab below, not the whole list
    json::Value msg;
    if (text.size() > 4 * 1024 * 1024 || !json::parse(text, msg, lim) || !msg.is_object()) return;
    const std::string kind = msg["t"].string_or();

    std::lock_guard<std::mutex> lk(mu_);
    auto lit = links_.find(conn);
    if (lit == links_.end()) return;
    Link& l = lit->second;
    const bool first = l.messages++ == 0;

    if (kind == "host") {  // added by the native host, always first: which browser executable this is
        if (!first) return;  // the extension cannot claim to be another browser later
        l.exe = clean_text(msg["exe"].string_or(), 1024);
        refresh_link(conn, l);
        return;
    }
    if (kind == "hello") {
        std::string b = lower(msg["browser"].string_or());
        if (b.size() > 16 || b.find_first_not_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos) b.clear();
        l.browser = b;
        refresh_link(conn, l);
        return;
    }
    if (kind != "tabs" || !msg["tabs"].is_array()) return;

    std::map<int64_t, Tab> next;
    for (const json::Value& v : msg["tabs"].arr) {
        if (next.size() >= kMaxTabsPerLink) break;
        int64_t id = 0;
        if (!v.is_object() || !whole(v["id"], id) || next.count(id)) continue;
        const auto old = l.tabs.find(id);
        Tab tab;
        arc::SourceInfo& s = tab.info;
        s.id = tab_id(conn, id);
        s.tab = true;
        s.detail = clean_text(v["host"].string_or(), arc::kMaxDetail);
        s.name = clean_text(v["title"].string_or(), arc::kMaxDisplayName);
        if (s.name.empty()) s.name = s.detail.empty() ? "Tab" : s.detail;
        s.playing = v["audible"].bool_or(false);
        s.muted = v["muted"].bool_or(false);
        const double vol = v["volume"].number_or(10000);
        s.volume = uint16_t(std::clamp(vol, 0.0, 10000.0));
        // Favicon: decoded once per change; the key changes with it so phones refetch.
        const std::string b64 = v["icon"].string_or();
        const std::string key = b64.empty() ? std::string() : s.id + ":" + fnv_hex(b64);
        if (old != l.tabs.end() && old->second.info.icon_key == key) {
            tab.icon = old->second.icon;
            s.icon_key = key;
        } else if (!key.empty() && b64.size() <= kMaxIconB64 && base64_decode(b64, tab.icon) && is_png(tab.icon) &&
                   tab.icon.size() <= arc::kMaxIconBytes && key.size() <= arc::kMaxIconKey) {
            s.icon_key = key;
        } else {
            tab.icon.clear();
        }
        s = place(conn, l, s);
        next.emplace(id, std::move(tab));
    }
    if (sink_) {
        for (auto& [id, t] : l.tabs)
            if (!next.count(id)) sink_->source_removed(t.info.id);
        for (auto& [id, t] : next) {
            const auto old = l.tabs.find(id);
            if (old == l.tabs.end() || old->second.info != t.info) sink_->source_upsert(t.info);
        }
    }
    l.tabs = std::move(next);
}

bool TabSources::find_tab(const std::string& id, uint64_t& conn, int64_t& tab) {
    for (auto& [cid, l] : links_)
        for (auto& [tid, t] : l.tabs)
            if (t.info.id == id) {
                conn = cid;
                tab = tid;
                return true;
            }
    return false;
}

// ---- arc::AudioBackend ------------------------------------------------------------------

bool TabSources::start(arc::AudioSink& sink, std::string* err) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        sink_ = &sink;
        // Tabs reported before the server started.
        for (auto& [cid, l] : links_)
            for (auto& [tid, t] : l.tabs) sink.source_upsert(t.info);
    }
    return inner_.start(*this, err);
}

void TabSources::stop() {
    inner_.stop();
    std::lock_guard<std::mutex> lk(mu_);
    sink_ = nullptr;
}

void TabSources::set_source_volume(const std::string& id, uint16_t volume) {
    if (!is_tab_id(id)) return inner_.set_source_volume(id, volume);
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t conn = 0;
    int64_t tab = 0;
    if (!find_tab(id, conn, tab)) return;
    Link& l = links_[conn];
    Tab& t = l.tabs[tab];
    volume = std::min<uint16_t>(volume, 10000);
    l.link->send("{\"t\":\"volume\",\"id\":" + std::to_string(tab) + ",\"volume\":" + std::to_string(volume) + "}");
    if (t.info.volume != volume) {  // optimistic: the extension confirms with its next report
        t.info.volume = volume;
        if (sink_) sink_->source_upsert(t.info);
    }
}

void TabSources::set_source_mute(const std::string& id, bool muted) {
    if (!is_tab_id(id)) return inner_.set_source_mute(id, muted);
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t conn = 0;
    int64_t tab = 0;
    if (!find_tab(id, conn, tab)) return;
    Link& l = links_[conn];
    Tab& t = l.tabs[tab];
    l.link->send("{\"t\":\"mute\",\"id\":" + std::to_string(tab) + ",\"muted\":" + (muted ? "true" : "false") + "}");
    if (t.info.muted != muted) {
        t.info.muted = muted;
        if (sink_) sink_->source_upsert(t.info);
    }
}

void TabSources::set_source_route(const std::string& source_id, const std::string& endpoint_id) {
    if (is_tab_id(source_id)) return;  // a tab plays where its browser plays
    inner_.set_source_route(source_id, endpoint_id);
}

bool TabSources::icon_png(const std::string& key, arc::Bytes& png) {
    if (!is_tab_id(key)) return inner_.icon_png(key, png);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [cid, l] : links_)
        for (auto& [tid, t] : l.tabs)
            if (t.info.icon_key == key && !t.icon.empty()) {
                png = t.icon;
                return true;
            }
    return false;
}

// ---- arc::AudioSink -----------------------------------------------------------------------

void TabSources::endpoint_upsert(const arc::EndpointInfo& e) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!e.input) {
        const auto it = endpoints_.find(e.id);
        const bool changed = it == endpoints_.end() || it->second != e.is_default;
        endpoints_[e.id] = e.is_default;
        if (sink_) sink_->endpoint_upsert(e);
        if (changed)
            for (auto& [cid, l] : links_) refresh_link(cid, l);
        return;
    }
    if (sink_) sink_->endpoint_upsert(e);
}

void TabSources::endpoint_removed(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool was_output = endpoints_.erase(id) > 0;
    if (sink_) sink_->endpoint_removed(id);
    if (was_output)
        for (auto& [cid, l] : links_) refresh_link(cid, l);
}

void TabSources::source_upsert(const arc::SourceInfo& s) {
    // The executable is looked up once per source, outside the lock.
    bool known = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        known = apps_.count(s.id) > 0;
    }
    const std::string exe = (!known && s.pid && exe_of_pid_) ? exe_of_pid_(s.pid) : std::string();
    std::lock_guard<std::mutex> lk(mu_);
    AppSource& a = apps_[s.id];
    if (!known) a.exe = exe;
    const bool moved = a.endpoint_id != s.endpoint_id || a.name != s.name;
    a.endpoint_id = s.endpoint_id;
    a.name = s.name;
    if (sink_) sink_->source_upsert(s);
    if (moved && !a.exe.empty())
        for (auto& [cid, l] : links_)
            if (!l.exe.empty() && lower(l.exe) == lower(a.exe)) refresh_link(cid, l);
}

void TabSources::source_removed(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = apps_.find(id);
    std::string exe;
    if (it != apps_.end()) {
        exe = it->second.exe;
        apps_.erase(it);
    }
    if (sink_) sink_->source_removed(id);
    if (!exe.empty())
        for (auto& [cid, l] : links_)
            if (!l.exe.empty() && lower(l.exe) == lower(exe)) refresh_link(cid, l);
}

void TabSources::meters(const std::vector<std::pair<std::string, uint8_t>>& levels) {
    std::lock_guard<std::mutex> lk(mu_);
    if (sink_) sink_->meters(levels);
}

}  // namespace agent
