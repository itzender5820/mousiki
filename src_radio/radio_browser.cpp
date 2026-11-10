// Radio Browser client: searches the public station directory on the Radio Browser servers, with a
// small JSON parser for the replies.
#include "radio_browser.h"
#include "utf8_util.h"
#include "string_util.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include "process_util.h"

namespace muisc::radio {

namespace {

const char* const kUserAgent = "Mousiki-Radio/2.5 (terminal music player; https://radio-browser.info client)";
const char* const kServers[] = {"de1.api.radio-browser.info", "all.api.radio-browser.info",
                                "nl1.api.radio-browser.info", "at1.api.radio-browser.info"};



// ---------------------------------------------------------------------------------------------
// Minimal JSON reader: objects, arrays, strings (all escapes incl. \uXXXX and surrogate pairs),
// numbers, true/false/null. Radio Browser sends plain JSON; this never throws.
// ---------------------------------------------------------------------------------------------
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J* get(const char* k) const {
        for (const auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

class JParser {
public:
    explicit JParser(const std::string& text) : s_(text) {}
    bool parse(J& out) { ws(); if (!value(out, 0)) return false; ws(); return ok_; }
private:
    const std::string& s_;
    size_t i_ = 0;
    bool ok_ = true;
    void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_; }
    bool fail() { ok_ = false; return false; }
    static void put_utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (i_ + 4 > s_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_ + static_cast<size_t>(k)];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        i_ += 4;
        return true;
    }
    bool str(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return fail();
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) return fail();
            const char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += ' '; break;
                case 'f': out += ' '; break;
                case 'n': out += ' '; break;
                case 'r': out += ' '; break;
                case 't': out += ' '; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return fail();
                    if (cp >= 0xD800 && cp < 0xDC00 && i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        const size_t save = i_;
                        i_ += 2;
                        unsigned lo;
                        if (hex4(lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else i_ = save;
                    }
                    if (cp < 0x20) cp = ' ';
                    put_utf8(out, cp);
                    break;
                }
                default: return fail();
            }
        }
        return fail();
    }
    bool value(J& out, int depth) {
        if (depth > 8 || i_ >= s_.size()) return fail();
        const char c = s_[i_];
        if (c == '{') {
            out.t = J::Obj; ++i_; ws();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            while (true) {
                ws();
                std::string key;
                if (!str(key)) return false;
                ws();
                if (i_ >= s_.size() || s_[i_] != ':') return fail();
                ++i_; ws();
                J v;
                if (!value(v, depth + 1)) return false;
                out.o.emplace_back(std::move(key), std::move(v));
                ws();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                return fail();
            }
        }
        if (c == '[') {
            out.t = J::Arr; ++i_; ws();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            while (true) {
                ws();
                J v;
                if (!value(v, depth + 1)) return false;
                out.a.push_back(std::move(v));
                ws();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                return fail();
            }
        }
        if (c == '"') { out.t = J::Str; return str(out.s); }
        if (s_.compare(i_, 4, "true") == 0) { out.t = J::Bool; out.b = true; i_ += 4; return true; }
        if (s_.compare(i_, 5, "false") == 0) { out.t = J::Bool; out.b = false; i_ += 5; return true; }
        if (s_.compare(i_, 4, "null") == 0) { out.t = J::Null; i_ += 4; return true; }
        const size_t start = i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' || s_[i_] == '+' || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E')) ++i_;
        if (i_ == start) return fail();
        out.t = J::Num;
        out.n = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        return true;
    }
};

std::string jstr(const J& o, const char* k) {
    const J* v = o.get(k);
    if (!v) return "";
    if (v->t == J::Str) return trim_ws(clean_tag_text(v->s));   // anyone can add a station: no control characters (a "\u001b" in the JSON is a real ESC), well-formed UTF-8
    if (v->t == J::Num) return std::to_string(static_cast<long long>(v->n));
    return "";
}
double jnum(const J& o, const char* k) {
    const J* v = o.get(k);
    if (!v) return 0;
    if (v->t == J::Num) return v->n;
    if (v->t == J::Bool) return v->b ? 1 : 0;
    if (v->t == J::Str) return std::atof(v->s.c_str());
    return 0;
}

