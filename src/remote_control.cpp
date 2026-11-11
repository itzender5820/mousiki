// The remote control's command channel: checks command names and arguments, and passes commands
// between the running program and mousiki --cmd calls through small files in its control folder,
// written atomically.
#include "remote_control.h"
#include <cmath>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>
#if defined(_WIN32)
#include <process.h>
#define MOUSIKI_GETPID _getpid
#else
#include <unistd.h>
#define MOUSIKI_GETPID getpid
#endif

#include "media_session.h"
#include "remote_server.h"
#include "settings.h"
#include "version.h"
#include "path_utf8.h"
#include "cli_core.h"
#include <map>
#include <thread>

namespace muisc::remote {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

const char* const kCommands[] = {"play-pause", "play", "pause", "stop", "next", "prev", "shuffle", "volume-up", "volume-down",
                                 "volume", "mute", "unmute", "toggle-mute", "seek", "live", "playback-mode", "wizard", "switch-mode", "quit",
                                 "playlist", "tune", "remote-view", "remote-scroll", "remote-focus", "track", "clear-queue", "book-open"};

bool known(const std::string& n) {
    for (const char* c : kCommands) if (n == c) return true;
    return false;
}

bool arg_ok(const std::string& a) {
    if (a.size() > 12) return false;
    for (char c : a) if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) return false;
    return true;
}

fs::path running_file() { return dir() / "running"; }
fs::path status_file() { return dir() / "status"; }

// Writes `text` to a temporary name and renames it over `target`: a reader never sees half a file.
bool write_atomic(const fs::path& target, const std::string& text) {
    std::error_code ec;
    fs::path tmp = target;
    tmp += ".tmp";   // appended to the native path: no round trip through the ANSI code page
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

int g_client_serial = 0;
Clock::time_point g_last_running{};
Clock::time_point g_last_poll{};
Clock::time_point g_last_status{};
bool g_started = false;

void write_running(const char* mode) {
    write_atomic(running_file(), std::to_string(static_cast<long long>(MOUSIKI_GETPID())) + "\n" + mode + "\n");
    g_last_running = Clock::now();
}

} // namespace

fs::path dir() { return config_path().parent_path() / "control"; }

std::vector<std::string> known_commands() { return std::vector<std::string>(std::begin(kCommands), std::end(kCommands)); }

double number(const std::string& arg, double fallback) {
    if (arg.empty()) return fallback;
    char* end = nullptr;
    const double v = std::strtod(arg.c_str(), &end);
    // "nan", "inf" or a 12-digit number would make the int casts of the callers undefined: a sane range only
    if (end == arg.c_str() || !std::isfinite(v)) return fallback;
    return std::clamp(v, -1.0e6, 1.0e6);
}

