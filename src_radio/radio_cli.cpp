// The command line of the radio (`mousiki --cli`, `mousiki --query`, /api/cli of the remote app, scripts): the same language as in the player
// -- chains with ';', `wait`, `in`, `if`, `run` of scripts from <config>/cli, `-` before a command to go on after an error -- with the
// radio's own commands (tune, stations, live ...). Every command answers with what happened. Typed lines are not a thing here (no command
// line pane): everything comes from outside, so there is no `yes` / `force`; `quit` is refused (`mousiki --cmd quit` does it).
#include "radio_app.h"

#include "cli_core.h"
#include "console_log.h"

#include "path_utf8.h"
#include <cmath>
#include <sstream>

using namespace muisc::radio;
namespace cli = muisc::cli;

namespace {

struct RCmd { const char* name; const char* usage; const char* help; };
const RCmd kRadioCmds[] = {
    {"help", "[command]", "this list, or one command"},
    {"status", "", "what plays: station, song, volume, live / paused / behind"},
    {"play", "", "tune the station under the cursor, or resume a paused / muted one"},
    {"pause", "", "pause the live stream (it keeps buffering; play continues where you were)"},
    {"stop", "", "stop the station"},
    {"play-pause", "", "pause / resume"},
    {"next", "", "next station"},
    {"prev", "", "previous station"},
    {"shuffle", "", "a random station"},
    {"tune", "<n | words>", "tune station n of the list, or the first one whose name has all the words"},
    {"stations", "[words]", "the stations (with their numbers), or those matching the words"},
    {"live", "", "back to the live edge after a pause or a rewind"},
    {"seek", "<seconds>", "jump back (-30) or forward in the buffered stream"},
    {"volume", "[+n | -n | n]", "volume (alone: show it)"},
    {"volume-up", "[n]", "louder (default 5)"},
    {"volume-down", "[n]", "quieter (default 5)"},
    {"mute", "", "mute"},
    {"unmute", "", "sound on"},
    {"toggle-mute", "", "mute / unmute"},
    {"playback-mode", "", "station order of next / prev: list <-> shuffle"},
    {"get", "<what>", "volume state station title artist muted behind"},
    {"echo", "<text>", "print text (for scripts)"},
    {"wait", "<time>", "a script waits: 5, 5s, 2m, 1h30m"},
    {"in", "<time> <command>", "run a command later: in 30m stop"},
    {"timers", "", "the commands waiting for their time"},
    {"cancel", "[n | all]", "cancel a timer (or all)"},
    {"if", "<condition> <command>", "playing paused stopped muted unmuted volume>50 ... (! negates)"},
    {"run", "<script>", "run a script from the cli folder"},
    {"scripts", "", "the scripts in the cli folder"},
    {"open", "<where>", "open console, scripts, settings, history, cheatsheet"},
    {"edit", "[script]", "the script overlay (an editor for the scripts of the cli folder)"},
    {"alias", "[name = commands]", "your own short name for commands (alias night = volume 25; stop); alone: list them"},
    {"unalias", "<name>", "remove an alias"},
    {"hooks", "", "the event hooks of hooks.cli (on station-change = ...)"},
    {"settings", "[words]", "the settings `set` can change, with their values"},
    {"set", "<setting> <on|off|toggle>", "change a setting and save it (not over the network)"},
};

// The settings `set` / `get` can reach: the on / off rows of the radio (no texts, colours, paths, keys, remote app address / port / PIN).
struct RSetting { const char* key; bool RadioSettings::*field; };
const RSetting kRadioSettings[] = {
    {"OnAirAscii", &RadioSettings::on_air_ascii}, {"PulseWave", &RadioSettings::pulse_wave}, {"DummyButtons", &RadioSettings::dummy_buttons},
    {"Visualizer", &RadioSettings::element_visualizer}, {"StereoSound", &RadioSettings::stereo}, {"NormalizeVolume", &RadioSettings::normalize},
    {"TuneNoise", &RadioSettings::tune_noise}, {"MarqueeScrolling", &RadioSettings::marquee}, {"UseOscilloscope", &RadioSettings::use_osci},
    {"UseSpectrogram", &RadioSettings::use_spectro},
};
const RSetting* find_rsetting(const std::string& key) {
    std::string k = key;
    for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const RSetting& e : kRadioSettings) {
        std::string n = e.key;
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (n == k) return &e;
    }
    return nullptr;
}
bool is_radio_command(const std::string& name) {
    for (const RCmd& c : kRadioCmds) if (name == c.name) return true;
    return false;
}

