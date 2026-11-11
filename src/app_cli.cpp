// The command line of Mousiki: "/c:" in the search bar, the third pane of the console overlay ([t]), scripts (<config folder>/cli/*.cli),
// the startup file, timers, and `mousiki --cmd / --cli / --query` from outside. One interpreter for all of them.
//
// A line is a chain of commands separated by ';'. A chain or a script runs as a Job: its commands one after the other; `wait 5s` makes the job
// sleep, an error ends it (a script line that starts with '-' may fail). Where a line comes from (Origin) decides the safety:
//   Typed   a person at the keyboard: a command that cannot be undone (`quit`, `clear-queue` ...) asks for `yes` first
//   Script  a script or the startup file, Remote: `mousiki --cmd`, the phone, a hook: such a command is refused unless written `force <command>`
// There is no command that deletes files, playlists or settings. `set` only reaches flags and numbers (settings.h: cli_setting_set).
#include "app.h"
#include "remote_control.h"
#include "console_log.h"
#include "utf8_util.h"
#include "text_edit.h"
#include "app_internal.h"
#include "app_state_cheat.h"
#include "app_state_cliov.h"
#include "string_util.h"
#include "cli_core.h"
#include "settings.h"
#include <algorithm>
#include <cstdio>
#include <sstream>

namespace muisc {

using cli::Origin;

namespace {

bool numeric_arg(const std::string& a) {
    if (a.empty() || a.size() > 12) return false;
    bool digit = false;
    for (char c : a) {
        if (c >= '0' && c <= '9') digit = true;
        else if (c != '+' && c != '-' && c != '.') return false;
    }
    return digit;
}

// The table of the commands: it is the one place for what exists, the help text, the completion and the safety.
//   arg: 0 none, 1 an optional number, 2 a number is needed, 3 text (the command parses it itself)
struct Cmd { const char* name; const char* usage; const char* help; int arg; bool caution; bool remote_cmd; };
const Cmd kCmds[] = {
    // name            usage                          help                                                       arg caution remote_cmd(= also a `mousiki --cmd` word)
    {"play-pause",    "",                            "toggle play / pause",                                     0, false, true},
    {"pause",         "",                            "pause (stop is the same here)",                           0, false, true},
    {"stop",          "",                            "pause",                                                   0, false, true},
    {"next",          "",                            "next title (with a queue: its next one)",                0, false, true},
    {"prev",          "",                            "previous title",                                          0, false, true},
    {"shuffle",       "",                            "shuffle next (a random title)",                           0, false, true},
    {"play",          "[words]",                     "play the first local title that matches; alone: resume",  3, false, true},
    {"volume",        "<0-100 | +n | -n>",           "set / change the volume",                                 2, false, true},
    {"volume-up",     "[n]",                         "louder (5)",                                              1, false, true},
    {"volume-down",   "[n]",                         "quieter (5)",                                             1, false, true},
    {"mute",          "",                            "mute",                                                    0, false, true},
    {"unmute",        "",                            "unmute",                                                  0, false, true},
    {"toggle-mute",   "",                            "mute / unmute",                                           0, false, true},
    {"seek",          "<seconds>",                   "jump forward / back (seek -30)",                          2, false, true},
    {"playback-mode", "",                            "next playback mode (list, repeat, shuffle ...)",          0, false, true},
    {"switch-mode",   "[-1]",                        "next mode: player, radio, audio book",                    1, false, true},
    {"wizard",        "",                            "open / close the help wizard",                            0, false, true},
    {"playlist",      "<n>",                         "queue saved playlist n (see playlists)",                  2, false, true},
    {"playlists",     "",                            "the saved playlists with their numbers",                  0, false, false},
    {"track",         "<n>",                         "play title n of the list",                                2, false, true},
    {"book-open",     "<n>",                         "audio book: open / play row n of the library tree",       2, false, true},
    {"queue",         "[words | next words | remove n | shuffle | clear]", "the queue: show; add the first match to its end / next; remove title n; shuffle; clear", 3, false, false},
    {"clear-queue",   "",                            "clear the queue",                                         0, true,  true},
    {"search",        "[words]",                     "filter the local list (alone: clear the filter)",         3, false, false},
    {"status",        "",                            "what plays now",                                          0, false, false},
    {"get",           "<what>",                      "a value: volume state title artist album position duration queue mode muted, or a setting", 3, false, false},
    {"set",           "<setting> <value>",           "change and save a flag / number setting (settings lists them)", 3, false, false},
    {"settings",      "[words]",                     "the settings that set can change",                        3, false, false},
    {"open",          "<where>",                     "open console, settings, cheatsheet, wizard, history, queue, folders, equalizer, playlist", 3, false, false},
    {"edit",          "[script]",                    "open the script overlay (library and editor), on a script",   3, false, false},
    {"rescan",        "",                            "scan the music folders again",                            0, false, false},
    {"echo",          "<text>",                      "write text (a script's message)",                         3, false, false},
    {"log",           "<text>",                      "write text into the log of the console",                  3, false, false},
    {"wait",          "<time>",                      "a script waits: 5, 5s, 2m, 1h30m",                        3, false, false},
    {"in",            "<time> <command>",            "run a command later: in 30m pause",                       3, false, false},
    {"timers",        "",                            "the commands waiting for their time",                     0, false, false},
    {"hooks",         "",                            "the event hooks of hooks.cli (on track-change = ...)",    0, false, false},
    {"cancel",        "[n | all]",                   "cancel a timer (or all)",                                 3, false, false},
    {"if",            "<condition> <command>",       "playing paused stopped muted queue-empty volume>50 (! negates)", 3, false, false},
    {"run",           "<script>",                    "run a script of the cli folder",                          3, false, false},
    {"scripts",       "",                            "the scripts",                                             0, false, false},
    {"alias",         "[name = commands]",           "a short name for commands (alone: the list)",             3, false, false},
    {"unalias",       "<name>",                      "remove an alias",                                         3, false, false},
    {"force",         "<command>",                   "run a command that asks for confirmation, without asking", 3, false, false},
    {"yes",           "",                            "confirm what was asked",                                  0, false, false},
    {"clear",         "",                            "clear the reply rows",                                    0, false, false},
    {"quit",          "",                            "quit Mousiki",                                            0, true,  true},
    {"help",          "[command]",                   "this list (into the log); help <command> for one",        3, false, false},
};
const Cmd* find_cmd(const std::string& n) {
    for (const Cmd& c : kCmds) if (n == c.name) return &c;
    return nullptr;
}
const std::pair<const char*, const char*> kAlias[] = {
    {"toggle", "play-pause"}, {"pp", "play-pause"}, {"vol", "volume"}, {"vol-up", "volume-up"}, {"vol-down", "volume-down"},
    {"m", "playback-mode"}, {"mode", "playback-mode"}, {"previous", "prev"}, {"skip", "next"}, {"clearqueue", "clear-queue"},
    {"q", "queue"}, {"s", "search"}, {"find", "search"}, {"?", "help"}, {"now", "status"}, {"exit", "quit"}, {"sleep", "wait"},
};

std::string join_lines(const std::vector<std::string>& v) {
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) r += (i ? "\n" : "") + v[i];
    return r;
}

}  // namespace