void start(const char* mode) {
    std::error_code ec;
    fs::create_directories(dir(), ec);
    // commands left over from an earlier run must not fire now
    for (fs::directory_iterator it(dir(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string fn = path_utf8(it->path().filename());
        if (it->path().extension() == ".cmd" || fn.rfind("reply-", 0) == 0) { std::error_code e2; fs::remove(it->path(), e2); }
    }
    write_running(mode);
    g_last_poll = g_last_status = Clock::time_point{};
    g_started = true;
    media::start();   // the system's media controls (no-op when off / unavailable; stays up across a mode switch)
}

void stop() {
    if (!g_started) return;
    std::error_code ec;
    fs::remove(running_file(), ec);
    g_started = false;
}

void tick(const char* mode) {
    if (!g_started) return;
    if (Clock::now() - g_last_running >= std::chrono::seconds(2)) write_running(mode);
}

namespace {
bool id_ok(const std::string& id) {
    if (id.empty() || id.size() > 48) return false;
    for (char c : id) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
    return true;
}
}  // namespace

void reply(const std::string& id, bool ok, const std::string& text) {
    if (id.compare(0, 4, "net-") == 0) { net::deliver_reply(id, ok, text); return; }   // an HTTP request waits for it
    if (!g_started || !id_ok(id)) return;
    write_atomic(dir() / ("reply-" + id + ".txt"), std::string(ok ? "ok\n" : "error\n") + text + (text.empty() || text.back() == '\n' ? "" : "\n"));
}

std::vector<Command> poll() {
    std::vector<Command> out;
    if (!g_started) return out;
    for (Command& c : net::take_commands()) out.push_back(std::move(c));   // the remote app (network), not throttled
    for (Command& c : media::poll()) out.push_back(std::move(c));          // media keys / desktop widgets, not throttled
    const auto now = Clock::now();
    if (now - g_last_poll < std::chrono::milliseconds(100)) return out;
    g_last_poll = now;
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir(), ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".cmd") files.push_back(it->path());
    if (files.empty()) return out;
    std::sort(files.begin(), files.end());
    int taken = 0;
    for (const fs::path& p : files) {
        std::string line;
        {
            std::ifstream f(p, std::ios::binary);
            std::getline(f, line, '\n');
        }
        std::error_code e2;
        fs::remove(p, e2);
        if (++taken > 16) continue;   // a flood is dropped, not queued
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("cli ", 0) == 0 && line.size() <= 700) {   // "cli <id or -> <the command line>": text, answered through a reply file
            std::istringstream cs(line.substr(4));
            Command t;
            t.name = "cli";
            cs >> t.id;
            std::getline(cs, t.arg);
            while (!t.arg.empty() && t.arg.front() == ' ') t.arg.erase(t.arg.begin());
            for (char& ch : t.arg) if (static_cast<unsigned char>(ch) < 32) ch = ' ';
            if (t.id == "-") t.id.clear();
            if (!t.id.empty() && !id_ok(t.id)) t.id.clear();
            if (!t.arg.empty()) out.push_back(t);
            continue;
        }
        if (line.size() > 40) continue;
        std::istringstream ss(line);
        Command c;
        ss >> c.name >> c.arg;
        if (known(c.name) && arg_ok(c.arg)) out.push_back(c);
    }
    return out;
}

void publish_status(const std::vector<std::pair<std::string, std::string>>& kv) {
    if (!g_started) return;
    if (media::active()) media::update(media::info_from_status(kv));
    const auto now = Clock::now();
    static Clock::time_point last_net{};
    if (net::running() && now - last_net >= std::chrono::milliseconds(250)) { last_net = now; net::set_status(kv); }
    if (now - g_last_status < std::chrono::seconds(1)) return;
    g_last_status = now;
    std::string text;
    for (const auto& [k, v] : kv) {
        std::string clean = v;
        for (char& ch : clean) if (ch == '\n' || ch == '\r') ch = ' ';
        text += k + "=" + clean + "\n";
    }
    // Paused or idle the text is the same second after second: it is written again only when it changed (or every 30 seconds, in
    // case somebody removed the file). The "running" file, not this one, tells --status that the program is alive.
    static std::string last_text;
    static Clock::time_point last_write{};
    if (text == last_text && now - last_write < std::chrono::seconds(30)) return;
    last_text = text;
    last_write = now;
    write_atomic(status_file(), text);
}

// ---- command line -------------------------------------------------------------------------------------------------------

bool is_client_call(int argc, char** argv) {
    if (argc < 2) return false;
    const std::string a = argv[1];
    return a == "--cmd" || a == "--cli" || a == "--query" || a == "--run" || a == "--scripts" || a == "--status" || a == "--remote-help" || a == "--remote-info" || a == "--help" || a == "-h" ||
           a == "--version" || a == "-v";
}

