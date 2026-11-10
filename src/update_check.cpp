// The update check: asks the releases page once a day in the background and remembers the answer, so
// the new-version notice is shown once per version.
#include "bg_tasks.h"
#include "update_check.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "console_log.h"
#include "process_util.h"
#include "settings.h"
#include "tiny_json.h"
#include "time_util.h"
#include "version.h"
#include "path_utf8.h"
#if defined(_WIN32)
#include "win_compat.h"
#endif

namespace muisc::update_check {

namespace fs = std::filesystem;

namespace {

std::mutex g_mu;
State g_state;
bool g_have = false;          // g_state holds a usable answer
bool g_announced = false;     // take_notice() has handed the text out
std::atomic<bool> g_started{false};
// The manual check ("Check Update" in Settings): it runs in its own thread and leaves its answer here for the screen.
std::atomic<bool> g_check_running{false};
std::mutex g_check_mu;
CheckResult g_check_result;
bool g_check_ready = false;

void save_state(const State& s);

// Writes the answer of a check to the stored state (the same record the start-up check keeps).
void remember_latest(const std::string& tag, const std::string& url) {
    State st;
    st.checked = epoch_now();
    st.latest = tag;
    st.url = url.compare(0, 8, "https://") == 0 ? url : std::string();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        st.notified = g_state.notified;
        g_state = st;
        g_have = true;
    }
    save_state(st);
}

// The manual check: asks the same script and turns every failure into a sentence the settings screen can show.
void run_check(fs::path script) {
    CheckResult res;
    res.current = MOUSIKI_VERSION;
    try {
        Args cmd = python_command();
        if (cmd.empty()) {
            res.error = "Python 3 was not found";
        } else {
            cmd.push_back(muisc::path_utf8(script));
            const ProcResult r = run_capture(cmd, /*merge_stderr=*/false);
            tinyjson::Value answer;
            if (!tinyjson::parse_answer(r.out, answer)) {
                res.error = "the check script gave no answer";
            } else if (!tinyjson::bool_field(answer, "ok")) {
                const std::string why = tinyjson::string_field(answer, "error");
                if (why == "NETWORK") res.error = "GitHub could not be reached (" + tinyjson::string_field(answer, "detail") + ")";
                else if (why == "MODULE_MISSING") res.error = "the lyrics helper module is missing -- reinstall Mousiki";
                else if (why == "NO_RELEASE") res.error = "GitHub lists no release";
                else res.error = "the check failed";
            } else {
                res.ok = true;
                res.latest = tinyjson::string_field(answer, "tag");
                res.url = tinyjson::string_field(answer, "url");
                res.newer = is_newer(res.latest, MOUSIKI_VERSION);
                remember_latest(res.latest, res.url);
            }
        }
    } catch (...) {
        res.error = "the check failed";
    }
    {
        std::lock_guard<std::mutex> lk(g_check_mu);
        g_check_result = res;
        g_check_ready = true;
    }
    g_check_running = false;
}

fs::path state_file() { return config_path().parent_path() / "update_check.txt"; }

// "v4.5.0" / "4.5.0-beta" -> {4,5,0}; empty when no number is found
std::vector<int> parse_version(const std::string& v) {
    std::vector<int> out;
    size_t i = 0;
    while (i < v.size() && (v[i] == 'v' || v[i] == 'V' || v[i] == ' ')) ++i;
    while (i < v.size()) {
        if (v[i] < '0' || v[i] > '9') break;
        int n = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') { n = n * 10 + (v[i] - '0'); if (n > 100000) n = 100000; ++i; }
        out.push_back(n);
        if (i < v.size() && v[i] == '.') ++i; else break;
    }
    return out;
}

void save_state(const State& s) {
    std::error_code ec;
    fs::create_directories(state_file().parent_path(), ec);
    std::ofstream f(state_file(), std::ios::binary | std::ios::trunc);
    if (f) f << state_to_text(s);
}

void worker(fs::path script, State st, bool ask_network) {
    try {
        if (ask_network) {
            Args cmd = python_command();
            if (cmd.empty()) return;
            cmd.push_back(muisc::path_utf8(script));
            const ProcResult r = run_capture(cmd, /*merge_stderr=*/false);
            tinyjson::Value answer;
            if (r.exit_code != 0 || !tinyjson::parse_answer(r.out, answer) || !tinyjson::bool_field(answer, "ok")) return;
            const std::string tag = tinyjson::string_field(answer, "tag");
            if (parse_version(tag).empty()) return;
            st.latest = tag;
            st.url = tinyjson::string_field(answer, "url");
            st.checked = epoch_now();
            if (st.url.compare(0, 8, "https://") != 0) st.url.clear();
            std::lock_guard<std::mutex> lk(g_mu);
            st.notified = g_state.notified;
            g_state = st;
            g_have = true;
            save_state(st);
        }
    } catch (...) {
        // a failed check is never an error for the program
    }
}

} // namespace