cli::State App::cli_state() const {
    cli::State st;
    st.has_track = has_track_;
    st.paused = has_track_ && player_.is_paused();
    st.muted = muted_;
    st.volume = player_.volume();
    st.queue = queue_.size();
    st.position = has_track_ ? player_.poll_elapsed() : 0.0;
    st.duration = has_track_ ? static_cast<double>(total_sec_) : 0.0;
    return st;
}

bool App::cli_in_search() const {
    if (mode_ != Mode::Search) return false;
    const std::string& b = search_.buffer;
    size_t i = 0;
    while (i < b.size() && b[i] == ' ') ++i;
    return b.size() >= i + 2 && (b[i] == 'c' || b[i] == 'C') && b[i + 1] == ':';
}

// A line from outside the job machinery (typed, a script, `mousiki --cmd`): runs at once as far as it goes without waiting.
std::string App::cli_run(const std::string& raw, Origin origin, const std::string& reply_id) {
    const std::string line = cli::trim(raw);
    if (line.empty()) return "";
    if (origin == Origin::Typed) {
        cli_out_.push_back("> " + line);
        if (cli_hist_.empty() || cli_hist_.back() != line) cli_hist_.push_back(line);
        if (cli_hist_.size() > 100) cli_hist_.erase(cli_hist_.begin());
        cli_hist_pos_ = -1;
    }
    log_event(std::string("CLI") + (origin == Origin::Typed ? "" : origin == Origin::Script ? " (script)" : origin == Origin::Network ? " (network)" : " (remote)") + ": " + line);
    cli::Job job;
    job.origin = origin;
    job.reply_id = reply_id;
    job.label = line;
    job.lines.push_back(line);
    cli_step(job);
    std::string reply = job.out.empty() ? std::string() : job.out.back();
    if (job.lines.empty() && !job.waiting) cli_finish(job);
    else cli_jobs_.push_back(std::move(job));
    force_redraw_ = true;
    return reply;
}

void App::cli_finish(cli::Job& job) {
    if (!job.reply_id.empty()) remote::reply(job.reply_id, !job.failed, join_lines(job.out));
}

void App::cli_step(cli::Job& job) {
    job.waiting = false;
    while (!job.lines.empty()) {
        if (++job.executed > 1000) { job.failed = true; job.lines.clear(); job.out.push_back("error: more than 1000 lines in one run (a script that calls itself?)"); break; }
        bool soft = false;
        const std::string line = cli::script_line(job.lines.front(), soft);
        job.lines.pop_front();
        if (line.empty()) continue;
        // `alias name = a; b` defines an alias of the whole chain: the line is not split here
        const bool is_alias_def = cli::is_alias_definition(line);
        const std::vector<std::string> parts = is_alias_def ? std::vector<std::string>{line} : cli::split_chain(line);
        if (parts.size() > 1) {   // a chain: its commands go to the front, in order
            for (size_t i = parts.size(); i-- > 0;) job.lines.push_front((soft ? "-" : "") + parts[i]);
            continue;
        }
        bool ok = true;
        const std::string reply = cli_exec(parts.empty() ? line : parts[0], job, ok, false);
        const std::string shown = ok ? reply : "error: " + reply;
        job.out.push_back(shown);
        if (job.origin == Origin::Typed) {
            std::istringstream in(shown);
            for (std::string l; std::getline(in, l);) cli_out_.push_back("  " + l);
            while (cli_out_.size() > 200) cli_out_.erase(cli_out_.begin());
            cli_reply_ = shown.substr(0, shown.find('\n'));
            ++cli_reply_serial_;
        } else if (!ok) log_event("CLI error: " + reply + "  [" + job.label + "]");
        if (job.to_overlay && cliov_) {   // the script overlay shows the answers at the bottom of its editor
            std::istringstream in(shown);
            for (std::string l; std::getline(in, l);) cliov_->out.push_back("  " + l);
            while (cliov_->out.size() > 100) cliov_->out.erase(cliov_->out.begin());
        }
        if (!ok && !soft) { job.failed = true; job.lines.clear(); break; }
        if (job.waiting) break;
    }
}