// "128", "128+", ">=128", "64-192", "-192", "<=192" (+ optional k / kbps) -> min / max. false = unreadable.
bool parse_bitrate(std::string t, int& mn, int& mx) {
    mn = 0; mx = 0;
    t = ascii_lower_str(trim_ws(t));
    if (t.empty()) return true;
    t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
    for (const char* unit : {"kbps", "kb/s", "kbit", "k"}) {
        const std::string u = unit;
        if (t.size() > u.size() && t.compare(t.size() - u.size(), u.size(), u) == 0) { t.resize(t.size() - u.size()); break; }
    }
    auto digits = [](const std::string& x) { return !x.empty() && x.size() <= 6 && std::all_of(x.begin(), x.end(), [](unsigned char c) { return std::isdigit(c) != 0; }); };
    if (t.rfind(">=", 0) == 0 || t.rfind("<=", 0) == 0) {
        const std::string n = t.substr(2);
        if (!digits(n)) return false;
        (t[0] == '>' ? mn : mx) = muisc::atoi_safe(n.c_str());
        return true;
    }
    if (t[0] == '>' || t[0] == '<') {
        const std::string n = t.substr(1);
        if (!digits(n)) return false;
        (t[0] == '>' ? mn : mx) = muisc::atoi_safe(n.c_str());
        return true;
    }
    if (t.back() == '+') { t.pop_back(); if (!digits(t)) return false; mn = muisc::atoi_safe(t.c_str()); return true; }
    const size_t dash = t.find('-');
    if (dash == std::string::npos) { if (!digits(t)) return false; mn = muisc::atoi_safe(t.c_str()); return true; }
    const std::string a = t.substr(0, dash), b = t.substr(dash + 1);
    if ((!a.empty() && !digits(a)) || (!b.empty() && !digits(b)) || (a.empty() && b.empty())) return false;
    if (!a.empty()) mn = muisc::atoi_safe(a.c_str());
    if (!b.empty()) mx = muisc::atoi_safe(b.c_str());
    if (mx > 0 && mn > mx) std::swap(mn, mx);
    return true;
}

