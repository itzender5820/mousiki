#include "cli_core.h"
#include "settings.h"
#include "path_utf8.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace muisc::cli {

namespace fs = std::filesystem;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split_chain(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (char c : line) {
        if (c == '"') q = !q;
        if (c == ';' && !q) { const std::string t = trim(cur); if (!t.empty()) out.push_back(t); cur.clear(); continue; }
        cur += c;
    }
    const std::string t = trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false, any = false;
    for (char c : s) {
        if (c == '"') { q = !q; any = true; continue; }
        if ((c == ' ' || c == '\t') && !q) { if (any || !cur.empty()) out.push_back(cur); cur.clear(); any = false; continue; }
        cur += c;
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

std::string unquote(const std::string& s) {
    const std::string t = trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
    return t;
}

bool parse_duration(const std::string& in, double& sec, double max_sec) {
    const std::string s = trim(in);
    if (s.empty()) return false;
    double total = 0, num = 0;
    bool have_num = false, any_unit = false;
    size_t i = 0;
    while (i < s.size()) {
        char* end = nullptr;
        const double v = std::strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) return false;
        num = v; have_num = true;
        i = static_cast<size_t>(end - s.c_str());
        if (i < s.size() && (s[i] == 's' || s[i] == 'm' || s[i] == 'h')) {
            total += num * (s[i] == 's' ? 1.0 : s[i] == 'm' ? 60.0 : 3600.0);
            any_unit = true; have_num = false; ++i;
        } else if (i < s.size()) return false;
    }
    if (have_num) { if (any_unit) return false; total += num; }   // "90" is seconds; "1h30" is not allowed
    if (!(total > 0) || total > max_sec) return false;
    sec = total;
    return true;
}

std::string duration_text(double sec) {
    long long t = static_cast<long long>(sec + 0.5);
    char b[48];
    if (t >= 3600) std::snprintf(b, sizeof b, "%lldh%02lldm%02llds", t / 3600, (t / 60) % 60, t % 60);
    else if (t >= 60) std::snprintf(b, sizeof b, "%lldm%02llds", t / 60, t % 60);
    else std::snprintf(b, sizeof b, "%llds", t);
    return b;
}

fs::path dir() { return config_path().parent_path() / "cli"; }

bool valid_name(const std::string& n) {
    if (n.empty() || n.size() > 48 || n[0] == '.') return false;
    for (char c : n) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
    return n.find("..") == std::string::npos;
}

fs::path script_path(const std::string& name) { return dir() / (name + ".cli"); }

std::vector<std::string> script_names() {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".cli") continue;
        const std::string n = path_utf8(it->path().stem());
        if (n != "aliases" && valid_name(n)) out.push_back(n);   // (aliases.cli is the alias list, not a script)
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool read_script(const std::string& name, std::vector<std::string>& lines, std::string& error) {
    lines.clear();
    if (!valid_name(name)) { error = "not a script name: " + name; return false; }
    std::ifstream f(script_path(name), std::ios::binary);
    if (!f) { error = "no script called \"" + name + "\" (scripts: " + path_utf8(dir()) + ")"; return false; }
    std::string l;
    while (std::getline(f, l) && lines.size() < 500) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.size() > 600) l.resize(600);
        lines.push_back(l);
    }
    return true;
}

bool write_script(const std::string& name, const std::vector<std::string>& lines, std::string& error) {
    if (!valid_name(name)) { error = "a script name has letters, digits, - _ . only"; return false; }
    std::error_code ec;
    fs::create_directories(dir(), ec);
    const fs::path p = script_path(name);
    fs::path tmp = p; tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { error = "cannot write " + path_utf8(p); return false; }
        for (const std::string& l : lines) f << l << "\n";
        if (!f) { error = "cannot write " + path_utf8(p); return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); error = "cannot write " + path_utf8(p); return false; }
    return true;
}

bool rename_script(const std::string& from, const std::string& to, std::string& error) {
    if (!valid_name(from) || !valid_name(to)) { error = "a script name has letters, digits, - _ . only"; return false; }
    std::error_code ec;
    if (!fs::exists(script_path(from), ec)) { error = "no script called \"" + from + "\""; return false; }
    if (fs::exists(script_path(to), ec)) { error = "\"" + to + "\" exists already"; return false; }
    fs::rename(script_path(from), script_path(to), ec);
    if (ec) { error = "could not rename it"; return false; }
    return true;
}

bool trash_script(const std::string& name, std::string& error) {
    if (!valid_name(name)) { error = "not a script name"; return false; }
    std::error_code ec;
    if (!fs::exists(script_path(name), ec)) { error = "no script called \"" + name + "\""; return false; }
    const fs::path trash = dir() / "deleted";
    fs::create_directories(trash, ec);
    fs::path to = trash / (name + ".cli");
    for (int i = 1; fs::exists(to, ec) && i < 1000; ++i) to = trash / (name + "." + std::to_string(i) + ".cli");
    fs::rename(script_path(name), to, ec);
    if (ec) { error = "could not move it to " + path_utf8(trash); return false; }
    return true;
}

bool script_matches(const std::string& name, const std::string& text, const std::string& query) {
    auto lower = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    const std::string hay = lower(name) + "\n" + lower(text);
    for (const std::string& w : split_words(lower(query))) if (!w.empty() && hay.find(w) == std::string::npos) return false;
    return true;
}

std::map<std::string, std::string> load_aliases() {
    std::map<std::string, std::string> out;
    std::ifstream f(dir() / "aliases.cli", std::ios::binary);
    std::string l;
    while (std::getline(f, l)) {
        l = trim(l);
        if (l.empty() || l[0] == '#') continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        if (valid_name(k) && !v.empty()) out[k] = v;
    }
    return out;
}