const std::pair<const char*, const char*> kRadioAlias[] = {
    {"pp", "play-pause"}, {"toggle", "play-pause"}, {"previous", "prev"}, {"skip", "next"}, {"vol", "volume"}, {"v", "volume"}, {"m", "mute"},
    {"?", "help"}, {"now", "status"}, {"sleep", "wait"}, {"st", "stations"}, {"t", "tune"},
};

std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

}  // namespace

cli::State RadioApp::cli_state() const {
    const RadioStatus rs = engine.status();
    cli::State st;
    st.has_track = rs.state != StreamState::Idle && rs.state != StreamState::Failed;
    st.paused = st.has_track && rs.timeshift_paused;
    st.muted = rs.muted;
    st.volume = rs.volume;
    st.queue = 0;
    st.position = 0;
    st.duration = 0;
    return st;
}

std::string RadioApp::cli_station_line() const {
    const RadioStatus rs = engine.status();
    if (rs.state == StreamState::Idle) return "stopped";
    if (rs.state == StreamState::Failed) return "failed: " + (rs.tuned_name.empty() ? std::string("station") : rs.tuned_name);
    std::string r = rs.timeshift_paused ? "paused: " : rs.state == StreamState::Live ? "playing: " : rs.state == StreamState::Reconnecting ? "reconnecting: " : "connecting: ";
    r += rs.tuned_name.empty() ? rs.info.station : rs.tuned_name;
    if (!rs.info.title.empty()) r += "  \"" + (rs.info.artist.empty() ? std::string() : rs.info.artist + " - ") + rs.info.title + "\"";
    r += "  vol " + std::to_string(rs.volume) + "%" + (rs.muted ? " (muted)" : "");
    if (rs.timeshift_paused) r += "  paused at " + cli::duration_text(rs.behind_sec) + " behind live";
    else if (rs.behind_sec >= 2.0) r += "  " + cli::duration_text(rs.behind_sec) + " behind live (`live` jumps to the live edge)";
    return r;
}

std::string RadioApp::cli_run(const std::string& raw, cli::Origin origin, const std::string& reply_id) {
    const std::string line = cli::trim(raw);
    if (line.empty()) return "";
    cli::Job job;
    job.origin = origin;
    job.reply_id = reply_id;
    job.label = line;
    job.lines.push_back(line);
    cli_step(job);
    std::string reply = job.out.empty() ? std::string() : job.out.back();
    if (job.lines.empty() && !job.waiting) cli_finish(job); else cli_jobs.push_back(std::move(job));
    return reply;
}

void RadioApp::cli_finish(cli::Job& job) {
    if (job.reply_id.empty()) return;
    std::string all;
    for (size_t i = 0; i < job.out.size(); ++i) all += (i ? "\n" : "") + job.out[i];
    muisc::remote::reply(job.reply_id, !job.failed, all);
}

