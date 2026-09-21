#include "external_control.h"

#include "process_util.h"
#include "tiny_json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace muisc {
namespace {

// ---------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------

constexpr const char* kMprisPrefix = "org.mpris.MediaPlayer2.";
constexpr const char* kMprisPath   = "/org/mpris/MediaPlayer2";
constexpr const char* kPlayerIface = "org.mpris.MediaPlayer2.Player";
constexpr const char* kRootIface   = "org.mpris.MediaPlayer2";

// Two seconds is generous for a local session-bus round trip (the real
// ones measure in single-digit milliseconds) but short enough that a
// wedged player doesn't visibly stall whatever thread is polling.
constexpr int kDbusTimeoutSecs = 2;
// The outer timeout(1) guard is deliberately LONGER than the D-Bus one:
// we want the tool's own timeout to fire first so we get a clean
// non-zero exit, and only fall back on SIGTERM if the tool itself hangs
// (which busctl can, if the bus socket exists but nothing answers).
constexpr int kHardTimeoutSecs = 4;

enum class Backend { None, Playerctl, Busctl, Gdbus, Osascript };

// ---------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string::npos) {
            std::string last = trim(s.substr(start));
            if (!last.empty()) out.push_back(last);
            break;
        }
        std::string line = trim(s.substr(start, nl - start));
        if (!line.empty()) out.push_back(line);
        start = nl + 1;
    }
    return out;
}

// ---------------------------------------------------------------------
// Backend probe
// ---------------------------------------------------------------------

bool have_tool(const char* name) {
    // `command -v` is a POSIX shell builtin, so this costs one `sh` and
    // no PATH walk of our own. run_capture() already sends stderr to
    // /dev/null; the >/dev/null is for the builtin's stdout.
    ProcResult r = run_capture("command -v " + shell_quote(name) + " >/dev/null 2>&1");
    return r.ok();
}

struct BackendInfo {
    Backend backend = Backend::None;
    bool have_timeout = false;   // is timeout(1) available as a belt-and-braces guard?
};

BackendInfo probe_backend() {
    BackendInfo info;
    info.have_timeout = have_tool("timeout");

    // Escape hatch: a machine with several of these installed only ever
    // exercises the first one in the cascade, which makes the other
    // paths impossible to check by hand. This costs nothing and is not
    // part of the public API contract.
    if (const char* forced = std::getenv("MOUSIKI_EXT_BACKEND")) {
        std::string f = to_lower(trim(forced));
        if (f == "playerctl" && have_tool("playerctl")) { info.backend = Backend::Playerctl; return info; }
        if (f == "busctl"    && have_tool("busctl"))    { info.backend = Backend::Busctl;    return info; }
        if (f == "gdbus"     && have_tool("gdbus"))     { info.backend = Backend::Gdbus;     return info; }
        if (f == "osascript" && have_tool("osascript")) { info.backend = Backend::Osascript; return info; }
        if (f == "none") { info.backend = Backend::None; return info; }
        // An unrecognised/unavailable override just falls through to the
        // normal cascade rather than disabling the feature.
    }

#if defined(__APPLE__)
    // macOS has no MPRIS and no session bus to speak of. osascript is
    // part of the base system, so this is effectively always taken.
    if (have_tool("osascript")) { info.backend = Backend::Osascript; return info; }
#else
    // playerctl first: it already does the bus-name bookkeeping, the
    // metadata flattening and the relative-seek arithmetic, so the code
    // below is a handful of string formats instead of a JSON walk.
    if (have_tool("playerctl")) { info.backend = Backend::Playerctl; return info; }
    // busctl next: it ships with systemd (so it's on essentially every
    // modern Linux desktop) and it can emit JSON, which is far less
    // fragile to read than GVariant's text form.
    if (have_tool("busctl")) { info.backend = Backend::Busctl; return info; }
    // gdbus last: always present wherever glib is, but its output has to
    // be scraped by hand.
    if (have_tool("gdbus")) { info.backend = Backend::Gdbus; return info; }
#endif
    return info;
}

const BackendInfo& backend_info() {
    // Function-local static: initialised exactly once, and C++11 onward
    // guarantees the initialisation is thread-safe, so two threads
    // calling available() at the same time can't race the PATH probe.
    static const BackendInfo info = probe_backend();
    return info;
}

// Wraps a command in timeout(1) when it's available. The D-Bus tools all
// take their own timeout flag, but a tool that never gets as far as
// talking to the bus (a stale DBUS_SESSION_BUS_ADDRESS pointing at a
// socket nobody is accepting on) can still block indefinitely.
std::string guard(const std::string& cmd) {
    if (!backend_info().have_timeout) return cmd;
    return "timeout " + std::to_string(kHardTimeoutSecs) + " " + cmd;
}

// ---------------------------------------------------------------------
// Bus-name scraping
// ---------------------------------------------------------------------

bool is_bus_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
}

