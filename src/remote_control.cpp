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
    const fs::path tmp = target.string() + ".tmp";
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
        if (it->path().extension() == ".cmd") { std::error_code e2; fs::remove(it->path(), e2); }
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
        if (line.size() > 40) continue;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
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
    write_atomic(status_file(), text);
}

// ---- command line -------------------------------------------------------------------------------------------------------

bool is_client_call(int argc, char** argv) {
    if (argc < 2) return false;
    const std::string a = argv[1];
    return a == "--cmd" || a == "--status" || a == "--remote-help" || a == "--remote-info" || a == "--help" || a == "-h" ||
           a == "--version" || a == "-v";
}

namespace {

void usage(std::ostream& o) {
    o << "Mousiki remote control: send a command to the running player, radio or audio book player.\n\n"
         "  mousiki --cmd <command> [value]     e.g.  mousiki --cmd next      mousiki --cmd volume 60      mousiki --cmd seek +10\n"
         "  mousiki --cmd --help                this list (also: mousiki --cmd help, mousiki --remote-help)\n"
         "  mousiki --status                    prints what is playing (state, title, artist, position, volume, mode ...)\n"
         "  mousiki --remote-info               the address and PIN of the remote app (Settings -> GENERAL -> REMOTE / UPDATES -> Remote App)\n\n"
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
    o << "Mousiki Plus v" << MOUSIKI_VERSION << " -- terminal music, radio and audio book player\n\n"
         "  mousiki                    start (the player, or the radio when Start Mode is radio)\n"
         "  mousiki --cmd <command>    control a running Mousiki (mousiki --cmd --help lists the commands)\n"
         "  mousiki --status           what is playing in the running Mousiki\n"
         "  mousiki --remote-info      address and PIN of the remote app (phone)\n"
         "  mousiki --version          the version\n"
         "  mousiki --help             this text\n\n"
         "Inside the program: ? shows every key, SHIFT+w opens the help wizard. The manual: Mousiki_User_Manual/Mousiki_User_Guide.md\n";
}

// RemoteApp= / RemoteAppPort= / RemoteAppPin= from config.txt
void remote_info(std::ostream& o) {
    const RemoteAppConfig rc = read_remote_app();
    if (!rc.enabled) {
        o << "The remote app is off. Switch it on in Settings -> GENERAL -> REMOTE / UPDATES -> Remote App (player or radio).\n";
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

    if (a1 == "--status") {
        if (!alive) { std::cerr << "Mousiki is not running.\n"; return 1; }
        std::ifstream f(status_file());
        if (!f) { std::cout << "mode=" << mode << "\nstate=unknown\n"; return 0; }
        std::cout << f.rdbuf();
        return 0;
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
    static int serial = 0;   // two commands in the same millisecond must not share a name
    char name[64];
    std::snprintf(name, sizeof name, "%013lld-%08lld-%04d.cmd", ms, static_cast<long long>(MOUSIKI_GETPID()), serial++ % 10000);
    if (!write_atomic(dir() / name, c.name + (c.arg.empty() ? "" : " " + c.arg) + "\n")) {
        std::cerr << "mousiki: cannot write to " << dir().string() << "\n";
        return 1;
    }
    return 0;
}

} // namespace muisc::remote