void RadioApp::cli_step(cli::Job& job) {
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
        if (parts.size() > 1) {
            for (size_t i = parts.size(); i-- > 0;) job.lines.push_front((soft ? "-" : "") + parts[i]);
            continue;
        }
        bool ok = true;
        const std::string reply = cli_exec(parts.empty() ? line : parts[0], job, ok);
        const std::string shown = ok ? reply : "error: " + reply;
        job.out.push_back(shown);
        auto to_rows = [&](std::vector<std::string>& rows, size_t cap) {
            std::istringstream in(shown);
            for (std::string l; std::getline(in, l);) rows.push_back("  " + l);
            while (rows.size() > cap) rows.erase(rows.begin());
        };
        if (job.origin == cli::Origin::Typed) to_rows(ui.model.cli.out, 200);   // the pane of "c:" and the console show the answers
        else if (!ok) muisc::ConsoleLog::instance().log_basic("CLI error: " + reply + "  [" + job.label + "]");
        if (job.to_overlay) to_rows(ui.model.cliov.out, 100);                  // the script overlay shows the answers at the bottom of its editor
        if (!ok && !soft) { job.failed = true; job.lines.clear(); break; }
        if (job.waiting) break;
    }
}

// Looks at what the radio does (a new station, pause / resume, the stream failing, the station stopped) and runs the matching lines of
// hooks.cli as a script (Origin::Script). One compare per frame; the file is read only when an event happened. A hook that causes its own
// event (on pause = play) is cut off by the limiter (8 runs in 10 s).
void RadioApp::cli_hooks_tick() {
    const RadioStatus rs = engine.status();
    CliHookSnap now;
    now.valid = true;
    now.tuned = rs.state != StreamState::Idle && rs.state != StreamState::Failed;
    now.paused = now.tuned && rs.timeshift_paused;
    now.url = now.tuned ? rs.tuned_url : std::string();
    const auto t = std::chrono::steady_clock::now();
    if (cli_hook_snap.valid) {
        const CliHookSnap& o = cli_hook_snap;
        std::vector<std::string> ev;
        if (now.tuned && (!o.tuned || o.url != now.url)) { cli_hook_station_pending = true; cli_hook_station_at = t; }
        else if (now.tuned && o.tuned && now.paused != o.paused) ev.push_back(now.paused ? "pause" : "resume");
        if (o.tuned && !now.tuned) ev.push_back(rs.state == StreamState::Failed ? "stream-error" : "idle");

        cli_hook_snap = now;
        // the new station counts once it has been there for half a second (a `volume` in the hook sticks)
        if (cli_hook_station_pending && (!now.tuned || t - cli_hook_station_at >= std::chrono::milliseconds(500))) {
            cli_hook_station_pending = false;
            if (now.tuned) ev.insert(ev.begin(), "station-change");
        }
        for (const std::string& e : ev) cli_fire_hook(e);
    } else cli_hook_snap = now;
}

void RadioApp::cli_fire_hook(const std::string& event) {
    std::vector<std::string> lines;
    std::string err;
    if (!cli::read_script("hooks", lines, err)) return;
    const std::vector<cli::Hook> hooks = cli::parse_hooks(lines, true);
    std::vector<std::string> run;
    for (const cli::Hook& h : hooks) if (h.event == event) run.push_back(h.chain);
    if (run.empty()) return;
    if (!cli_hook_limit.allow(std::chrono::steady_clock::now())) {
        if (!cli_hook_warned) { cli_hook_warned = true; muisc::ConsoleLog::instance().log_basic("CLI: hooks paused: more than 8 runs in 10 s (does a hook cause its own event?)"); }
        return;
    }
    cli_hook_warned = false;
    const RadioStatus rs = engine.status();
    const bool tuned = rs.state != StreamState::Idle && rs.state != StreamState::Failed;
    const std::map<std::string, std::string> vals = {
        {"station", tuned ? (rs.tuned_name.empty() ? rs.info.station : rs.tuned_name) : std::string()},
        {"title", rs.info.title}, {"artist", rs.info.artist},
        {"state", !tuned ? "stopped" : rs.timeshift_paused ? "paused" : "playing"}, {"volume", std::to_string(rs.volume)}};
    for (const std::string& chain : run) {
        cli::Job job;
        job.origin = cli::Origin::Script;
        job.label = "on " + event;
        job.lines.push_back(cli::expand_hook(chain, vals));
        muisc::ConsoleLog::instance().log_basic("CLI hook " + event + ": " + job.lines.front());
        cli_step(job);
        if (job.lines.empty() && !job.waiting) cli_finish(job); else cli_jobs.push_back(std::move(job));
    }
}