// Pulls every `org.mpris.MediaPlayer2.*` name out of an arbitrary blob.
// Deliberately format-agnostic: the same routine works on busctl's JSON
// (names in double quotes), gdbus's GVariant text (single quotes) and
// busctl's column layout (whitespace separated), so none of the three
// discovery paths has to assume an output shape that could change.
std::vector<std::string> scrape_bus_names(const std::string& text) {
    std::vector<std::string> out;
    const std::string prefix = kMprisPrefix;
    size_t pos = 0;
    while ((pos = text.find(prefix, pos)) != std::string::npos) {
        size_t end = pos;
        while (end < text.size() && is_bus_name_char(text[end])) ++end;
        std::string name = text.substr(pos, end - pos);
        pos = end;
        // A bare prefix with no suffix isn't a player, and neither is a
        // name ending in '.' (can't happen on the bus, but the scan is
        // loose enough to produce one from a truncated line).
        if (name.size() <= prefix.size() || name.back() == '.') continue;
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
    }
    return out;
}

// ---------------------------------------------------------------------
// Player identification
// ---------------------------------------------------------------------

std::string short_id(const std::string& bus_name) {
    const std::string prefix = kMprisPrefix;
    if (starts_with(bus_name, prefix)) return bus_name.substr(prefix.size());
    return bus_name;
}

std::string classify_kind(const std::string& id) {
    const std::string l = to_lower(id);
    if (l.find("spotify") != std::string::npos) return "spotify";
    // "chrom" catches both chrome and chromium; the rest are the
    // browsers that actually ship MPRIS support.
    static const char* kBrowsers[] = {
        "chrom", "firefox", "librewolf", "waterfox", "floorp", "zen",
        "brave", "vivaldi", "opera", "edge", "yandex", "thorium",
        "epiphany", "midori", "qutebrowser", "webkit", "falkon"
    };
    for (const char* b : kBrowsers) {
        if (l.find(b) != std::string::npos) return "browser";
    }
    return "other";
}

// Fallback label when Identity can't be read. Browser instances show up
// as "chromium.instance10741" / "firefox.instance_1_2" — everything from
// the first dot on is bus bookkeeping, not something a user wants to see.
std::string prettify_id(const std::string& id) {
    std::string base = id.substr(0, id.find('.'));
    if (base.empty()) return id;
    std::replace(base.begin(), base.end(), '_', ' ');
    base[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(base[0])));
    return base;
}

int kind_rank(const std::string& kind) {
    if (kind == "spotify") return 0;
    if (kind == "browser") return 1;
    return 2;
}

void sort_players(std::vector<ExternalPlayer>& players) {
    std::stable_sort(players.begin(), players.end(),
                     [](const ExternalPlayer& a, const ExternalPlayer& b) {
                         int ra = kind_rank(a.kind), rb = kind_rank(b.kind);
                         if (ra != rb) return ra < rb;
                         return to_lower(a.display_name) < to_lower(b.display_name);
                     });
}

// ---------------------------------------------------------------------
// Unit conversion shared by every backend
// ---------------------------------------------------------------------

double micros_to_sec(double micros) {
    // MPRIS uses 0 for "no length" as often as it omits the key, and a
    // negative position is meaningless — both map to our -1 "unknown".
    if (!(micros > 0.0)) return -1.0;
    return micros / 1000000.0;
}

int volume_to_pct(double v) {
    if (!(v >= 0.0)) return -1;
    int pct = static_cast<int>(std::lround(v * 100.0));
    // Some players (Chromium among them) will happily report a Volume
    // above 1.0 after a boost; clamp rather than hand the UI a 137.
    return std::max(0, std::min(100, pct));
}

ExtStatus parse_status(const std::string& s) {
    const std::string l = to_lower(trim(s));
    if (l == "playing") return ExtStatus::Playing;
    if (l == "paused")  return ExtStatus::Paused;
    if (l == "stopped") return ExtStatus::Stopped;
    return ExtStatus::Unknown;
}

// ---------------------------------------------------------------------
// busctl backend
// ---------------------------------------------------------------------

void append_utf8(unsigned int cp, std::string& out) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool read_hex4(const std::string& s, size_t p, unsigned int& cp) {
    if (p + 4 > s.size()) return false;
    unsigned int v = 0;
    for (size_t k = 0; k < 4; ++k) {
        char c = s[p + k];
        unsigned int d;
        if (c >= '0' && c <= '9') d = static_cast<unsigned int>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<unsigned int>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<unsigned int>(c - 'A' + 10);
        else return false;
        v = v * 16 + d;
    }
    cp = v;
    return true;
}

// tiny_json.h deliberately doesn't implement \uXXXX (snapshot.json, the
// only thing it was written for, is written by the same parser and never
// contains one). busctl's JSON writer may escape non-ASCII, and track
// titles are exactly the place where non-ASCII shows up, so decode those
// escapes into real UTF-8 before handing the document to the parser.
std::string decode_json_unicode(const std::string& s) {
    if (s.find("\\u") == std::string::npos) return s;   // the overwhelmingly common case
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            unsigned int cp = 0;
            if (s[i + 1] == 'u' && read_hex4(s, i + 2, cp)) {
                i += 6;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    unsigned int lo = 0;
                    if (read_hex4(s, i + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        i += 6;
                    }
                }
                // Characters that mean something to the JSON grammar have
                // to stay escaped — writing a raw quote here would split
                // the very string we're trying to preserve.
                if (cp == '"') out += "\\\"";
                else if (cp == '\\') out += "\\\\";
                else if (cp < 0x20) out += ' ';
                else append_utf8(cp, out);
                continue;
            }
            out += s[i];
            out += s[i + 1];
            i += 2;
            continue;
        }
        out += s[i++];
    }
    return out;
}