bool is_newer(const std::string& latest, const std::string& current) {
    const std::vector<int> a = parse_version(latest), b = parse_version(current);
    if (a.empty() || b.empty()) return false;
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

std::string state_to_text(const State& s) {
    std::ostringstream o;
    o << "checked=" << s.checked << "\nlatest=" << s.latest << "\nurl=" << s.url << "\nnotified=" << s.notified << "\n";
    return o.str();
}

State state_from_text(const std::string& text) {
    State s;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "checked") s.checked = std::atoll(v.c_str());
        else if (k == "latest") s.latest = v.substr(0, 40);
        else if (k == "url") s.url = v.substr(0, 200);
        else if (k == "notified") s.notified = v.substr(0, 40);
    }
    return s;
}

std::string notice_text(const std::string& latest, const std::string& current, const std::string& url) {
    std::string v = latest;
    while (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    return "Mousiki " + v + " is out (you have " + current + "): " +
           (url.empty() ? std::string("https://github.com/StSchwerdtfeger/mousiki-plus/releases/latest") : url) +
           "   (shown once)";
}

void start(const fs::path& script) {
    if (g_started.exchange(true)) return;
    if (!read_update_check()) return;
    State st;
    {
        std::ifstream f(state_file(), std::ios::binary);
        if (f) { std::stringstream ss; ss << f.rdbuf(); st = state_from_text(ss.str()); }
    }
    const long long age = epoch_now() - st.checked;
    const bool fresh = st.checked > 0 && age >= 0 && age < 24LL * 3600;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_state = st;
        g_have = !st.latest.empty();
    }
    if (fresh) return;   // asked within the last day: the stored answer is used, no network
    bg_async([script, st] { worker(script, st, true); return 0; });
}

bool check_now(const fs::path& script) {
    if (g_check_running.exchange(true)) return false;
    bg_async([script] { run_check(script); return 0; });
    return true;
}

bool check_busy() { return g_check_running.load(); }

bool take_check_result(CheckResult& out) {
    std::lock_guard<std::mutex> lk(g_check_mu);
    if (!g_check_ready) return false;
    out = g_check_result;
    g_check_ready = false;
    return true;
}

bool take(Notice& out) {
    if (!g_started.load()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_announced || !g_have) return false;
    if (!is_newer(g_state.latest, MOUSIKI_VERSION) || g_state.latest == g_state.notified) return false;
    g_announced = true;
    g_state.notified = g_state.latest;
    save_state(g_state);
    out.latest = g_state.latest;
    out.current = MOUSIKI_VERSION;
    out.url = g_state.url;
    return true;
}

std::string take_notice() {
    Notice n;
    return take(n) ? notice_text(n.latest, n.current, n.url) : std::string();
}

std::string release_url(const Notice& n) {
    return n.url.compare(0, 8, "https://") == 0 ? n.url : std::string("https://github.com/StSchwerdtfeger/mousiki-plus/releases/latest");
}

} // namespace muisc::update_check