void RadioApp::cli_tick() {
    cli_hooks_tick();
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < cli_timers.size();) {
        if (cli_timers[i].at > now) { ++i; continue; }
        const cli::Timer t = cli_timers[i];
        cli_timers.erase(cli_timers.begin() + static_cast<long>(i));
        cli_run(t.line, t.origin == cli::Origin::Typed ? cli::Origin::Script : t.origin);
    }
    for (size_t i = 0; i < cli_jobs.size();) {
        cli::Job& j = cli_jobs[i];
        if (j.waiting && j.wait_until > now) { ++i; continue; }
        cli_step(j);
        if (j.lines.empty() && !j.waiting) { cli_finish(j); cli_jobs.erase(cli_jobs.begin() + static_cast<long>(i)); }
        else ++i;
    }
}

std::string RadioApp::cli_exec(const std::string& line_in, cli::Job& job, bool& ok) {
    const std::string line = cli::trim(line_in);
    auto fail = [&](const std::string& m) { ok = false; return m; };
    std::string name = line, arg;
    if (const size_t sp = line.find_first_of(" \t"); sp != std::string::npos) { name = line.substr(0, sp); arg = cli::trim(line.substr(sp + 1)); }
    name = lower(name);
    if (!name.empty() && name[0] == '/') name.erase(0, 1);
    for (const auto& a : kRadioAlias) if (name == a.first) { name = a.second; break; }
    const bool net = job.origin == cli::Origin::Network;
    if (name == "quit" || name == "exit") {
        if (job.origin == cli::Origin::Typed) return fail("quit is not a command of the radio command line: close the radio with [" + window_key_label("Quit") + "] (`mousiki --cmd quit` does it from outside)");
        return fail("quit is not a command of the radio command line: use `mousiki --cmd quit` (there is nothing here to confirm it)");
    }
    if (net && (name == "set" || name == "alias" || name == "unalias" || name == "edit" || name == "force" || name == "open")) return fail(name + " is not allowed over the network");
    // the aliases of the user (aliases.cli): a name that is none of the commands (a command is never shadowed)
    if (!cli_aliases_loaded) { cli_aliases = cli::load_aliases(); cli_aliases_loaded = true; }
    if (!is_radio_command(name)) {
        const auto it = cli_aliases.find(name);
        if (it != cli_aliases.end()) {
            const std::vector<std::string> body = cli::split_chain(it->second);
            if (body.empty()) return fail("empty alias " + name);
            for (size_t i = body.size(); i-- > 0;) job.lines.push_front(i + 1 == body.size() && !arg.empty() ? body[i] + " " + arg : body[i]);
            return "alias " + name;
        }
    }
    if (name == "alias" || name == "unalias") {
        cli_aliases = cli::load_aliases();
        if (name == "unalias") {
            if (!cli_aliases.erase(arg)) return fail("no alias " + arg);
            cli::save_aliases(cli_aliases);
            return "removed alias " + arg;
        }
        if (arg.empty()) {
            if (cli_aliases.empty()) return "no aliases (alias night = volume 25; wait 1s; stop)";
            std::string r;
            for (const auto& [k, v] : cli_aliases) r += (r.empty() ? "" : "\n") + k + " = " + v;
            return r;
        }
        const size_t eq = arg.find('=');
        if (eq == std::string::npos) return fail("alias <name> = <commands>");
        const std::string k = cli::trim(arg.substr(0, eq)), v = cli::trim(arg.substr(eq + 1));
        std::string kl = lower(k);
        bool builtin = is_radio_command(kl);
        for (const auto& a : kRadioAlias) if (kl == a.first) builtin = true;
        if (!cli::valid_name(k) || builtin || kl == "quit" || kl == "exit") return fail("\"" + k + "\" is a command or not a name (letters, digits, - _ .)");
        if (v.empty()) return fail("alias <name> = <commands>");
        cli_aliases[k] = v;
        if (!cli::save_aliases(cli_aliases)) return fail("could not save " + path_utf8(cli::dir() / "aliases.cli"));
        return "alias " + k + " = " + v;
    }
    if (name == "hooks") {
        std::vector<std::string> hl;
        std::string err, evs;
        for (const std::string& e : cli::hook_events(true)) evs += (evs.empty() ? "" : " ") + e;
        if (!cli::read_script("hooks", hl, err)) return "no hooks yet. Write them with `edit hooks`, one per line:  on <event> = <commands>   (events: " + evs + ";  {station} {title} {artist} {state} {volume} are filled in)";
        const std::vector<cli::Hook> hs = cli::parse_hooks(hl, true);
        if (hs.empty()) return "hooks.cli has no usable line for the radio. Format:  on <event> = <commands>   (events: " + evs + ")";
        std::string r = std::to_string(hs.size()) + (hs.size() == 1 ? " hook:" : " hooks:");
        for (const cli::Hook& h : hs) r += "\n  on " + h.event + " = " + h.chain;
        return r;
    }
    if (name == "settings") {
        std::string r;
        int n = 0;
        const std::string f = lower(arg);
        for (const RSetting& e : kRadioSettings) {
            if (!f.empty() && lower(e.key).find(f) == std::string::npos) continue;
            ++n;
            r += (r.empty() ? "" : "  ") + std::string(e.key) + "=" + (cfg.*(e.field) ? "true" : "false");
        }
        return n ? std::to_string(n) + ": " + r : "no setting contains \"" + arg + "\"";
    }
    if (name == "set") {
        const std::vector<std::string> w = cli::split_words(arg);
        if (w.size() < 2) return fail("set <setting> <on | off | toggle>  (settings lists them)");
        const RSetting* e = find_rsetting(w[0]);
        if (!e) return fail("unknown setting \"" + w[0] + "\" (settings lists the ones that can be set)");
        const std::string v = lower(w[1]);
        bool& f = cfg.*(e->field);
        if (v == "toggle") f = !f;
        else if (v == "true" || v == "on" || v == "yes" || v == "1") f = true;
        else if (v == "false" || v == "off" || v == "no" || v == "0") f = false;
        else return fail(std::string(e->key) + " is on / off (or toggle)");
        apply_audio();
        std::string err;
        if (!save_radio_settings(cfg, &err)) return fail("changed, but not saved: " + err);
        return std::string(e->key) + "=" + (f ? "true" : "false") + " (saved)";
    }

    if (name == "help") {
        if (!arg.empty()) {
            const std::string a = lower(arg);
            for (const RCmd& c : kRadioCmds) if (a == c.name) return std::string(c.name) + (*c.usage ? " " : "") + c.usage + "  --  " + c.help;
            return fail("no command " + arg + " (help lists them)");
        }
        std::string r = "radio commands (`;` chains them, `wait 5s` pauses a script, `-cmd` goes on after an error):";
        for (const RCmd& c : kRadioCmds) r += std::string("\n  ") + c.name + (*c.usage ? " " : "") + c.usage + "  --  " + c.help;
        return r;
    }
    if (name == "open" || name == "edit") {   // the screens of the radio that belong to the command line
        UiModel& um = ui.model;
        const bool busy = um.cheat_open || um.wizard_open || um.hmenu.open || um.settings.open || um.menu.open || um.browse.open || um.lmenu.open || um.stov.open ||
                          um.overlay != 0 || um.scope_big || um.spectro_full || um.cliov.open;
        if (busy) return fail("close the open screen first (ESC)");
        if (name == "edit") {
            const std::string n = cli::unquote(arg);
            if (!n.empty() && !cli::valid_name(n)) return fail("not a script name: " + n);
            cliov_open(n, "");
            return n.empty() ? "script overlay" : "editing " + n;
        }
        const std::string w = lower(arg);
        if (w == "console") console_open(0);
        else if (w == "scripts" || w == "script") cliov_open("", "");
        else if (w == "settings") settings_open();
        else if (w == "history") { um.hmenu.open = true; um.hmenu.flash.clear(); um.hmenu.cursor = 0; um.hmenu.top_cursor = 0; um.hmenu.scroll = 0; }
        else if (w == "cheatsheet" || w == "keys") { um.cheat_open = true; um.cheat_scroll = 0; }
        else return fail("open console | scripts | settings | history | cheatsheet");
        return "opened " + w;
    }
    if (name == "status") return cli_station_line() + "  mode " + (ui.model.shuffle ? "shuffle" : "list");
    if (name == "echo") return arg;
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
        if (sp == std::string::npos || !cli::parse_duration(arg.substr(0, sp), sec)) return fail("in <time> <command>, e.g. in 30m stop (at most 24h)");
        const std::string what = cli::trim(arg.substr(sp + 1));
        if (what.empty()) return fail("in <time> <command>");
        if (cli_timers.size() >= 50) return fail("50 timers are waiting: cancel some");
        cli::Timer t;
        t.at = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(sec * 1000));
        t.line = what;
        t.origin = job.origin;
        t.id = ++cli_timer_serial;
        cli_timers.push_back(t);
        return "timer " + std::to_string(t.id) + ": in " + cli::duration_text(sec) + " -- " + what;
    }
    if (name == "timers") {
        if (cli_timers.empty()) return "no timers";
        std::string r;
        const auto now = std::chrono::steady_clock::now();
        for (const cli::Timer& t : cli_timers) r += (r.empty() ? "" : "\n") + std::string("timer ") + std::to_string(t.id) + ": in " + cli::duration_text(std::max(0.0, std::chrono::duration<double>(t.at - now).count())) + " -- " + t.line;
        return r;
    }
    if (name == "cancel") {
        if (arg.empty() || lower(arg) == "all") { const size_t n = cli_timers.size(); cli_timers.clear(); return "cancelled " + std::to_string(n) + (n == 1 ? " timer" : " timers"); }
        const int id = std::atoi(arg.c_str());
        for (size_t i = 0; i < cli_timers.size(); ++i) if (cli_timers[i].id == id) { cli_timers.erase(cli_timers.begin() + static_cast<long>(i)); return "cancelled timer " + std::to_string(id); }
        return fail("no timer " + arg);
    }
    if (name == "if") {
        std::vector<std::string> w = cli::split_words(arg);
        std::string cond;
        size_t k = 0;
        for (; k < w.size(); ++k) {
            const std::string lw = lower(w[k]);
            const std::string probe = cond.empty() ? w[k] : cond + " " + w[k];
            bool cok = false;
            cli::eval_condition(probe, cli_state(), cok);
            if (cok) { cond = probe; ++k; break; }
            if (lw == "not" || lw == "!") { cond = cond.empty() ? "!" : cond + " !"; continue; }
            return fail("if: not a condition: " + probe + " (playing paused stopped muted unmuted volume>50 ...)");
        }
        if (k >= w.size()) return fail("if <condition> <command>");
        bool cok = true;
        const bool yes = cli::eval_condition(cond, cli_state(), cok);
        const size_t pos = arg.find(w[k]);
        const std::string rest = pos == std::string::npos ? w[k] : arg.substr(pos);
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
    if (name == "get") {
        const std::string k = lower(arg);
        const RadioStatus rs = engine.status();
        if (const RSetting* e = find_rsetting(arg)) return cfg.*(e->field) ? "true" : "false";
        if (k == "volume") return std::to_string(rs.volume);
        if (k == "state") return rs.state == StreamState::Idle ? "stopped" : rs.state == StreamState::Failed ? "failed" : rs.timeshift_paused ? "paused" : "playing";
        if (k == "station") return rs.tuned_name.empty() ? rs.info.station : rs.tuned_name;
        if (k == "title") return rs.info.title;
        if (k == "artist") return rs.info.artist;
        if (k == "muted") return rs.muted ? "true" : "false";
        if (k == "behind") return std::to_string(static_cast<int>(rs.behind_sec));
        return fail("get volume | state | station | title | artist | muted | behind, or a setting (settings)");
    }
    if (name == "stations" || name == "tune") {
        const std::vector<std::string> words = cli::split_words(lower(arg));
        auto matches = [&](const Station& s) {
            const std::string n = lower(s.name + " " + s.genre + " " + s.country);
            for (const std::string& w : words) if (n.find(w) == std::string::npos) return false;
            return true;
        };
        if (name == "stations") {
            std::string r;
            int n = 0;
            for (size_t i = 0; i < ui.stations.size(); ++i) {
                if (!matches(ui.stations[i])) continue;
                if (++n <= 10) r += (r.empty() ? "" : "\n") + std::to_string(i + 1) + ". " + ui.stations[i].name + (ui.stations[i].genre.empty() ? "" : "  (" + ui.stations[i].genre + ")");
            }
            if (!n) return words.empty() ? "no stations" : "no station matches \"" + arg + "\"";
            return std::to_string(n) + (n == 1 ? " station" : " stations") + (n > 10 ? " (first 10)" : "") + ":\n" + r;
        }
        if (arg.empty()) return fail("tune <n | words>  (stations lists them)");
        int idx = -1;
        const bool num = std::all_of(arg.begin(), arg.end(), [](unsigned char c) { return std::isdigit(c); });
        if (num) { idx = std::atoi(arg.c_str()) - 1; if (idx < 0 || idx >= static_cast<int>(ui.stations.size())) return fail("no station " + arg + " (1.." + std::to_string(ui.stations.size()) + ")"); }
        else for (size_t i = 0; i < ui.stations.size(); ++i) if (matches(ui.stations[i])) { idx = static_cast<int>(i); break; }
        if (idx < 0) return fail("no station matches \"" + arg + "\"");
        tune_to(idx);
        const Station& s = ui.stations[static_cast<size_t>(idx)];
        return "tuning: " + s.name + (s.genre.empty() ? "" : "  (" + s.genre + (s.country.empty() ? "" : ", " + s.country) + ")") + "  [" + std::to_string(idx + 1) + "/" + std::to_string(ui.stations.size()) + "]";
    }

    // the commands of `mousiki --cmd`, each with an answer that says what happened
    const RadioStatus was = engine.status();
    const bool tuned = was.state != StreamState::Idle && was.state != StreamState::Failed;
    if (name == "pause" && !tuned) return "nothing is tuned";
    if ((name == "seek" || name == "live") && !tuned) return fail("nothing is tuned");
    if (name == "seek" && std::fabs(muisc::remote::number(arg, 0.0)) < 0.5) return fail("seek <seconds>, e.g. seek -30");
    if (name == "volume" && arg.empty()) return "volume " + std::to_string(was.volume) + "%" + (was.muted ? " (muted)" : "");
    const bool known = name == "play" || name == "pause" || name == "stop" || name == "play-pause" || name == "next" || name == "prev" || name == "shuffle" || name == "live" || name == "seek" ||
                       name == "volume" || name == "volume-up" || name == "volume-down" || name == "mute" || name == "unmute" || name == "toggle-mute" || name == "playback-mode";
    if (!known) return fail("unknown command \"" + name + "\" (help lists them)");
    remote_apply({name, arg, "", false});
    const RadioStatus rs = engine.status();
    const std::string where = rs.tuned_name.empty() ? rs.info.station : rs.tuned_name;
    if (name == "stop") return tuned ? "stopped: " + where : "already stopped";
    if (name == "pause") return was.timeshift_paused ? "already paused: " + where : "paused: " + where + " (the stream keeps buffering; `play` continues here, `live` jumps to the live edge)";
    if (name == "play") return !tuned ? "tuning the station under the cursor" : was.timeshift_paused ? "resumed: " + where : was.muted ? "sound on: " + where : "already playing: " + where;
    if (name == "play-pause") return !tuned ? "tuning the station under the cursor" : rs.timeshift_paused ? "paused: " + where : "playing: " + where;
    if (name == "next" || name == "prev" || name == "shuffle") {
        const int ti = rs.tuned_index;
        const std::string now = ti >= 0 && ti < static_cast<int>(ui.stations.size()) ? ui.stations[static_cast<size_t>(ti)].name : where;
        return std::string(name == "next" ? "next station: " : name == "prev" ? "previous station: " : "random station: ") + now;
    }
    if (name == "live") return "back at the live edge: " + where + (was.behind_sec >= 2.0 ? "  (was " + cli::duration_text(was.behind_sec) + " behind)" : std::string());
    if (name == "seek") { const double d = muisc::remote::number(arg, 0.0); return std::string("seek ") + (d >= 0 ? "+" : "") + std::to_string(static_cast<int>(d)) + " s in the buffer of " + where; }
    if (name == "volume" || name == "volume-up" || name == "volume-down") return "volume " + std::to_string(was.volume) + "% -> " + std::to_string(rs.volume) + "%" + (rs.muted ? " (muted)" : "");
    if (name == "mute" || name == "unmute" || name == "toggle-mute") return std::string(rs.muted ? "muted" : "sound on") + (was.muted == rs.muted ? " (was already)" : "") + "  volume " + std::to_string(rs.volume) + "%";
    if (name == "playback-mode") return std::string("station order: ") + (ui.model.shuffle ? "shuffle (next / prev pick random stations)" : "list (next / prev go through the list)");
    return "ok: " + name;
}