// Once per frame: timers that are due start their commands, jobs whose wait is over go on, the startup script runs once.
void App::cli_tick() {
    const auto now = std::chrono::steady_clock::now();
    if (cli_started_at_ == std::chrono::steady_clock::time_point{}) cli_started_at_ = now;
    if (!cli_startup_done_ && now - cli_started_at_ > std::chrono::milliseconds(2500)) {   // (the library has been read by then)
        cli_startup_done_ = true;
        std::vector<std::string> lines;
        std::string err;
        if (cli::read_script("startup", lines, err)) {
            cli::Job job;
            job.origin = Origin::Script;
            job.label = "startup";
            for (const std::string& l : lines) job.lines.push_back(l);
            log_event("CLI: running startup.cli (" + std::to_string(lines.size()) + " lines)");
            cli_step(job);
            if (job.lines.empty() && !job.waiting) cli_finish(job); else cli_jobs_.push_back(std::move(job));
        }
    }
    for (size_t i = 0; i < cli_timers_.size();) {
        if (cli_timers_[i].at > now) { ++i; continue; }
        const cli::Timer t = cli_timers_[i];
        cli_timers_.erase(cli_timers_.begin() + static_cast<long>(i));
        cli_run(t.line, t.origin == Origin::Typed ? Origin::Script : t.origin);   // (a timer was set by a person, but nobody is there to confirm)
    }
    for (size_t i = 0; i < cli_jobs_.size();) {
        cli::Job& j = cli_jobs_[i];
        if (j.waiting && j.wait_until > now) { ++i; continue; }
        cli_step(j);
        if (j.lines.empty() && !j.waiting) { cli_finish(j); cli_jobs_.erase(cli_jobs_.begin() + static_cast<long>(i)); }
        else ++i;
    }
    cli_hooks_tick();
}

// Looks at what the player does (a new title, pause / resume, the queue running empty, playback ended) and runs the matching lines of
// hooks.cli as a script (Origin::Script: nothing that asks for `yes` runs without `force`). Cheap: one compare per frame; the file is
// read only when an event happened. A hook that causes its own event (on pause = play) is cut off by the limiter (8 runs in 10 s).
void App::cli_hooks_tick() {
    if (!cli_startup_done_) return;   // (the first look is after the startup script)
    CliHookSnap now;
    now.valid = true;
    now.has = has_track_;
    now.paused = has_track_ && player_.is_paused();
    now.queue = queue_.size();
    if (has_track_) now.key = path_utf8(current_.path);
    if (cli_hook_snap_.valid) {
        const CliHookSnap& o = cli_hook_snap_;
        std::vector<std::string> ev;
        const auto t = std::chrono::steady_clock::now();
        if (now.has && (!o.has || o.key != now.key)) { cli_hook_track_pending_ = true; cli_hook_track_at_ = t; }
        else if (now.has && o.has && now.paused != o.paused) {
            if (t - cli_hook_track_at_ > std::chrono::milliseconds(800)) ev.push_back(now.paused ? "pause" : "resume");
        }
        if (o.has && !now.has) ev.push_back("idle");
        if (o.queue > 0 && now.queue == 0) ev.push_back("queue-empty");
        cli_hook_snap_ = now;
        // the new title counts once it has been there for half a second (the load is done: a `volume` in the hook sticks)
        if (cli_hook_track_pending_ && (!now.has || t - cli_hook_track_at_ >= std::chrono::milliseconds(500))) {
            cli_hook_track_pending_ = false;
            if (now.has) ev.insert(ev.begin(), "track-change");
        }
        for (const std::string& e : ev) cli_fire_hook(e);
    } else cli_hook_snap_ = now;
}

void App::cli_fire_hook(const std::string& event) {
    std::vector<std::string> lines;
    std::string err;
    if (!cli::read_script("hooks", lines, err)) return;
    const std::vector<cli::Hook> hooks = cli::parse_hooks(lines);
    std::vector<std::string> run;
    for (const cli::Hook& h : hooks) if (h.event == event) run.push_back(h.chain);
    if (run.empty()) return;
    if (!cli_hook_limit_.allow(std::chrono::steady_clock::now())) {
        if (!cli_hook_warned_) { cli_hook_warned_ = true; log_event("CLI: hooks paused: more than 8 runs in 10 s (does a hook cause its own event?)"); }
        return;
    }
    cli_hook_warned_ = false;
    const TrackMetadata md = has_track_ ? display_metadata() : TrackMetadata{};
    const std::map<std::string, std::string> vals = {
        {"title", has_track_ ? md.name : std::string()}, {"artist", has_track_ && md.artist != "-" ? md.artist : std::string()},
        {"album", has_track_ && md.album != "-" ? md.album : std::string()},
        {"state", !has_track_ ? "stopped" : player_.is_paused() ? "paused" : "playing"}, {"volume", std::to_string(player_.volume())},
        {"queue", std::to_string(queue_.size())}, {"position", std::to_string(static_cast<int>(has_track_ ? player_.poll_elapsed() : 0.0))}};
    for (const std::string& chain : run) {
        cli::Job job;
        job.origin = Origin::Script;
        job.label = "on " + event;
        job.lines.push_back(cli::expand_hook(chain, vals));
        log_event("CLI hook " + event + ": " + job.lines.front());
        cli_step(job);
        if (job.lines.empty() && !job.waiting) cli_finish(job); else cli_jobs_.push_back(std::move(job));
    }
}