bool save_aliases(const std::map<std::string, std::string>& a) {
    std::error_code ec;
    fs::create_directories(dir(), ec);
    const fs::path p = dir() / "aliases.cli";
    fs::path tmp = p; tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << "# aliases of the Mousiki command line: name = command; command\n";
        for (const auto& [k, v] : a) f << k << " = " << v << "\n";
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

bool eval_condition(const std::string& in, const State& st, bool& ok) {
    ok = true;
    std::string c = trim(in);
    bool neg = false;
    for (;;) {
        if (!c.empty() && c[0] == '!') { neg = !neg; c = trim(c.substr(1)); continue; }
        if (c.size() > 4 && c.compare(0, 4, "not ") == 0) { neg = !neg; c = trim(c.substr(4)); continue; }
        break;
    }
    std::string l;
    for (char ch : c) l += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    bool r = false;
    if (l == "playing") r = st.has_track && !st.paused;
    else if (l == "paused") r = st.has_track && st.paused;
    else if (l == "stopped" || l == "idle") r = !st.has_track;
    else if (l == "muted") r = st.muted;
    else if (l == "unmuted") r = !st.muted;
    else if (l == "queue-empty") r = st.queue == 0;
    else if (l == "queue-nonempty" || l == "queue") r = st.queue > 0;
    else {
        const size_t op = l.find_first_of("<>=");
        if (op == std::string::npos || op + 1 >= l.size()) { ok = false; return false; }
        const std::string what = trim(l.substr(0, op)), num = trim(l.substr(op + 1));
        char* end = nullptr;
        const double v = std::strtod(num.c_str(), &end);
        if (end == num.c_str() || *end) { ok = false; return false; }
        double have;
        if (what == "volume") have = st.volume;
        else if (what == "queue") have = static_cast<double>(st.queue);
        else if (what == "position") have = st.position;
        else if (what == "duration") have = st.duration;
        else { ok = false; return false; }
        r = l[op] == '>' ? have > v : l[op] == '<' ? have < v : have == v;
    }
    return neg ? !r : r;
}

std::string script_line(const std::string& raw, bool& soft) {
    soft = false;
    std::string l = trim(raw);
    if (l.empty() || l[0] == '#') return "";
    if (l[0] == '-') { soft = true; l = trim(l.substr(1)); }
    return l;
}


// ---- hooks ------------------------------------------------------------------------------------------------------------------
bool is_alias_definition(const std::string& line) {
    const std::string l = trim(line);
    if (l.size() < 7 || std::tolower(static_cast<unsigned char>(l[0])) != 'a' || std::tolower(static_cast<unsigned char>(l[1])) != 'l' ||
        std::tolower(static_cast<unsigned char>(l[2])) != 'i' || std::tolower(static_cast<unsigned char>(l[3])) != 'a' ||
        std::tolower(static_cast<unsigned char>(l[4])) != 's' || (l[5] != ' ' && l[5] != '\t')) return false;
    return l.find('=') != std::string::npos;
}

const std::vector<std::string>& hook_events(bool radio) {
    static const std::vector<std::string> p = {"track-change", "pause", "resume", "queue-empty", "idle"};
    static const std::vector<std::string> r = {"station-change", "pause", "resume", "stream-error", "idle"};
    return radio ? r : p;
}

std::vector<Hook> parse_hooks(const std::vector<std::string>& lines, bool radio) {
    std::vector<Hook> out;
    for (const std::string& raw : lines) {
        std::string l = trim(raw);
        if (l.empty() || l[0] == '#') continue;
        if (l.size() < 3 || std::tolower(static_cast<unsigned char>(l[0])) != 'o' || std::tolower(static_cast<unsigned char>(l[1])) != 'n' || (l[2] != ' ' && l[2] != '\t')) continue;
        l = trim(l.substr(3));
        size_t sp = l.find_first_of(" \t=");
        if (sp == std::string::npos) continue;
        std::string ev = l.substr(0, sp);
        for (char& c : ev) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string rest = trim(l.substr(sp));
        if (!rest.empty() && rest[0] == '=') rest = trim(rest.substr(1));
        bool known = false;
        for (const std::string& k : hook_events(radio)) if (k == ev) known = true;
        if (!known || rest.empty()) continue;
        out.push_back({ev, rest});
    }
    return out;
}

std::string hook_safe_value(const std::string& v) {
    std::string r;
    for (unsigned char c : v) {
        if (c < 32 || c == 127 || c == ';') { r += c == ';' ? ',' : ' '; continue; }
        r += static_cast<char>(c);
        if (r.size() >= 120) break;
    }
    return trim(r);
}

std::string expand_hook(const std::string& chain, const std::map<std::string, std::string>& values) {
    std::string out;
    for (size_t i = 0; i < chain.size();) {
        if (chain[i] == '{') {
            const size_t e = chain.find('}', i);
            if (e != std::string::npos) {
                const auto it = values.find(chain.substr(i + 1, e - i - 1));
                if (it != values.end()) { out += hook_safe_value(it->second); i = e + 1; continue; }
            }
        }
        out += chain[i++];
    }
    return out;
}

bool HookLimiter::allow(std::chrono::steady_clock::time_point now, int max_fires, int window_sec) {
    while (!fires_.empty() && now - fires_.front() > std::chrono::seconds(window_sec)) fires_.pop_front();
    if (static_cast<int>(fires_.size()) >= max_fires) return false;
    fires_.push_back(now);
    return true;
}

}  // namespace muisc::cli