// busctl wraps every value as {"type":"<sig>","data":<value>}. This peels
// that wrapper off; elements inside an "as" array are bare strings, so a
// value without the wrapper is returned untouched.
const tinyjson::Value* unwrap(const tinyjson::Value* v) {
    if (!v) return nullptr;
    if (v->type == tinyjson::Type::Object) {
        if (const tinyjson::Value* d = v->find("data")) return d;
    }
    return v;
}

const tinyjson::Value* dict_get(const tinyjson::Value* dict, const char* key) {
    if (!dict || dict->type != tinyjson::Type::Object) return nullptr;
    return unwrap(dict->find(key));
}

std::string json_str(const tinyjson::Value* v) {
    return v && v->type == tinyjson::Type::String ? v->str : std::string();
}

// 64-bit D-Bus integers (mpris:length, Position) are numbers in every
// busctl we've seen, but sd-bus's JSON writer has stringified them in
// the past to dodge IEEE754 precision loss — accept either.
double json_num(const tinyjson::Value* v, double def) {
    if (!v) return def;
    if (v->type == tinyjson::Type::Number) return v->num;
    if (v->type == tinyjson::Type::String) {
        try { return std::stod(v->str); } catch (...) { return def; }
    }
    return def;
}

bool json_bool(const tinyjson::Value* v, bool def) {
    return v && v->type == tinyjson::Type::Bool ? v->b : def;
}

// Joins xesam:artist (an "as") into one display string. A lone string is
// accepted too: a few players get the signature wrong.
std::string join_artists(const tinyjson::Value* v) {
    if (!v) return {};
    if (v->type == tinyjson::Type::String) return v->str;
    if (v->type != tinyjson::Type::Array) return {};
    std::string out;
    for (const tinyjson::Value& e : v->arr) {
        const tinyjson::Value* s = unwrap(&e);
        std::string part = trim(json_str(s));
        if (part.empty()) continue;
        if (!out.empty()) out += ", ";
        out += part;
    }
    return out;
}

std::string busctl_base() {
    return "busctl --user --timeout=" + std::to_string(kDbusTimeoutSecs) + " ";
}

// The `--` is load-bearing, not decoration. busctl parses its whole
// command line with getopt, so a negative method argument — which is
// precisely what a backwards Seek is — gets eaten as a flag:
//   busctl: unrecognized option '-5'
// Ending option scanning up front makes every argument positional and
// costs nothing for the calls that have no negative numbers in them.
std::string busctl_call(const std::string& bus, const char* iface, const std::string& member,
                        const std::string& args = {}) {
    std::string cmd = busctl_base() + "call -- " + shell_quote(bus) + " " + kMprisPath + " " + iface + " " + member;
    if (!args.empty()) cmd += " " + args;
    return guard(cmd);
}

ExtTrackState poll_busctl(const std::string& bus) {
    ExtTrackState st;
    std::string cmd = guard(busctl_base() + "--json=short call -- " + shell_quote(bus) + " " + kMprisPath +
                            " org.freedesktop.DBus.Properties GetAll s " + kPlayerIface);
    ProcResult r = run_capture(cmd);
    // A player that vanished between list_players() and here exits
    // non-zero with a ServiceUnknown on stderr. That's an expected
    // outcome, not an error worth surfacing — stderr is already
    // discarded by run_capture's default.
    if (!r.ok() || r.out.empty()) return st;

    tinyjson::Value root;
    if (!tinyjson::parse(decode_json_unicode(r.out), root)) return st;

    // GetAll's reply is a one-element tuple, which busctl renders as
    // {"type":"a{sv}","data":[{...the dict...}]}. Nested dictionaries
    // put their object straight in "data", so accept both shapes.
    const tinyjson::Value* data = root.find("data");
    if (!data) return st;
    const tinyjson::Value* props = data;
    if (data->type == tinyjson::Type::Array) {
        if (data->arr.empty()) return st;
        props = &data->arr[0];
    }
    if (props->type != tinyjson::Type::Object || props->obj.empty()) return st;

    st.valid = true;
    st.status       = parse_status(json_str(dict_get(props, "PlaybackStatus")));
    st.position_sec = micros_to_sec(json_num(dict_get(props, "Position"), -1.0));
    st.volume_pct   = volume_to_pct(json_num(dict_get(props, "Volume"), -1.0));
    st.can_go_next     = json_bool(dict_get(props, "CanGoNext"), false);
    st.can_go_previous = json_bool(dict_get(props, "CanGoPrevious"), false);
    st.can_pause       = json_bool(dict_get(props, "CanPause"), false);
    st.can_seek        = json_bool(dict_get(props, "CanSeek"), false);
    st.can_control     = json_bool(dict_get(props, "CanControl"), false);

    if (const tinyjson::Value* meta = dict_get(props, "Metadata")) {
        st.title    = json_str(dict_get(meta, "xesam:title"));
        st.album    = json_str(dict_get(meta, "xesam:album"));
        st.art_url  = json_str(dict_get(meta, "mpris:artUrl"));
        st.track_id = json_str(dict_get(meta, "mpris:trackid"));
        st.artist   = join_artists(dict_get(meta, "xesam:artist"));
        // Browsers routinely publish an empty xesam:artist; the channel
        // or uploader name, when there is one, lands in albumArtist.
        if (st.artist.empty()) st.artist = join_artists(dict_get(meta, "xesam:albumArtist"));
        st.length_sec = micros_to_sec(json_num(dict_get(meta, "mpris:length"), -1.0));
    }
    return st;
}