namespace {

void usage(std::ostream& o) {
    o << "Mousiki remote control: send a command to the running player, radio or audio book player.\n\n"
         "  mousiki --cmd <command> [value]     e.g.  mousiki --cmd next      mousiki --cmd volume 60      mousiki --cmd seek +10\n"
         "  mousiki --cmd --help                this list (also: mousiki --cmd help, mousiki --remote-help)\n"
         "  mousiki --status                    prints what is playing (state, title, artist, position, volume, mode ...)\n"
         "  mousiki --remote-info               the address and PIN of the remote app (Settings -> GENERAL -> CLI / REMOTE / UPDATES -> Remote App)\n"
         "\nCommand lines (text commands, chains, scripts; the same as the CLI inside Mousiki: /c: in the search bar, [t] console):\n"
         "  mousiki --cmd play \"beta tune\"       any name that is no numeric command below is sent as a command line\n"
         "  mousiki --cli \"volume 30; next\"      a chain (commands separated by ;)\n"
         "  mousiki --query get volume          like --cli, but waits for the answer and prints it (exit code 0 ok, 3 error, 4 no answer; --timeout <s>)\n"
         "  mousiki --run night                 runs the script night.cli of the cli folder (<config folder>/cli/);  mousiki --scripts lists them\n"
         "  mousiki --status --json | --format \"{artist} - {title}\" | volume     the status as JSON / a template / one value\n"
         "  A command that cannot be undone (quit, clear-queue) is refused from outside unless written  force quit.\n\n"
         "Commands:\n"
         "  play-pause  play  pause  stop       toggle / start / pause (radio: timeshift pause) / stop\n"
         "  next  prev  shuffle                 next / previous track or station, shuffle next (radio: a random station);\n"
         "                                      with a queue: next = its next title, shuffle = a random title of it\n"
         "  volume-up  volume-down              +5 / -5 (or a number: volume-up 10)\n"
         "  volume <0-100>                      set the volume (volume +10 / volume -10 change it)\n"
         "  mute  unmute  toggle-mute\n"
         "  seek <seconds>                      jump forward / back (seek -30); radio: within the timeshift buffer\n"
         "  live                                radio: back to the live stream\n"
         "  playback-mode                       player: the next playback mode, like the m key (list, repeat, shuffle ...)\n"
         "  wizard                              open / close the help wizard (SHIFT+w)\n"
         "  playlist <n>                        player: the titles of saved playlist n (A-Z order) into the queue\n"
         "  track <n>                           player: play title n of the local list (as sorted / filtered there)\n"
         "  clear-queue                         player: clear the queue (CTRL+SHIFT+Z in Mousiki undoes it)\n"
         "  tune <n>                            radio: tune station n of the station list\n"
         "  book-open <n>                       audio book: open / close row n of the library tree (the phone's LISTS)\n"
         "                                      (track <n> plays a chapter row of that tree, from its start)\n"
         "  switch-mode [-1]                    the next mode (player, radio, audio book; the * key); -1: the previous one\n"
         "  quit\n\n"
         "Exit code: 0 sent, 1 Mousiki is not running, 2 unknown command or bad value.\n"
         "System-wide keys: bind the keys of your choice in the operating system to these commands, for example a media key or\n"
         "Ctrl+Alt+Right to `mousiki --cmd next` (window manager settings, AutoHotkey, a macOS Shortcut, ...). See the manual.\n";
}

void general_help(std::ostream& o) {
    o << "Mousiki Plus v" << MOUSIKI_VERSION << " -- terminal music (-video), radio and audio book player\n\n"
         "  mousiki                    start (the player, or the radio when Start Mode is radio)\n"
         "  mousiki --cmd <command>    control a running Mousiki (mousiki --cmd --help lists the commands)\n"
         "  mousiki --status           what is playing in the running Mousiki (--json, --format, or one key)\n"
         "  mousiki --query <line>     a command line for the running Mousiki, with its answer (--cli: without; --run <script>)\n"
         "  mousiki --remote-info      address and PIN of the remote app (phone)\n"
         "  mousiki --version          the version\n"
         "  mousiki --help             this text\n\n"
         "Inside the program: ? shows every key, SHIFT+w opens the help wizard. The manual: Mousiki_User_Manual/Mousiki_User_Guide.md\n";
}

// RemoteApp= / RemoteAppPort= / RemoteAppPin= from config.txt
void remote_info(std::ostream& o) {
    const RemoteAppConfig rc = read_remote_app();
    if (!rc.enabled) {
        o << "The remote app is off. Switch it on in Settings -> GENERAL -> CLI / REMOTE / UPDATES -> Remote App (player or radio).\n";
        return;
    }
    const std::string ip = net::lan_address();
    o << "Remote app:  http://" << (ip.empty() ? std::string("<this computer's address>") : ip) << ":" << rc.port << "\n"
      << "PIN:         " << rc.pin << "\n"
      << "Open the address in the Mousiki Remote app (or in the phone's browser); the phone must be in the same network.\n";
}

} // namespace