std::string App::cli_exec(const std::string& line_in, cli::Job& job, bool& ok, bool forced) {
    std::string line = cli::trim(line_in);
    auto fail = [&](const std::string& m) { ok = false; return m; };
    std::string name = line, arg;
    if (const size_t sp = line.find_first_of(" \t"); sp != std::string::npos) { name = line.substr(0, sp); arg = cli::trim(line.substr(sp + 1)); }
    name = ascii_lower_str(name);
    if (!name.empty() && name[0] == '/') name.erase(0, 1);
    for (const auto& a : kAlias) if (name == a.first) { name = a.second; break; }

    // from the web server (/api/cli: Home Assistant & co): the harmless set -- nothing that quits, changes settings or scripts, opens
    // screens or empties the queue, and no `force` (so also `run` of a script cannot do more than the endpoint itself)
    if (job.origin == Origin::Network) {
        static const char* const kDenied[] = {"quit", "force", "yes", "set", "alias", "unalias", "edit", "open", "clear-queue", "rescan", "wizard", "switch-mode"};
        for (const char* d : kDenied)
            if (name == d) return fail(name + " is not allowed over the network (use it at the keyboard, or `mousiki --cli` on this computer)");
        if (name == "queue" && ascii_lower_str(cli::trim(arg)).compare(0, 5, "clear") == 0) return fail("queue clear is not allowed over the network");
    }
    // the aliases of the user (aliases.cli): a name that is none of the commands
    if (!cli_aliases_loaded_) { cli_aliases_ = cli::load_aliases(); cli_aliases_loaded_ = true; }
    if (!find_cmd(name)) {
        const auto it = cli_aliases_.find(name);
        if (it != cli_aliases_.end()) {
            const std::vector<std::string> body = cli::split_chain(it->second);
            if (body.empty()) return fail("empty alias " + name);
            for (size_t i = body.size(); i-- > 0;) job.lines.push_front(i + 1 == body.size() && !arg.empty() ? body[i] + " " + arg : body[i]);
            return "alias " + name;
        }
        return fail("unknown command \"" + name + "\" -- type help");
    }
    const Cmd& cmd = *find_cmd(name);

    // safety: a command that cannot be undone
    if (cmd.caution && !forced) {
        if (job.origin != Origin::Typed) return fail(name + " needs `force " + name + "` in a script or from outside");
        cli_confirm_.line = name;
        cli_confirm_.until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        return fail(name + " cannot be undone: type yes to run it (within 20 s), or force " + name);
    }
    if (cmd.arg == 0 && !arg.empty()) return fail(name + " takes no argument");
    if (cmd.arg == 2 && arg.empty()) return fail(name + " needs a number, e.g. " + name + " 60");
    if ((cmd.arg == 1 || cmd.arg == 2) && !arg.empty() && !numeric_arg(arg)) return fail(name + " needs a number, e.g. " + name + (cmd.arg == 2 ? " 60" : " 10"));

    // --- commands that need text or the app's own logic ---
    if (name == "help") {
        std::ostringstream o;
        if (!arg.empty()) {
            const Cmd* c = find_cmd(ascii_lower_str(arg));
            if (!c) return fail("no command \"" + arg + "\"");
            return std::string(c->name) + (c->usage[0] ? " " : "") + c->usage + "  --  " + c->help + (c->caution ? "  (asks for yes; force in scripts)" : "");
        }
        log_event("CLI help: commands (a line may hold several, separated by ;  ESC leaves  UP/DOWN history  CTRL+RIGHT completes):");
        for (const Cmd& c : kCmds) log_event(std::string("CLI help:   ") + c.name + (c.usage[0] ? " " : "") + c.usage + "  --  " + c.help + (c.caution ? "  [asks for yes]" : ""));
        log_event("CLI help: short forms: vol m toggle q s now skip; scripts: <config>/cli/*.cli, startup.cli runs at the start; there is no command that deletes anything");
        return "see the log (t) for the " + std::to_string(sizeof(kCmds) / sizeof(kCmds[0])) + " commands; help <command> for one";
    }
    if (name == "clear") { cli_out_.clear(); cli_reply_.clear(); return ""; }
    if (name == "yes") {
        if (job.origin != Origin::Typed) return fail("yes is for the keyboard; use force <command> in a script");
        if (cli_confirm_.line.empty() || std::chrono::steady_clock::now() > cli_confirm_.until) return fail("nothing waits for yes");
        const std::string l = cli_confirm_.line;
        cli_confirm_.line.clear();
        return cli_exec(l, job, ok, true);
    }
    if (name == "force") {
        if (arg.empty()) return fail("force <command>");
        return cli_exec(arg, job, ok, true);
    }
    if (name == "echo") return arg;
    if (name == "log") { log_event("CLI: " + arg); return "logged"; }
    if (name == "wait") {
        double sec = 0;
        if (!cli::parse_duration(arg, sec, 3600.0)) return fail("wait <time>: 5, 5s, 2m (at most 1h)");
        job.waiting = true;
        job.wait_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(sec * 1000));
        return "waiting " + cli::duration_text(sec);
    }
    if (name == "in") {
        const size_t sp = arg.find_first_of(" \t");
        double sec = 0;
        if (sp == std::string::npos || !cli::parse_duration(arg.substr(0, sp), sec)) return fail("in <time> <command>, e.g. in 30m pause (at most 24h)");
        const std::string what = cli::trim(arg.substr(sp + 1));
        if (what.empty()) return fail("in <time> <command>");
        if (cli_timers_.size() >= 50) return fail("50 timers are waiting: cancel some");
        cli::Timer t;
        t.at = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(sec * 1000));
        t.line = what;
        t.origin = job.origin;
        t.id = ++cli_timer_serial_;
        cli_timers_.push_back(t);
        return "timer " + std::to_string(t.id) + ": in " + cli::duration_text(sec) + " -- " + what;
    }
    if (name == "hooks") {
        std::vector<std::string> hl;
        std::string err;
        std::string evs;
        for (const std::string& e : cli::hook_events()) evs += (evs.empty() ? "" : " ") + e;
        if (!cli::read_script("hooks", hl, err)) return "no hooks yet. Write them with `edit hooks`, one per line:  on <event> = <commands>   (events: " + evs + ";  {title} {artist} {album} {state} {volume} {queue} {position} are filled in)";
        const std::vector<cli::Hook> hs = cli::parse_hooks(hl);
        if (hs.empty()) return "hooks.cli has no usable line. Format:  on <event> = <commands>   (events: " + evs + ")";
        std::string r = std::to_string(hs.size()) + (hs.size() == 1 ? " hook:" : " hooks:");
        for (const cli::Hook& h : hs) r += "\n  on " + h.event + " = " + h.chain;
        return r;
    }
    if (name == "timers") {
        if (cli_timers_.empty()) return "no timers";
        std::string r;
        const auto now = std::chrono::steady_clock::now();
        for (const cli::Timer& t : cli_timers_) r += (r.empty() ? "" : "\n") + std::string("timer ") + std::to_string(t.id) + ": in " + cli::duration_text(std::max(0.0, std::chrono::duration<double>(t.at - now).count())) + " -- " + t.line;
        return r;
    }
    if (name == "cancel") {
        if (arg.empty() || ascii_lower_str(arg) == "all") { const size_t n = cli_timers_.size(); cli_timers_.clear(); return "cancelled " + std::to_string(n) + (n == 1 ? " timer" : " timers"); }
        const int id = std::atoi(arg.c_str());
        for (size_t i = 0; i < cli_timers_.size(); ++i) if (cli_timers_[i].id == id) { cli_timers_.erase(cli_timers_.begin() + static_cast<long>(i)); return "cancelled timer " + std::to_string(id); }
        return fail("no timer " + arg);
    }
    if (name == "if") {
        // if <condition> <command>: the condition is the words up to the first one that is a command
        std::vector<std::string> w = cli::split_words(arg);
        std::string cond;
        size_t k = 0;
        for (; k < w.size(); ++k) {
            std::string lw = ascii_lower_str(w[k]);
            std::string probe = cond.empty() ? w[k] : cond + " " + w[k];
            bool cok = false;
            cli::eval_condition(probe, cli_state(), cok);
            if (cok) { cond = probe; ++k; break; }
            if (lw == "not" || lw == "!") { cond = cond.empty() ? "!" : cond + " !"; continue; }
            return fail("if: not a condition: " + probe + " (playing paused stopped muted unmuted queue-empty queue-nonempty volume>50 ...)");
        }
        if (k >= w.size()) return fail("if <condition> <command>");
        bool cok = true;
        const bool yes = cli::eval_condition(cond, cli_state(), cok);
        std::string rest;
        const size_t pos = arg.find(w[k]);
        rest = pos == std::string::npos ? w[k] : arg.substr(pos);
        if (!yes) return "(not " + cond + ")";
        job.lines.push_front(rest);
        return "(" + cond + ")";
    }
    if (name == "run") {
        std::vector<std::string> lines;
        std::string err;
        const std::string n = cli::unquote(arg);
        if (!cli::read_script(n, lines, err)) return fail(err);
        for (size_t i = lines.size(); i-- > 0;) job.lines.push_front(lines[i]);
        return "running " + n + " (" + std::to_string(lines.size()) + " lines)";
    }
    if (name == "scripts") {
        const std::vector<std::string> n = cli::script_names();
        if (n.empty()) return "no scripts (" + path_utf8(cli::dir()) + ")";
        std::string r;
        for (const std::string& s : n) r += (r.empty() ? "" : "  ") + s;
        return r;
    }
    if (name == "alias" || name == "unalias") {
        cli_aliases_ = cli::load_aliases();
        if (name == "unalias") {
            if (!cli_aliases_.erase(arg)) return fail("no alias " + arg);
            cli::save_aliases(cli_aliases_);
            return "removed alias " + arg;
        }
        if (arg.empty()) {
            if (cli_aliases_.empty()) return "no aliases (alias night = volume 25; wait 1s; pause)";
            std::string r;
            for (const auto& [k, v] : cli_aliases_) r += (r.empty() ? "" : "\n") + k + " = " + v;
            return r;
        }
        const size_t eq = arg.find('=');
        if (eq == std::string::npos) return fail("alias <name> = <commands>");
        const std::string k = cli::trim(arg.substr(0, eq)), v = cli::trim(arg.substr(eq + 1));
        if (!cli::valid_name(k) || find_cmd(k)) return fail("\"" + k + "\" is a command or not a name (letters, digits, - _ .)");
        if (v.empty()) return fail("alias <name> = <commands>");
        cli_aliases_[k] = v;
        if (!cli::save_aliases(cli_aliases_)) return fail("could not save " + path_utf8(cli::dir() / "aliases.cli"));
        return "alias " + k + " = " + v;
    }
    if (name == "settings") {
        std::string r;
        int n = 0;
        const std::string f = ascii_lower_str(arg);
        for (const std::string& k : cli_setting_names()) {
            if (!f.empty() && ascii_lower_str(k).find(f) == std::string::npos) continue;
            ++n;
            r += (r.empty() ? "" : "  ") + k + "=" + cli_setting_get(settings_, k);
        }
        return n ? std::to_string(n) + ": " + r : "no setting contains \"" + arg + "\"";
    }
    if (name == "get") {
        const std::string k = ascii_lower_str(arg);
        if (k.empty()) return fail("get <what>: volume state title artist album position duration queue mode muted, or a setting (settings)");
        const TrackMetadata md = has_track_ ? display_metadata() : TrackMetadata{};
        if (k == "volume") return std::to_string(player_.volume());
        if (k == "state") return !has_track_ ? "stopped" : player_.is_paused() ? "paused" : "playing";
        if (k == "title") return has_track_ ? md.name : std::string();
        if (k == "artist") return has_track_ && md.artist != "-" ? md.artist : std::string();
        if (k == "album") return has_track_ && md.album != "-" ? md.album : std::string();
        if (k == "position") return std::to_string(static_cast<int>(has_track_ ? player_.poll_elapsed() : 0.0));
        if (k == "duration") return std::to_string(has_track_ ? total_sec_ : static_cast<size_t>(0));
        if (k == "queue") return std::to_string(queue_.size());
        if (k == "mode") return play_mode_name(settings_.play_mode);
        if (k == "muted") return muted_ ? "true" : "false";
        bool found = false;
        const std::string v = cli_setting_get(settings_, arg, &found);
        if (!found) return fail("unknown value \"" + arg + "\"");
        return v;
    }
    if (name == "set") {
        const std::vector<std::string> w = cli::split_words(arg);
        if (w.size() < 2) return fail("set <setting> <value>  (settings lists them)");
        std::string canon;
        const std::string err = cli_setting_set(settings_, w[0], w[1], &canon);
        if (!err.empty()) return fail(err);
        save_settings(settings_);
        cover_pic_key_.clear();
        force_redraw_ = true;
        return canon + "=" + cli_setting_get(settings_, canon) + " (saved)";
    }
    if (name == "open") {
        const std::string w = ascii_lower_str(arg);
        if (mode_ != Mode::Browse && mode_ != Mode::Search && mode_ != Mode::Console) return fail("close the open screen first (ESC)");
        if (w == "console") mode_ = Mode::Console;
        else if (w == "settings") { settings_open(); mode_ = Mode::Settings; settings_tab_ = 0; settings_row_ = 0; settings_col_ = 0; }
        else if (w == "cheatsheet" || w == "keys") { mode_ = Mode::Cheatsheet; cheat_->searching = false; }
        else if (w == "wizard") wizard_open();
        else if (w == "history") history_open();
        else if (w == "queue") queue_overlay_open();
        else if (w == "folders") folder_overlay_open();
        else if (w == "scripts" || w == "script") cliov_open("", "");
        else if (w == "equalizer") eq_open();
        else if (w == "playlist" || w == "playlists") playlist_open_editor();
        else return fail("open console | settings | cheatsheet | wizard | history | queue | folders | equalizer | playlist | scripts");
        return "opened " + w;
    }
    if (name == "edit") {
        if (mode_ != Mode::Browse && mode_ != Mode::Search && mode_ != Mode::Console) return fail("close the open screen first (ESC)");
        const std::string n = cli::unquote(arg);
        if (!n.empty() && !cli::valid_name(n)) return fail("not a script name: " + n);
        cliov_open(n, "");
        return n.empty() ? "script overlay" : "editing " + n;
    }
    if (name == "rescan") { rescan_library(); return "scanning the music folders"; }
    // "Title - Artist  01:23/03:45  vol 60%": what the playback commands answer with (the user sees what happened, not just "ok")
    auto track_line = [&]() -> std::string {
        if (!has_track_) return std::string();
        const TrackMetadata md = display_metadata();
        return md.name + (md.artist != "-" && !md.artist.empty() ? " - " + md.artist : std::string())
               + "  " + clock_text(player_.poll_elapsed()) + "/" + clock_text(static_cast<double>(total_sec_))
               + "  vol " + std::to_string(player_.volume()) + "%" + (muted_ ? " (muted)" : "");
    };
    auto with_artist = [](const std::string& a) { return a.empty() || a == "-" ? std::string() : " - " + a; };
    auto state_word = [&]() -> std::string { return !has_track_ ? "stopped" : player_.is_paused() ? "paused" : "playing"; };
    if (name == "status") {
        if (!has_track_) return std::string("nothing playing  vol ") + std::to_string(player_.volume()) + "%  queue " + std::to_string(queue_.size());
        return state_word() + ": " + track_line() + "  mode " + play_mode_name(settings_.play_mode) + "  queue " + std::to_string(queue_.size());
    }
    if (name == "play" && !arg.empty() && !numeric_arg(arg)) {
        if (audiobook_) return fail("in the audio book player use: book-open <n>, track <n>");
        const std::vector<LocalTrack> hits = filter_and_rank_local_view(cli::unquote(arg));
        if (hits.empty()) return fail("no local title matches \"" + arg + "\"");
        start_local_track(hits.front());
        return "playing " + hits.front().title + with_artist(hits.front().folder_artist) + (hits.size() > 1 ? "  (+" + std::to_string(hits.size() - 1) + " more matches)" : std::string());
    }
    if (name == "play") {
        if (!has_track_) return fail("nothing is loaded: play <words> plays a title, track <n> plays title n of the list");
        if (!player_.is_paused()) return "already playing: " + track_line();
        remote_apply({"play", ""});
        return "resumed: " + track_line();
    }
    if (name == "queue") {
        std::string sub = arg, rest;
        if (const size_t sp = arg.find_first_of(" \t"); sp != std::string::npos) { sub = arg.substr(0, sp); rest = cli::unquote(cli::trim(arg.substr(sp + 1))); }
        const std::string ls = ascii_lower_str(sub);
        if (arg.empty() || ls == "list") {
            std::string r = "queue: " + std::to_string(queue_.size()) + (queue_.size() == 1 ? " title" : " titles");
            for (size_t i = 0; i < queue_.size() && i < 4; ++i) r += "\n  " + std::to_string(i + 1) + ". " + queue_[i].title;
            if (queue_.size() > 4) r += "\n  ...";
            return r;
        }
        if (ls == "clear") {
            if (!forced && job.origin != Origin::Typed) return fail("queue clear needs `force queue clear` in a script or from outside");
            if (!forced) { cli_confirm_.line = "queue clear"; cli_confirm_.until = std::chrono::steady_clock::now() + std::chrono::seconds(20); return fail("queue clear cannot be undone here: type yes (within 20 s), or force queue clear"); }
            const size_t n = queue_.size();
            remote_apply({"clear-queue", "1"});
            return "queue cleared (" + std::to_string(n) + ")";
        }
        if (ls == "shuffle") { if (queue_.size() < 2) return fail("nothing to shuffle"); queue_shuffle_order(); return "queue shuffled"; }
        if (ls == "remove") {
            const int n = std::atoi(rest.c_str());
            if (n < 1 || n > static_cast<int>(queue_.size())) return fail("queue remove <1.." + std::to_string(queue_.size()) + ">");
            const std::string t = queue_[static_cast<size_t>(n - 1)].title;
            queue_.erase(queue_.begin() + (n - 1));
            clamp_queue_selected();
            return "removed " + t;
        }
        if (audiobook_) return fail("the audio book player has no queue");
        const bool next = ls == "next";
        const std::string q = next ? rest : cli::unquote(arg);
        if (q.empty()) return fail("queue next <words>");
        const std::vector<LocalTrack> hits = filter_and_rank_local_view(q);
        if (hits.empty()) return fail("no local title matches \"" + q + "\"");
        const LocalTrack& t = hits.front();
        QueueItem item{true, t.title, t.folder_artist, t.path, "", false, ""};
        if (next) { const int pos = std::clamp(qst_.next_run, 0, static_cast<int>(queue_.size())); queue_.insert(queue_.begin() + pos, std::move(item)); ++qst_.next_run; }
        else queue_.push_back(std::move(item));
        clamp_queue_selected();
        return std::string(next ? "queued next: " : "queued ") + t.title + with_artist(t.folder_artist) + "  (queue " + std::to_string(queue_.size()) + ")";
    }
    if (name == "search") {
        if (audiobook_) return fail("search the books with / (plain words)");
        const std::string q = cli::unquote(arg);
        last_local_query_ = q;
        pre_search_local_query_ = q;   // (ESC of the search bar then keeps the filter)
        list_source_ = ListSource::Local;
        refresh_local_view();
        return q.empty() ? "filter cleared: " + std::to_string(local_view_.size()) + " titles" : std::to_string(local_view_.size()) + " matches for \"" + q + "\"";
    }
    if (name == "playlists") {
        const std::vector<PlaylistSummary> pls = playlist_summaries();
        if (pls.empty()) return "no saved playlists";
        std::string r;
        for (size_t i = 0; i < pls.size() && i < 12; ++i) r += (i ? "\n" : "") + std::to_string(i + 1) + ". " + pls[i].name;
        if (pls.size() > 12) r += "\n... (" + std::to_string(pls.size()) + " in all)";
        return r;
    }
    // the rest: the commands of `mousiki --cmd`; each answers with what happened (state, title, position, volume)
    if (name == "shuffle" && audiobook_) return fail("no shuffle in the audio book mode");
    const bool had = has_track_;
    const bool was_paused = had && player_.is_paused();
    const std::string was_title = had ? display_metadata().name : std::string();
    const int was_vol = player_.volume();
    const bool was_muted = muted_;
    const size_t was_queue = queue_.size();
    const int was_mode = settings_.play_mode;
    remote_apply({name, arg});
    if (name == "quit") return "bye";
    const std::string tl = track_line();
    if (name == "pause" || name == "stop") {
        if (!had) return "nothing is playing";
        const std::string what = name == "stop" ? "paused (stop pauses: the position is kept, `play` resumes): " : "paused: ";
        return (was_paused ? "already " + what : what) + tl;
    }
    if (name == "play-pause") return !had ? fail("nothing is loaded: play <words> plays a title") : (player_.is_paused() ? "paused: " : "playing: ") + tl;
    if (name == "next" || name == "prev") {
        if (!has_track_) return std::string(name == "next" ? "no next title" : "no previous title") + (was_queue ? "" : " (queue empty)");
        return std::string(name == "next" ? "next: " : "previous: ") + tl + (was_title.empty() || was_title == display_metadata().name ? "" : "  (was: " + was_title + ")");
    }
    if (name == "shuffle") return has_track_ ? "shuffled to: " + tl + "  (queue " + std::to_string(queue_.size()) + ")" : fail("nothing to shuffle (empty list)");
    if (name == "seek") {
        if (!had) return fail("nothing is playing: seek needs a loaded title");
        const double d = remote::number(arg, 0.0);
        return std::string("seek ") + (d >= 0 ? "+" : "") + std::to_string(static_cast<int>(d)) + " s -> " + tl;
    }
    if (name == "volume" || name == "volume-up" || name == "volume-down") {
        if (!had) return "volume unchanged: nothing is playing (the volume applies to the next title)";
        return "volume " + std::to_string(was_vol) + "% -> " + std::to_string(player_.volume()) + "%" + (muted_ ? " (muted)" : "");
    }
    if (name == "mute" || name == "unmute" || name == "toggle-mute")
        return std::string(muted_ ? "muted" : "sound on") + (was_muted == muted_ ? " (was already)" : "") + "  volume " + std::to_string(player_.volume()) + "%";
    if (name == "playback-mode")
        return std::string("playback mode: ") + play_mode_name(was_mode) + " -> " + play_mode_name(settings_.play_mode);
    if (name == "clear-queue") return was_queue ? "queue cleared (" + std::to_string(was_queue) + (was_queue == 1 ? " title)" : " titles)") : std::string("the queue is empty");
    if (name == "wizard") return mode_ == Mode::Wizard ? "wizard opened (ESC closes it)" : "wizard closed";
    if (name == "switch-mode") return std::string("switching to the ") + (remote::number(arg, 1) < 0 ? "previous" : "next") + " mode of the program";
    if (name == "track" || name == "book-open") {
        if (!has_track_) return fail(std::string(name) + " " + arg + ": no such row in the list");
        return "playing: " + tl;
    }
    if (name == "playlist") {
        const size_t n = queue_.size();
        return "playlist " + arg + " queued: " + std::to_string(n) + (n == 1 ? " title" : " titles") + (has_track_ ? "  now: " + tl : std::string());
    }
    if (name == "remote-view" || name == "remote-focus" || name == "remote-scroll") return "phone screen: " + name + " " + arg;
    return "ok: " + name + (arg.empty() ? std::string() : " " + arg);
}