std::string identity_busctl(const std::string& bus) {
    std::string cmd = guard(busctl_base() + "--json=short call -- " + shell_quote(bus) + " " + kMprisPath +
                            " org.freedesktop.DBus.Properties Get ss " + kRootIface + " Identity");
    ProcResult r = run_capture(cmd);
    if (!r.ok()) return {};
    tinyjson::Value root;
    if (!tinyjson::parse(decode_json_unicode(r.out), root)) return {};
    const tinyjson::Value* data = root.find("data");
    if (!data || data->type != tinyjson::Type::Array || data->arr.empty()) return {};
    return trim(json_str(unwrap(&data->arr[0])));
}

std::vector<std::string> list_names_busctl() {
    // ListNames rather than `busctl list`, because the latter also
    // reports *activatable* names — services with a .service file that
    // aren't actually running. Spotify installs one, so `busctl list`
    // would show a Spotify player on a machine where Spotify is closed.
    std::string cmd = guard(busctl_base() + "--json=short call -- org.freedesktop.DBus /org/freedesktop/DBus "
                            "org.freedesktop.DBus ListNames");
    ProcResult r = run_capture(cmd);
    if (!r.ok() || r.out.empty()) {
        // Fall back to the column listing if ListNames didn't work; the
        // false positives above are better than no discovery at all.
        r = run_capture(guard("busctl --user list --no-pager --no-legend --acquired"));
        if (!r.ok()) return {};
    }
    return scrape_bus_names(r.out);
}

// ---------------------------------------------------------------------
// gdbus backend
//
// gdbus prints GVariant text rather than JSON, e.g.
//   ({'CanSeek': <true>, 'Metadata': <{'xesam:title': <'Song'>,
//     'mpris:length': <int64 194000000>}>, 'Position': <int64 3200000>},)
// so this needs a small hand-rolled reader. It only ever has to find a
// key at one nesting level and hand back the raw variant body, which is
// a lot less code than a general GVariant parser.
// ---------------------------------------------------------------------

// Extracts the text between the <> of `'key': <...>`, honouring nesting
// and ignoring anything inside quotes (a title containing "> " must not
// end the scan early).
bool gv_variant(const std::string& s, const std::string& key, std::string& out) {
    const std::string needle = "'" + key + "':";
    size_t k = s.find(needle);
    if (k == std::string::npos) return false;
    size_t open = s.find('<', k + needle.size());
    if (open == std::string::npos) return false;
    size_t i = open + 1;
    int depth = 1;
    bool in_str = false;
    while (i < s.size()) {
        char c = s[i];
        if (in_str) {
            if (c == '\\' && i + 1 < s.size()) { i += 2; continue; }
            if (c == '\'') in_str = false;
        } else if (c == '\'') {
            in_str = true;
        } else if (c == '<') {
            ++depth;
        } else if (c == '>') {
            if (--depth == 0) { out = s.substr(open + 1, i - open - 1); return true; }
        }
        ++i;
    }
    return false;
}

// First single-quoted run in a GVariant body. Also handles the typed
// forms gdbus emits for non-obvious types: `objectpath '/x'`, `@s 'x'`.
std::string gv_string(const std::string& body) {
    size_t i = body.find('\'');
    if (i == std::string::npos) return {};
    std::string out;
    for (++i; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()) { out += body[i + 1]; ++i; continue; }
        if (body[i] == '\'') break;
        out += body[i];
    }
    return out;
}

std::vector<std::string> gv_string_array(const std::string& body) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < body.size()) {
        if (body[i] != '\'') { ++i; continue; }
        std::string s;
        for (++i; i < body.size(); ++i) {
            if (body[i] == '\\' && i + 1 < body.size()) { s += body[i + 1]; ++i; continue; }
            if (body[i] == '\'') { ++i; break; }
            s += body[i];
        }
        out.push_back(std::move(s));
    }
    return out;
}

// Numbers arrive either bare (`1.0`) or type-tagged (`int64 194000000`,
// `uint32 5`) depending on whether gdbus can infer the signature from
// context. Taking the text after the last space peels the tag off
// without having to know the tag names — and crucially without eating
// the digits inside one: stripping "leading letters" turns `int64 5`
// into `64 5`, which parses as a perfectly plausible, perfectly wrong 64.
double gv_number(const std::string& body, double def) {
    std::string t = trim(body);
    size_t sp = t.find_last_of(" \t");
    if (sp != std::string::npos) t = t.substr(sp + 1);
    if (t.empty()) return def;
    try { return std::stod(t); } catch (...) { return def; }
}