// CTRL+RIGHT in the command line: completes the command word. "" = nothing to add; several candidates are listed in the output.
std::string RadioApp::cli_complete(const std::string& line) {
    if (line.find_first_of(" \t") != std::string::npos) return std::string();   // only the command word (the arguments are free text)
    const std::string word = lower(cli::trim(line));
    if (word.empty()) return std::string();
    std::vector<std::string> hits;
    for (const RCmd& c : kRadioCmds) if (std::string(c.name).rfind(word, 0) == 0) hits.push_back(c.name);
    for (const auto& a : kRadioAlias) if (std::string(a.first).rfind(word, 0) == 0 && std::string(a.first).size() > 1) hits.push_back(a.first);
    if (!cli_aliases_loaded) { cli_aliases = cli::load_aliases(); cli_aliases_loaded = true; }
    for (const auto& [k, v] : cli_aliases) { (void)v; if (lower(k).rfind(word, 0) == 0) hits.push_back(k); }
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    CliTyped& c = ui.model.cli;
    if (hits.empty()) { c.out.push_back("  no command starts with \"" + word + "\""); return std::string(); }
    if (hits.size() == 1) return hits[0] + " ";
    std::string common = hits[0];
    std::string list;
    for (const std::string& h : hits) {
        size_t n = 0;
        while (n < common.size() && n < h.size() && common[n] == h[n]) ++n;
        common.resize(n);
        list += (list.empty() ? "" : "  ") + h;
    }
    c.out.push_back("  " + list);
    while (c.out.size() > 200) c.out.erase(c.out.begin());
    return common.size() > word.size() ? common : std::string();
}