int client_main(int argc, char** argv) {
    const std::string a1 = argc > 1 ? argv[1] : "";
    if (a1 == "--remote-help") { usage(std::cout); return 0; }
    if (a1 == "--help" || a1 == "-h") { general_help(std::cout); return 0; }
    if (a1 == "--version" || a1 == "-v") { std::cout << "Mousiki Plus v" << MOUSIKI_VERSION << "\n"; return 0; }
    if (a1 == "--remote-info") { remote_info(std::cout); return 0; }
    if (a1 == "--cmd" && argc > 2) {
        const std::string a2 = argv[2];
        if (a2 == "--help" || a2 == "-h" || a2 == "help" || a2 == "--list" || a2 == "list") { usage(std::cout); return 0; }
    }

    std::error_code ec;
    // is a program running? ("running" refreshed within the last 10 s)
    bool alive = false;
    std::string mode;
    if (fs::exists(running_file(), ec)) {
        const auto age = fs::file_time_type::clock::now() - fs::last_write_time(running_file(), ec);
        if (!ec && age < std::chrono::seconds(10)) {
            alive = true;
            std::ifstream f(running_file());
            std::string pid;
            std::getline(f, pid);
            std::getline(f, mode);
        }
    }

    if (a1 == "--scripts") {   // the scripts of the command line (no running program needed)
        for (const std::string& n : cli::script_names()) std::cout << n << "\n";
        return 0;
    }
    if (a1 == "--status") {
        if (!alive) { std::cerr << "Mousiki is not running.\n"; return 1; }
        std::ifstream f(status_file());
        std::map<std::string, std::string> kv;
        if (!f) kv = {{"mode", mode}, {"state", "unknown"}};
        for (std::string l; f && std::getline(f, l);) { const size_t eq = l.find('='); if (eq != std::string::npos) kv[l.substr(0, eq)] = l.substr(eq + 1); }
        std::string fmt, want;   // --status --json | --status --format "{artist} - {title}" | --status <key>
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--json") want = "json";
            else if (a == "--format" && i + 1 < argc) { want = "format"; fmt = argv[++i]; }
            else if (a[0] != '-') want = "key:" + a;
        }
        if (want == "json") {
            static const char* const kNum[] = {"position_sec", "duration_sec", "volume", "queue_len"};
            std::cout << "{";
            bool first = true;
            for (const auto& [k, v] : kv) {
                bool num = false;
                for (const char* n : kNum) if (k == n) num = !v.empty() && v.find_first_not_of("0123456789.-") == std::string::npos;
                std::string e;
                for (char ch : v) { if (ch == '"' || ch == '\\') { e += '\\'; e += ch; } else if (static_cast<unsigned char>(ch) < 32) e += ' '; else e += ch; }
                std::cout << (first ? "" : ", ") << "\"" << k << "\": " << (num ? e : "\"" + e + "\"");
                first = false;
            }
            std::cout << "}\n";
        } else if (want == "format") {
            std::string out;
            for (size_t i = 0; i < fmt.size(); ++i) {
                const size_t close = fmt[i] == '{' ? fmt.find('}', i) : std::string::npos;
                if (close == std::string::npos) { out += fmt[i]; continue; }
                const auto it = kv.find(fmt.substr(i + 1, close - i - 1));
                out += it == kv.end() ? "" : it->second;
                i = close;
            }
            std::cout << out << "\n";
        } else if (want.rfind("key:", 0) == 0) {
            const auto it = kv.find(want.substr(4));
            if (it == kv.end()) return 2;
            std::cout << it->second << "\n";
        } else for (const auto& [k, v] : kv) std::cout << k << "=" << v << "\n";
        return 0;
    }
    // --cli / --query / --run: a command line (text) for the running program; --query (or --wait) waits for the answer
    {
        std::vector<std::string> words;
        bool wait = false;
        double timeout = 5.0;
        bool as_cli = a1 == "--cli" || a1 == "--query" || a1 == "--run";
        if (a1 == "--query") wait = true;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--wait") wait = true;
            else if (a == "--timeout" && i + 1 < argc) { timeout = std::clamp(std::atof(argv[++i]), 0.5, 600.0); wait = true; }
            else words.push_back(a);
        }
        if (a1 == "--cmd") {   // a command with a number is the old file command; anything else is a command line, as with --cli
            if (!words.empty() && !known(words[0])) as_cli = true;                       // a name that is no numeric command: a command line (the program decides)
            else if (!words.empty() && words[0] == "play" && words.size() >= 2 && !arg_ok(words[1])) as_cli = true;   // play <words>
            // (a known command with a bad value is refused here as before: exit code 2)
        }
        if (as_cli) {
            if (words.empty()) { std::cerr << "mousiki: " << a1 << " needs a command, e.g.  mousiki --query status   mousiki --cli \"volume 30; next\"\n"; return 2; }
            std::string line;
            if (a1 == "--run") line = "run " + words[0];
            else if (words.size() == 1) line = words[0];
            else for (size_t i = 0; i < words.size(); ++i) {
                const bool q = words[i].find_first_of(" ;\t") != std::string::npos;
                line += (i ? " " : "") + (q ? "\"" + words[i] + "\"" : words[i]);
            }
            if (line.size() > 600) { std::cerr << "mousiki: the command line is too long (600 characters; put it in a script and use --run)\n"; return 2; }
            if (!alive) { std::cerr << "Mousiki is not running.\n"; return 1; }
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            int& serial2 = g_client_serial;
            char idb[64];
            const int sn = serial2++ % 10000;
            std::snprintf(idb, sizeof idb, "%013lld-%lld-%d", ms, static_cast<long long>(MOUSIKI_GETPID()), sn);
            const std::string id = idb;
            char name[96];
            std::snprintf(name, sizeof name, "%013lld-%08lld-%04d.cmd", ms, static_cast<long long>(MOUSIKI_GETPID()), sn);
            if (!write_atomic(dir() / name, "cli " + (wait ? id : std::string("-")) + " " + line + "\n")) { std::cerr << "mousiki: cannot write to " << path_utf8(dir()) << "\n"; return 1; }
            if (!wait) return 0;
            const fs::path rp = dir() / ("reply-" + id + ".txt");
            const auto until = Clock::now() + std::chrono::milliseconds(static_cast<long long>(timeout * 1000));
            while (Clock::now() < until) {
                std::ifstream rf(rp);
                if (rf) {
                    std::string first, rest, l;
                    std::getline(rf, first);
                    while (std::getline(rf, l)) rest += l + "\n";
                    rf.close();
                    std::error_code e3;
                    fs::remove(rp, e3);
                    (first == "ok" ? std::cout : std::cerr) << rest;
                    return first == "ok" ? 0 : 3;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            std::cerr << "mousiki: no answer within " << timeout << " s (the command may still run)\n";
            return 4;
        }
    }

    // --cmd <name> [arg]
    if (argc < 3) { usage(std::cerr); return 2; }
    Command c;
    c.name = argv[2];
    if (argc > 3) c.arg = argv[3];
    if (!known(c.name)) { std::cerr << "mousiki: unknown command '" << c.name << "'\n\n"; usage(std::cerr); return 2; }
    if (!arg_ok(c.arg)) { std::cerr << "mousiki: the value must be a number such as 60, +10 or -30\n"; return 2; }
    if (!alive) { std::cerr << "Mousiki is not running.\n"; return 1; }

    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    int& serial = g_client_serial;   // two commands in the same millisecond must not share a name (and keep their order)
    char name[64];
    std::snprintf(name, sizeof name, "%013lld-%08lld-%04d.cmd", ms, static_cast<long long>(MOUSIKI_GETPID()), serial++ % 10000);
    if (!write_atomic(dir() / name, c.name + (c.arg.empty() ? "" : " " + c.arg) + "\n")) {
        std::cerr << "mousiki: cannot write to " << path_utf8(dir()) << "\n";
        return 1;
    }
    return 0;
}

} // namespace muisc::remote