bool gv_bool(const std::string& body, bool def) {
    std::string t = trim(body);
    if (t == "true") return true;
    if (t == "false") return false;
    return def;
}

std::string gdbus_base(const std::string& bus) {
    return "gdbus call --session --timeout " + std::to_string(kDbusTimeoutSecs) +
           " --dest " + shell_quote(bus) + " --object-path " + kMprisPath + " --method ";
}

ExtTrackState poll_gdbus(const std::string& bus) {
    ExtTrackState st;
    std::string cmd = guard(gdbus_base(bus) + "org.freedesktop.DBus.Properties.GetAll " + kPlayerIface);
    ProcResult r = run_capture(cmd);
    if (!r.ok() || r.out.empty()) return st;
    const std::string& s = r.out;

    std::string body;
    st.valid = true;
    if (gv_variant(s, "PlaybackStatus", body)) st.status = parse_status(gv_string(body));
    if (gv_variant(s, "Position", body))       st.position_sec = micros_to_sec(gv_number(body, -1.0));
    if (gv_variant(s, "Volume", body))         st.volume_pct = volume_to_pct(gv_number(body, -1.0));
    if (gv_variant(s, "CanGoNext", body))      st.can_go_next = gv_bool(body, false);
    if (gv_variant(s, "CanGoPrevious", body))  st.can_go_previous = gv_bool(body, false);
    if (gv_variant(s, "CanPause", body))       st.can_pause = gv_bool(body, false);
    if (gv_variant(s, "CanSeek", body))        st.can_seek = gv_bool(body, false);
    if (gv_variant(s, "CanControl", body))     st.can_control = gv_bool(body, false);

    std::string meta;
    if (gv_variant(s, "Metadata", meta)) {
        if (gv_variant(meta, "xesam:title", body))  st.title = gv_string(body);
        if (gv_variant(meta, "xesam:album", body))  st.album = gv_string(body);
        if (gv_variant(meta, "mpris:artUrl", body)) st.art_url = gv_string(body);
        if (gv_variant(meta, "mpris:trackid", body)) st.track_id = gv_string(body);
        if (gv_variant(meta, "mpris:length", body)) st.length_sec = micros_to_sec(gv_number(body, -1.0));
        auto join = [](const std::vector<std::string>& v) {
            std::string out;
            for (const std::string& e : v) {
                std::string part = trim(e);
                if (part.empty()) continue;
                if (!out.empty()) out += ", ";
                out += part;
            }
            return out;
        };
        if (gv_variant(meta, "xesam:artist", body)) st.artist = join(gv_string_array(body));
        if (st.artist.empty() && gv_variant(meta, "xesam:albumArtist", body)) st.artist = join(gv_string_array(body));
    }
    return st;
}

std::string identity_gdbus(const std::string& bus) {
    ProcResult r = run_capture(guard(gdbus_base(bus) + "org.freedesktop.DBus.Properties.Get " +
                                     kRootIface + " Identity"));
    if (!r.ok()) return {};
    return trim(gv_string(r.out));
}

std::vector<std::string> list_names_gdbus() {
    ProcResult r = run_capture(guard("gdbus call --session --timeout " + std::to_string(kDbusTimeoutSecs) +
                                     " --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus"
                                     " --method org.freedesktop.DBus.ListNames"));
    if (!r.ok()) return {};
    return scrape_bus_names(r.out);
}

// ---------------------------------------------------------------------
// playerctl backend
//
// playerctl speaks in short ids ("spotify", "chromium.instance10741"),
// which are exactly the MPRIS bus name minus the well-known prefix — so
// the mapping in both directions is just that prefix.
// ---------------------------------------------------------------------

// A byte that cannot appear in a track title, used to split one
// metadata call into fields. 0x1F is ASCII's "unit separator", which is
// what it was invented for.
constexpr char kFieldSep = '\x1f';

std::string playerctl_for(const std::string& bus) {
    return "playerctl -p " + shell_quote(short_id(bus)) + " ";
}