std::string curl_error_text(int code, const std::string& host) {
    switch (code) {
        case 6:  case 7:  return "no connection to " + host + " (is the network up?)";
        case 28: return host + " did not answer in time";
        case 35: case 51: case 58: case 60: case 77: return "TLS problem talking to " + host;
        case 22: return host + " answered with an error";
        case 127: case 126: case -1: return "could not run curl (is it installed and on the PATH?)";
        default: return "request to " + host + " failed (curl exit " + std::to_string(code) + ")";
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
bool build_browse_args(const BrowseQuery& q, std::vector<std::pair<std::string, std::string>>& args, std::string* error) {
    args.clear();
    auto add = [&](const char* k, const std::string& v) { if (!v.empty()) args.emplace_back(k, v); };
    add("name", trim_ws(q.name));
    // tags: comma separated -> tagList (every tag has to match)
    {
        std::string list, cur;
        auto flush = [&]() { const std::string t = trim_ws(cur); if (!t.empty()) list += (list.empty() ? "" : ",") + t; cur.clear(); };
        for (char c : q.tags) { if (c == ',') flush(); else cur += c; }
        flush();
        add("tagList", list);
    }
    // country: a 2-letter code goes to countrycode (exact), anything else to the (substring) country name
    {
        const std::string c = trim_ws(q.country);
        const bool code = c.size() == 2 && std::isalpha(static_cast<unsigned char>(c[0])) && std::isalpha(static_cast<unsigned char>(c[1]));
        if (code) { std::string u = c; for (auto& ch : u) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); add("countrycode", u); }
        else add("country", c);
    }
    add("state", trim_ws(q.state));
    add("language", trim_ws(q.language));
    int mn = 0, mx = 0;
    if (!parse_bitrate(q.bitrate, mn, mx)) {
        if (error) *error = "bitrate: use 128 (at least), 64-192 (range) or -192 (at most)";
        return false;
    }
    if (mn > 0) args.emplace_back("bitrateMin", std::to_string(mn));
    if (mx > 0) args.emplace_back("bitrateMax", std::to_string(mx));
    args.emplace_back("hidebroken", "true");
    args.emplace_back("order", "clickcount");
    args.emplace_back("reverse", "true");
    args.emplace_back("limit", std::to_string(kBrowseLimit));
    return true;
}

std::vector<BrowseStation> parse_browse_json(const std::string& json, bool* ok) {
    std::vector<BrowseStation> out;
    J root;
    const bool parsed = JParser(json).parse(root) && root.t == J::Arr;
    if (ok) *ok = parsed;
    if (!parsed) return out;
    out.reserve(root.a.size());
    for (const J& o : root.a) {
        if (o.t != J::Obj) continue;
        BrowseStation b;
        b.uuid = jstr(o, "stationuuid");
        b.name = jstr(o, "name");
        b.url = jstr(o, "url");
        b.url_resolved = jstr(o, "url_resolved");
        if (b.name.empty() || !Station::web_url(b.stream_url())) continue;   // only http(s) streams (see Station::web_url)
        b.homepage = jstr(o, "homepage");
        b.tags = jstr(o, "tags");
        b.country = jstr(o, "country");
        b.countrycode = jstr(o, "countrycode");
        b.state = jstr(o, "state");
        b.language = jstr(o, "language");
        b.codec = jstr(o, "codec");
        int br = static_cast<int>(jnum(o, "bitrate"));
        if (br > 3000) br /= 1000;          // some entries carry bit/s
        b.bitrate = std::max(0, br);
        b.votes = static_cast<int>(jnum(o, "votes"));
        b.clickcount = static_cast<int>(jnum(o, "clickcount"));
        b.online = jnum(o, "lastcheckok") != 0;
        b.hls = jnum(o, "hls") != 0;
        b.lastchecktime = jstr(o, "lastchecktime");
        out.push_back(std::move(b));
    }
    return out;
}

Station browse_to_station(const BrowseStation& b) {
    Station s;
    s.name = b.name;
    // genre = the first three tags, " / " separated
    {
        std::string g, cur;
        int n = 0;
        auto flush = [&]() { const std::string t = trim_ws(cur); cur.clear(); if (t.empty() || n >= 3) return; g += (g.empty() ? "" : " / ") + t; ++n; };
        for (char c : b.tags) { if (c == ',') flush(); else cur += c; }
        flush();
        s.genre = g;
    }
    s.country = !b.countrycode.empty() ? b.countrycode : b.country;
    s.codec_hint = b.codec;
    s.bitrate_hint = b.bitrate;
    s.url = b.stream_url();
    s.favorite = false;
    s.dial_mhz = dial_for_name(b.name);
    return s;
}

// ===============================================================================================
struct RadioBrowser::Shared {
    mutable std::mutex m;
    BrowseState state = BrowseState::Idle;
    std::string message;
    std::shared_ptr<const std::vector<BrowseStation>> results;
    unsigned generation = 0;
    std::string server;
    std::string good_server;   // tried first next time
    int running = 0;           // worker threads alive (for wait())
};

namespace {

// GET <base><path> with curl; query args via -G --data-urlencode. Output in `body`, curl's exit code returned.
int curl_get(const std::string& base, const std::string& path,
             const std::vector<std::pair<std::string, std::string>>& args, std::string& body) {
    muisc::Args cmd = {"curl", "-sS", "-f", "-L", "--connect-timeout", "6", "--max-time", "25", "-A", kUserAgent,
                       "-G", base + path};
    for (const auto& kv : args) { cmd.push_back("--data-urlencode"); cmd.push_back(kv.first + "=" + kv.second); }
    const muisc::ProcResult r = muisc::run_capture(cmd, false);
    body = r.out;
    return r.exit_code;
}

// The bases to try, in order. MOUSIKI_RADIO_API replaces the list.
std::vector<std::pair<std::string, std::string>> server_bases(const std::string& good) {   // {host label, base url}
    std::vector<std::pair<std::string, std::string>> v;
    if (const char* e = std::getenv("MOUSIKI_RADIO_API")) {
        if (*e) { std::string b = e; while (!b.empty() && b.back() == '/') b.pop_back(); v.emplace_back(b, b); return v; }
    }
    if (!good.empty()) v.emplace_back(good, "https://" + good);
    for (const char* h : kServers) if (good != h) v.emplace_back(h, std::string("https://") + h);
    return v;
}

} // namespace

RadioBrowser::RadioBrowser() : sh_(std::make_shared<Shared>()) {}
RadioBrowser::~RadioBrowser() = default;

void RadioBrowser::search(const BrowseQuery& q) {
    std::vector<std::pair<std::string, std::string>> args;
    std::string err;
    auto sh = sh_;
    if (!build_browse_args(q, args, &err)) {
        std::lock_guard<std::mutex> lk(sh->m);
        ++sh->generation;
        sh->state = BrowseState::Error;
        sh->message = err;
        sh->results.reset();
        return;
    }
    unsigned gen;
    std::string good;
    {
        std::lock_guard<std::mutex> lk(sh->m);
        gen = ++sh->generation;
        sh->state = BrowseState::Searching;
        sh->message = "searching ...";
        good = sh->good_server;
        ++sh->running;
    }
    std::thread([sh, args, gen, good]() {
        std::string last_error = "no server answered";
        bool done = false;
        try {   // (an exception out of this thread would end the program)
        for (const auto& [label, base] : server_bases(good)) {
            {   // superseded while we were trying servers: stop early
                std::lock_guard<std::mutex> lk(sh->m);
                if (sh->generation != gen) { done = true; break; }
            }
            std::string body;
            const int code = curl_get(base, "/json/stations/search", args, body);
            if (code != 0) { last_error = curl_error_text(code, label); continue; }
            bool ok = false;
            auto list = parse_browse_json(body, &ok);
            if (!ok) { last_error = label + " sent something that is not a station list"; continue; }
            std::lock_guard<std::mutex> lk(sh->m);
            if (sh->generation == gen) {
                sh->results = std::make_shared<const std::vector<BrowseStation>>(std::move(list));
                sh->state = BrowseState::Done;
                sh->server = label;
                sh->good_server = label;
                sh->message = sh->results->empty() ? "no stations found" : "";
            }
            done = true;
            break;
        }
        } catch (...) { last_error = "internal error while searching"; done = false; }
        std::lock_guard<std::mutex> lk(sh->m);
        if (!done && sh->generation == gen) {
            sh->state = BrowseState::Error;
            sh->message = last_error;
            sh->results.reset();
        }
        --sh->running;
    }).detach();
}

void RadioBrowser::count_click(const std::string& uuid) {
    if (uuid.empty() || !std::all_of(uuid.begin(), uuid.end(), [](unsigned char c) { return std::isxdigit(c) || c == '-'; })) return;
    auto sh = sh_;
    std::string good;
    { std::lock_guard<std::mutex> lk(sh->m); good = sh->good_server; }
    std::thread([uuid, good]() {
        try {   // (an exception out of this thread would end the program)
            for (const auto& [label, base] : server_bases(good)) {
                std::string body;
                if (curl_get(base, "/json/url/" + uuid, {}, body) == 0) return;
                (void)label;
            }
        } catch (...) {}
    }).detach();
}

BrowseSnapshot RadioBrowser::snapshot() const {
    std::lock_guard<std::mutex> lk(sh_->m);
    BrowseSnapshot s;
    s.state = sh_->state;
    s.message = sh_->message;
    s.results = sh_->results;
    s.generation = sh_->generation;
    s.server = sh_->server;
    return s;
}

void RadioBrowser::wait() {
    for (int i = 0; i < 1200; ++i) {   // at most ~2 minutes
        { std::lock_guard<std::mutex> lk(sh_->m); if (sh_->running == 0) return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace muisc::radio