// CTRL+RIGHT in a command line (not TAB: that moves between the console's panes). The first word is completed to a command or an alias
// (several candidates: up to their common start, the candidates in the reply); after `play` / `queue` / `search` the text is completed to
// the title of the best local match; after `help` to a command; after `run` to a script; after `set` / `get` to a setting.
std::string App::cli_complete(const std::string& line_in) {
    cli_reply_.clear();
    size_t lead = 0;
    while (lead < line_in.size() && line_in[lead] == ' ') ++lead;
    const std::string line = line_in.substr(lead);
    if (!cli_aliases_loaded_) { cli_aliases_ = cli::load_aliases(); cli_aliases_loaded_ = true; }
    auto complete_from = [&](const std::string& w, const std::vector<std::string>& pool, std::string& out) {
        const std::string lw = ascii_lower_str(w);
        std::vector<std::string> hit;
        for (const std::string& k : pool) if (ascii_lower_str(k).rfind(lw, 0) == 0) hit.push_back(k);
        if (hit.empty()) { cli_reply_ = "nothing starts with \"" + w + "\""; return false; }
        std::string common = hit.front();
        for (const std::string& h : hit) { size_t n = 0; while (n < common.size() && n < h.size() && ascii_lower_str(common.substr(n, 1)) == ascii_lower_str(h.substr(n, 1))) ++n; common.resize(n); }
        out = common + (hit.size() == 1 ? " " : "");
        if (hit.size() > 1) { for (size_t i = 0; i < hit.size() && i < 8; ++i) cli_reply_ += (i ? "  " : "") + hit[i]; if (hit.size() > 8) cli_reply_ += " ..."; }
        return true;
    };
    std::vector<std::string> words;
    for (const Cmd& c : kCmds) words.push_back(c.name);
    for (const auto& [k, v] : cli_aliases_) { (void)v; words.push_back(k); }
    const size_t sp = line.find(' ');
    std::string out;
    if (sp == std::string::npos) {
        if (line.empty() || !complete_from(line, words, out)) return "";
        if (out == line) { for (const std::string& k : words) if (ascii_lower_str(line) == ascii_lower_str(k)) return line + " "; return ""; }   // (already the whole word: the space after it)
        return out;
    }
    const std::string cmd = ascii_lower_str(line.substr(0, sp));
    const std::string arg = cli::trim(line.substr(sp + 1));
    if (cmd == "help") { std::vector<std::string> p; for (const Cmd& c : kCmds) p.push_back(c.name); if (arg.empty() || !complete_from(arg, p, out)) return ""; return "help " + out; }
    if (cmd == "run" || cmd == "edit") { if (arg.empty() || !complete_from(arg, cli::script_names(), out)) return ""; return cmd + " " + out; }
    if (cmd == "set" || cmd == "get") {
        if (arg.find(' ') != std::string::npos || arg.empty()) return "";
        if (!complete_from(arg, cli_setting_names(), out)) return "";
        return cmd + " " + out;
    }
    if ((cmd == "play" || cmd == "queue" || cmd == "q" || cmd == "search" || cmd == "s") && !arg.empty() && !audiobook_) {
        const std::vector<LocalTrack> hits = filter_and_rank_local_view(arg);
        if (hits.empty()) { cli_reply_ = "no local title matches \"" + arg + "\""; return ""; }
        cli_reply_ = hits.size() > 1 ? std::to_string(hits.size()) + " matches" : std::string();
        return cmd + " " + hits.front().title;
    }
    return "";
}

}  // namespace muisc