ExtTrackState poll_playerctl(const std::string& bus) {
    ExtTrackState st;
    std::string fmt;
    fmt += "{{status}}";        fmt += kFieldSep;
    fmt += "{{position}}";      fmt += kFieldSep;
    fmt += "{{mpris:length}}";  fmt += kFieldSep;
    fmt += "{{volume}}";        fmt += kFieldSep;
    fmt += "{{mpris:trackid}}"; fmt += kFieldSep;
    fmt += "{{title}}";         fmt += kFieldSep;
    fmt += "{{artist}}";        fmt += kFieldSep;
    fmt += "{{album}}";         fmt += kFieldSep;
    fmt += "{{mpris:artUrl}}";

    ProcResult r = run_capture(guard(playerctl_for(bus) + "metadata --format " + shell_quote(fmt)));
    if (!r.ok()) return st;

    std::vector<std::string> f;
    {
        std::string cur;
        for (char c : r.out) {
            if (c == kFieldSep) { f.push_back(cur); cur.clear(); continue; }
            if (c == '\n' || c == '\r') continue;
            cur += c;
        }
        f.push_back(cur);
    }
    if (f.size() < 9) return st;

    st.valid = true;
    st.status = parse_status(f[0]);
    // {{position}} and {{mpris:length}} are the raw MPRIS values, i.e.
    // microseconds — unlike the `playerctl position` subcommand, which
    // prints seconds. If the template didn't expand (older playerctl),
    // fall back to that subcommand rather than reporting no position.
    st.position_sec = micros_to_sec(trim(f[1]).empty() ? -1.0 : std::atof(f[1].c_str()));
    if (st.position_sec < 0.0) {
        ProcResult p = run_capture(guard(playerctl_for(bus) + "position"));
        if (p.ok() && !trim(p.out).empty()) {
            double sec = std::atof(p.out.c_str());
            if (sec >= 0.0) st.position_sec = sec;
        }
    }
    st.length_sec = micros_to_sec(trim(f[2]).empty() ? -1.0 : std::atof(f[2].c_str()));
    st.volume_pct = volume_to_pct(trim(f[3]).empty() ? -1.0 : std::atof(f[3].c_str()));
    st.track_id = trim(f[4]);
    st.title    = trim(f[5]);
    st.artist   = trim(f[6]);
    st.album    = trim(f[7]);
    st.art_url  = trim(f[8]);
    // playerctl's format templates expose metadata, not the Can*
    // properties, and spending four more subprocesses to fetch them
    // would cost more than it's worth. Assume permissive: a command the
    // player actually refuses just returns false at dispatch time,
    // which is the same information one frame later.
    st.can_control = st.can_pause = st.can_seek = true;
    st.can_go_next = st.can_go_previous = true;
    return st;
}

std::vector<std::string> list_names_playerctl() {
    ProcResult r = run_capture(guard("playerctl -l"));
    if (!r.ok()) return {};   // "No players found" exits non-zero
    std::vector<std::string> out;
    for (const std::string& line : split_lines(r.out)) {
        if (line.empty() || line.find(' ') != std::string::npos) continue;
        out.push_back(kMprisPrefix + line);
    }
    return out;
}

// ---------------------------------------------------------------------
// macOS / osascript backend
//
// There is no MPRIS on macOS and no session bus to discover players on,
// so the "discovery" here is a hardcoded probe of the two scriptable
// music apps. Browsers on macOS expose no comparable scripting API for
// media playback — Chrome's and Safari's AppleScript dictionaries cover
// tabs and URLs, not the media session — so a YouTube tab simply cannot
// be seen or controlled from here, and is deliberately not reported.
//
// `bus_name` on this platform carries the AppleScript application name
// ("Spotify", "Music") rather than a D-Bus name; nothing outside this
// file interprets it, so the contract still holds.
// ---------------------------------------------------------------------

#if defined(__APPLE__)

std::string osascript(const std::string& script) {
    ProcResult r = run_capture(guard("osascript -e " + shell_quote(script)));
    return r.ok() ? trim(r.out) : std::string();
}

bool app_running(const std::string& app) {
    std::string out = osascript("tell application \"System Events\" to return (exists process \"" + app + "\")");
    return to_lower(out) == "true";
}

// Spotify reports track duration in milliseconds; Music reports seconds.
bool duration_is_ms(const std::string& app) { return app == "Spotify"; }

ExtTrackState poll_osascript(const std::string& app) {
    ExtTrackState st;
    const std::string sep(1, kFieldSep);
    std::string script =
        "tell application \"" + app + "\"\n"
        "  set s to (player state as text)\n"
        "  set p to (player position)\n"
        "  set t to current track\n"
        "  return s & \"" + sep + "\" & p & \"" + sep + "\" & (name of t) & \"" + sep + "\" &"
        " (artist of t) & \"" + sep + "\" & (album of t) & \"" + sep + "\" & (duration of t) & \"" + sep + "\" &"
        " (sound volume)\n"
        "end tell";
    std::string out = osascript(script);
    if (out.empty()) return st;

    std::vector<std::string> f;
    std::string cur;
    for (char c : out) {
        if (c == kFieldSep) { f.push_back(cur); cur.clear(); continue; }
        if (c == '\n' || c == '\r') continue;
        cur += c;
    }
    f.push_back(cur);
    if (f.size() < 7) return st;

    st.valid = true;
    st.status = parse_status(f[0]);
    st.position_sec = std::atof(f[1].c_str());
    st.title = trim(f[2]);
    st.artist = trim(f[3]);
    st.album = trim(f[4]);
    double dur = std::atof(f[5].c_str());
    st.length_sec = duration_is_ms(app) ? (dur > 0 ? dur / 1000.0 : -1.0) : (dur > 0 ? dur : -1.0);
    // `sound volume` is already 0-100 here, not the MPRIS 0.0-1.0.
    st.volume_pct = volume_to_pct(std::atof(f[6].c_str()) / 100.0);
    st.can_control = st.can_pause = st.can_seek = true;
    st.can_go_next = st.can_go_previous = true;
    return st;
}

bool osascript_cmd(const std::string& app, const std::string& verb) {
    ProcResult r = run_capture(guard("osascript -e " + shell_quote("tell application \"" + app + "\" to " + verb)));
    return r.ok();
}

#endif // __APPLE__

} // namespace

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

bool ExternalControl::available() {
    return backend_info().backend != Backend::None;
}

const char* ExternalControl::backend_name() {
    switch (backend_info().backend) {
        case Backend::Playerctl: return "playerctl";
        case Backend::Busctl:    return "busctl";
        case Backend::Gdbus:     return "gdbus";
        case Backend::Osascript: return "osascript";
        case Backend::None:      break;
    }
    return "none";
}

std::vector<ExternalPlayer> ExternalControl::list_players() {
    std::vector<ExternalPlayer> players;
    const Backend backend = backend_info().backend;
    if (backend == Backend::None) return players;

#if defined(__APPLE__)
    if (backend == Backend::Osascript) {
        for (const char* app : {"Spotify", "Music"}) {
            if (!app_running(app)) continue;
            ExternalPlayer p;
            p.bus_name = app;
            p.id = to_lower(app);
            p.display_name = app;
            p.kind = classify_kind(p.id);
            players.push_back(std::move(p));
        }
        sort_players(players);
        return players;
    }
#endif

    std::vector<std::string> names;
    switch (backend) {
        case Backend::Playerctl: names = list_names_playerctl(); break;
        case Backend::Busctl:    names = list_names_busctl();    break;
        case Backend::Gdbus:     names = list_names_gdbus();     break;
        default: return players;
    }

    for (const std::string& bus : names) {
        ExternalPlayer p;
        p.bus_name = bus;
        p.id = short_id(bus);
        p.kind = classify_kind(p.id);
        // Identity is a nicety, not a requirement: it costs an extra
        // round trip per player, which is fine because discovery runs on
        // a user action or a slow timer, not per frame. playerctl has no
        // way to ask for it, so that backend always uses the fallback.
        if (backend == Backend::Busctl) p.display_name = identity_busctl(bus);
        else if (backend == Backend::Gdbus) p.display_name = identity_gdbus(bus);
        if (p.display_name.empty()) p.display_name = prettify_id(p.id);
        players.push_back(std::move(p));
    }

    sort_players(players);
    return players;
}

ExtTrackState ExternalControl::poll(const std::string& bus_name) {
    ExtTrackState st;
    if (bus_name.empty()) return st;
    switch (backend_info().backend) {
        case Backend::Playerctl: return poll_playerctl(bus_name);
        case Backend::Busctl:    return poll_busctl(bus_name);
        case Backend::Gdbus:     return poll_gdbus(bus_name);
        case Backend::Osascript:
#if defined(__APPLE__)
            return poll_osascript(bus_name);
#else
            return st;
#endif
        case Backend::None: break;
    }
    return st;
}

namespace {

// Everything except Seek/SetPosition/Volume is a no-argument method on
// org.mpris.MediaPlayer2.Player, so all six transport commands collapse
// into one dispatcher per backend.
bool simple_command(const std::string& bus, const char* mpris_method, const char* playerctl_verb,
                    const char* applescript_verb) {
    if (bus.empty()) return false;
    switch (backend_info().backend) {
        case Backend::Playerctl:
            return run_capture(guard(playerctl_for(bus) + playerctl_verb)).ok();
        case Backend::Busctl:
            return run_capture(busctl_call(bus, kPlayerIface, mpris_method)).ok();
        case Backend::Gdbus:
            return run_capture(guard(gdbus_base(bus) + kPlayerIface + "." + mpris_method)).ok();
        case Backend::Osascript:
#if defined(__APPLE__)
            return applescript_verb && *applescript_verb && osascript_cmd(bus, applescript_verb);
#else
            (void)applescript_verb;
            return false;
#endif
        case Backend::None: break;
    }
    return false;
}

// MPRIS measures every time in microseconds. Rounding rather than
// truncating keeps a 5.0s step from becoming 4999999µs.
long long to_micros(double sec) {
    return static_cast<long long>(std::llround(sec * 1000000.0));
}

} // namespace

bool ExternalControl::play_pause(const std::string& bus_name) {
    return simple_command(bus_name, "PlayPause", "play-pause", "playpause");
}

bool ExternalControl::play(const std::string& bus_name) {
    return simple_command(bus_name, "Play", "play", "play");
}

bool ExternalControl::pause(const std::string& bus_name) {
    return simple_command(bus_name, "Pause", "pause", "pause");
}

bool ExternalControl::stop(const std::string& bus_name) {
    // Neither Spotify nor Music has an AppleScript "stop", so pause is
    // the closest honest equivalent there.
    return simple_command(bus_name, "Stop", "stop", "pause");
}

bool ExternalControl::next(const std::string& bus_name) {
    return simple_command(bus_name, "Next", "next", "next track");
}

bool ExternalControl::previous(const std::string& bus_name) {
    return simple_command(bus_name, "Previous", "previous", "previous track");
}

// Observed on Chromium/Chrome: Seek is honoured, but the offset is not.
// Chrome routes it through the media session's "seek forward/backward"
// action, which steps a fixed ~5s no matter what you ask for — a +60s
// Seek measured as +5.15s. SetPosition, by contrast, lands exactly. So
// if the UI wants an accurate jump on a browser, poll() first and use
// set_position() with the reported track_id rather than this.
bool ExternalControl::seek_relative(const std::string& bus_name, double delta_sec) {
    if (bus_name.empty() || delta_sec == 0.0) return false;
    const long long micros = to_micros(delta_sec);
    switch (backend_info().backend) {
        case Backend::Playerctl: {
            // playerctl's relative form is a magnitude with a trailing
            // sign character, not a signed number.
            double mag = delta_sec < 0 ? -delta_sec : delta_sec;
            std::string arg = std::to_string(mag) + (delta_sec < 0 ? "-" : "+");
            return run_capture(guard(playerctl_for(bus_name) + "position " + shell_quote(arg))).ok();
        }
        case Backend::Busctl:
            // The "x" is the signature: Seek takes a signed int64.
            return run_capture(busctl_call(bus_name, kPlayerIface, "Seek", "x " + std::to_string(micros))).ok();
        case Backend::Gdbus:
            // gdbus infers int32 from a bare number, which Seek rejects
            // with a signature mismatch — the type tag is mandatory.
            return run_capture(guard(gdbus_base(bus_name) + kPlayerIface + ".Seek " +
                                     shell_quote("int64 " + std::to_string(micros)))).ok();
        case Backend::Osascript:
#if defined(__APPLE__)
            return osascript_cmd(bus_name, "set player position to (player position + " +
                                           std::to_string(delta_sec) + ")");
#else
            return false;
#endif
        case Backend::None: break;
    }
    return false;
}

bool ExternalControl::set_position(const std::string& bus_name, const std::string& track_id, double pos_sec) {
    if (bus_name.empty() || pos_sec < 0.0) return false;
    const long long micros = to_micros(pos_sec);
    switch (backend_info().backend) {
        case Backend::Playerctl:
            // playerctl takes absolute seconds and resolves the track id
            // itself, so track_id is unused on this path.
            return run_capture(guard(playerctl_for(bus_name) + "position " +
                                     shell_quote(std::to_string(pos_sec)))).ok();
        case Backend::Busctl:
            // SetPosition(o track_id, x position). MPRIS requires the
            // caller to name the track so a seek raced against a track
            // change is dropped instead of applied to the wrong song —
            // which is exactly why the trackid is in ExtTrackState.
            if (track_id.empty()) return false;
            return run_capture(busctl_call(bus_name, kPlayerIface, "SetPosition",
                                           "ox " + shell_quote(track_id) + " " + std::to_string(micros))).ok();
        case Backend::Gdbus:
            if (track_id.empty()) return false;
            return run_capture(guard(gdbus_base(bus_name) + kPlayerIface + ".SetPosition " +
                                     shell_quote("objectpath '" + track_id + "'") + " " +
                                     shell_quote("int64 " + std::to_string(micros)))).ok();
        case Backend::Osascript:
#if defined(__APPLE__)
            return osascript_cmd(bus_name, "set player position to " + std::to_string(pos_sec));
#else
            return false;
#endif
        case Backend::None: break;
    }
    return false;
}

// Returning true here means "the player accepted the write", not "the
// volume changed". Chromium exposes Volume read/write and answers the
// property set with success, then ignores it — its MPRIS volume is
// pinned at 1.0 and the real control is the tab's own mixer stream.
// Spotify does honour it. Callers should re-poll rather than assume.
bool ExternalControl::set_volume(const std::string& bus_name, int volume_pct) {
    if (bus_name.empty()) return false;
    volume_pct = std::max(0, std::min(100, volume_pct));
    // MPRIS Volume is a double where 1.0 is "normal". Printed with a
    // fixed two decimals so it always looks like a double to the
    // GVariant parsers — "1" would be read as an integer and rejected.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", volume_pct / 100.0);
    const std::string dbl = buf;

    switch (backend_info().backend) {
        case Backend::Playerctl:
            return run_capture(guard(playerctl_for(bus_name) + "volume " + shell_quote(dbl))).ok();
        case Backend::Busctl:
            // Volume is a property, so this is set-property, not call.
            return run_capture(guard(busctl_base() + "set-property -- " + shell_quote(bus_name) + " " + kMprisPath +
                                     " " + kPlayerIface + " Volume d " + dbl)).ok();
        case Backend::Gdbus:
            return run_capture(guard(gdbus_base(bus_name) + "org.freedesktop.DBus.Properties.Set " +
                                     kPlayerIface + " Volume " + shell_quote("<double " + dbl + ">"))).ok();
        case Backend::Osascript:
#if defined(__APPLE__)
            return osascript_cmd(bus_name, "set sound volume to " + std::to_string(volume_pct));
#else
            return false;
#endif
        case Backend::None: break;
    }
    return false;
}

const char* ext_status_name(ExtStatus s) {
    switch (s) {
        case ExtStatus::Playing: return "Playing";
        case ExtStatus::Paused:  return "Paused";
        case ExtStatus::Stopped: return "Stopped";
        case ExtStatus::Unknown: break;
    }
    return "Unknown";
}

} // namespace muisc
