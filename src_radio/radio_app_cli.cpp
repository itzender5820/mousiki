// The typed command line of the radio: "c:" in the SEARCH box, the console overlay (log + command line, SHIFT+c) and the script overlay
// ("cs:" in the SEARCH box, Settings -> CLI Scripts, `open scripts`). The music player has the same three; the language is cli_core.h's,
// the commands are the radio's (radio_cli.cpp). Drawn by radio_ui_cli.cpp.
#include "radio_app.h"

#include "cli_core.h"
#include "console_log.h"
#include "text_edit.h"
#include "terminal_ui.h"

#include <algorithm>
#include <set>
#include <sstream>

using namespace muisc::radio;
namespace cli = muisc::cli;

namespace {
constexpr int kKeyCtrlR = 0x12;
constexpr size_t kMaxLines = 500;
}

// ---- the typed line --------------------------------------------------------------------------------------------------------------

void RadioApp::cli_typed(const std::string& raw) {
    const std::string line = cli::trim(raw);
    if (line.empty()) return;
    CliTyped& c = ui.model.cli;
    c.out.push_back("> " + line);
    if (c.hist.empty() || c.hist.back() != line) c.hist.push_back(line);
    if (c.hist.size() > 100) c.hist.erase(c.hist.begin());
    c.hist_pos = -1;
    muisc::ConsoleLog::instance().log_basic("CLI: " + line);
    cli_run(line, cli::Origin::Typed);
    while (c.out.size() > 200) c.out.erase(c.out.begin());
}

// The key of the SEARCH box while it holds "c:" or "cs:". true = handled (ESC and plain typing are left to key_search()).
bool RadioApp::cli_search_key(KeyCtx& kc) {
    UiModel& um = ui.model;
    const int k = kc.k;
    const bool is_c = search_is_cli(um.search), is_cs = search_is_cs(um.search);
    if (!is_c && !is_cs) return false;
    if (!kc.arrow && k == 27) return false;
    const std::string rest = cli::trim(um.search.substr(um.search.find(':') + 1));
    if (!kc.arrow && (k == 13 || k == 10)) {
        if (is_c) {
            cli_typed(rest);
            um.search = "c:";
            um.search_edit.to_end(um.search);
        } else {   // "cs:words" -- ENTER opens the script overlay with that search
            um.search.clear(); um.search_edit.reset(); um.cursor = 0;
            ui.refilter(); ui.refilter_main_lists(); set_search_focus(false);
            cliov_open("", rest);
        }
        return true;
    }
    if (kc.arrow && (k == 'A' || k == 'B')) {
        if (is_c) {
            CliTyped& c = um.cli;
            if (!c.hist.empty()) {
                const int n = static_cast<int>(c.hist.size());
                c.hist_pos = k == 'A' ? (c.hist_pos < 0 ? n - 1 : std::max(0, c.hist_pos - 1))
                                      : (c.hist_pos < 0 ? -1 : c.hist_pos + 1 >= n ? -1 : c.hist_pos + 1);
                um.search = "c:" + (c.hist_pos < 0 ? std::string() : c.hist[static_cast<size_t>(c.hist_pos)]);
                um.search_edit.to_end(um.search);
            }
        }
        return true;
    }
    if (is_c && k == kKeyCtrlRight) {   // complete the command word (CTRL+RIGHT, not TAB)
        const std::string done = cli_complete(rest);
        if (!done.empty()) { um.search = "c:" + done; um.search_edit.to_end(um.search); }
        return true;
    }
    return false;
}

// ---- the status messages ---------------------------------------------------------------------------------------------------------

// Every status line the radio shows under its hints (the notice of the SEARCH box, the menus' flash lines, the settings' status) is
// remembered with the time when it changes: the console's STATUS MESSAGES box (the player has the same one). Cheap: a few string compares.
void RadioApp::capture_status() {
    UiModel& um = ui.model;
    const std::string* const lines[] = {&um.notice, &um.menu.flash, &um.browse.flash, &um.lmenu.flash, &um.settings.status, &um.hmenu.flash, &um.stov.flash};
    constexpr size_t n = sizeof lines / sizeof lines[0];
    ConsoleModel& c = um.con;
    if (c.seen.size() != n) c.seen.assign(n, std::string());
    for (size_t i = 0; i < n; ++i) {
        const std::string& cur = *lines[i];
        if (cur == c.seen[i]) continue;
        c.seen[i] = cur;
        if (cur.empty()) continue;
        std::string one = cur;
        for (char& ch : one) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        const std::time_t t = std::time(nullptr);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[16];
        std::strftime(buf, sizeof buf, "%H:%M:%S", &tmv);
        c.status.push_back(std::string("[") + buf + "] " + one);
        if (c.status.size() > 500) c.status.erase(c.status.begin(), c.status.begin() + static_cast<std::ptrdiff_t>(c.status.size() - 500));
    }
}

// ---- the console overlay ---------------------------------------------------------------------------------------------------------

void RadioApp::console_open(int pane) {
    ConsoleModel& c = ui.model.con;
    c.open = true;
    c.pane = pane;
    c.up[0] = c.up[1] = c.up[2] = 0;
    c.note.clear();
    set_text_entry(pane == 2);
}

bool RadioApp::key_console(KeyCtx& kc) {
    ConsoleModel& c = ui.model.con;
    if (!c.open) return false;
    const int k = kc.k;
    const bool arrow = kc.arrow;
    const bool key_console_again = !arrow && k == key_of(cfg, "Console");
    if (c.pane == 2) {   // the command line: every key is typed ([C] does not close it: ESC clears the line first, a second ESC closes)
        if (!arrow && k == 27) {
            if (!c.buf.empty()) { c.buf.clear(); c.edit.reset(); ui.model.cli.hist_pos = -1; }
            else { c.open = false; set_text_entry(ui.model.search_focus); }
        } else if (!arrow && k == 9) { c.pane = 0; set_text_entry(false); }   // (the boxes go round: log, status messages, command line)
        else if (!arrow && (k == 13 || k == 10)) { cli_typed(c.buf); c.buf.clear(); c.edit.reset(); }
        else if (arrow && (k == 'A' || k == 'B')) {
            CliTyped& h = ui.model.cli;
            if (!h.hist.empty()) {
                const int n = static_cast<int>(h.hist.size());
                h.hist_pos = k == 'A' ? (h.hist_pos < 0 ? n - 1 : std::max(0, h.hist_pos - 1))
                                      : (h.hist_pos < 0 ? -1 : h.hist_pos + 1 >= n ? -1 : h.hist_pos + 1);
                c.buf = h.hist_pos < 0 ? std::string() : h.hist[static_cast<size_t>(h.hist_pos)];
                c.edit.to_end(c.buf);
            }
        } else if (k == kKeyCtrlRight) {
            const std::string done = cli_complete(c.buf);
            if (!done.empty()) { c.buf = done; c.edit.to_end(c.buf); }
        } else if (edit_text_key(c.buf, c.edit, k, 200, &c.note)) ui.model.cli.hist_pos = -1;
        return true;
    }
    if (!arrow && (k == 27 || key_console_again || k == 'q' || k == 'Q')) { c.open = false; set_text_entry(ui.model.search_focus); }
    else if (!arrow && k == 9) {   // TAB: the next box that is there (log -> status messages -> command line -> log)
        int next = c.pane;
        for (int i = 0; i < 3; ++i) { next = (next + 1) % 3; if (c.has[next]) break; }
        c.pane = next;
        set_text_entry(c.pane == 2);
    }
    else if (!arrow && (k == 'v' || k == 'V')) {
        const bool on = muisc::ConsoleLog::instance().toggle_level() == muisc::LogVerbosity::Verbose;
        ui.model.notice = on ? "verbose log: on (this session)" : "verbose log: off";
        notice_until = ui.model.t_sec + 4;
    } else if (arrow && (k == 'A' || k == 'B')) {
        const int step = last_key_was_shifted() ? 8 : 1;
        c.up[c.pane] = std::max(0, c.up[c.pane] + (k == 'A' ? step : -step));   // (the upper end is clamped while drawing)
    } else if (k == kKeyHome) c.up[c.pane] = 1 << 20;
    else if (k == kKeyEnd) c.up[c.pane] = 0;
    return true;
}

// ---- the script overlay ----------------------------------------------------------------------------------------------------------

void RadioApp::cliov_refresh() {
    CliOvModel& o = ui.model.cliov;
    const std::vector<std::string> found = cli::script_names();
    std::set<std::string> all(found.begin(), found.end());
    all.insert("startup");   // (the two special files are always listed, so they can be created here)
    all.insert("aliases");
    o.names.clear();
    o.names.push_back("startup");
    o.names.push_back("aliases");
    for (const std::string& n : all) if (n != "startup" && n != "aliases") o.names.push_back(n);
    o.texts.clear();
    for (const std::string& n : o.names) {
        std::vector<std::string> l;
        std::string err, t;
        if (cli::read_script(n, l, err)) for (const std::string& x : l) t += x + "\n";
        o.texts.push_back(t);
    }
    const std::string cur = (o.sel >= 0 && o.sel < static_cast<int>(o.shown.size())) ? o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])] : std::string();
    o.shown.clear();
    for (size_t i = 0; i < o.names.size(); ++i) if (cli::script_matches(o.names[i], o.texts[i], o.filter)) o.shown.push_back(static_cast<int>(i));
    o.sel = 0;
    for (size_t i = 0; i < o.shown.size(); ++i) if (!cur.empty() && o.names[static_cast<size_t>(o.shown[i])] == cur) o.sel = static_cast<int>(i);
    o.scroll = std::clamp(o.scroll, 0, std::max(0, static_cast<int>(o.shown.size()) - 1));
}

void RadioApp::cliov_load(const std::string& name) {
    CliOvModel& o = ui.model.cliov;
    std::vector<std::string> l;
    std::string err;
    if (!cli::read_script(name, l, err)) l.clear();   // (startup / aliases that do not exist yet: an empty editor)
    if (l.empty()) l.push_back("");
    o.lines = l;
    o.cur = name;
    o.cy = 0; o.cx = o.anchor = 0; o.top = 0;
    o.dirty = false; o.discard_armed = false;
    o.note.clear();
}

void RadioApp::cliov_open(const std::string& name, const std::string& filter, bool from_settings) {
    CliOvModel& o = ui.model.cliov;
    o.filter = filter;
    o.filtering = false;
    o.prompt = 0;
    o.focus = 0;
    o.from_settings = from_settings;
    cliov_refresh();
    if (!name.empty()) {
        for (size_t i = 0; i < o.shown.size(); ++i) if (o.names[static_cast<size_t>(o.shown[i])] == name) o.sel = static_cast<int>(i);
        if (cli::valid_name(name)) { cliov_load(name); o.focus = 1; }
    } else if (!o.shown.empty() && !filter.empty()) {
        o.sel = 0;
        cliov_load(o.names[static_cast<size_t>(o.shown[0])]);
    }
    o.open = true;
    set_text_entry(true);
}

bool RadioApp::cliov_save() {
    CliOvModel& o = ui.model.cliov;
    if (o.cur.empty()) { o.note = "no script: [n] makes a new one"; return false; }
    std::vector<std::string> l = o.lines;
    while (l.size() > 1 && l.back().empty()) l.pop_back();
    std::string err;
    if (!cli::write_script(o.cur, l, err)) { o.note = err; return false; }
    o.dirty = false;
    o.discard_armed = false;
    o.note = "saved " + o.cur + ".cli";
    muisc::ConsoleLog::instance().log_basic("CLI: saved script " + o.cur);
    cliov_refresh();
    return true;
}

void RadioApp::cliov_run(bool from_editor) {
    CliOvModel& o = ui.model.cliov;
    std::vector<std::string> lines;
    std::string label = o.cur;
    if (from_editor || !o.shown.empty()) {
        if (from_editor) lines = o.lines;
        else {
            label = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(std::clamp(o.sel, 0, static_cast<int>(o.shown.size()) - 1))])];
            std::string err;
            if (!cli::read_script(label, lines, err)) { o.out.push_back("error: " + err); return; }
        }
    }
    if (label.empty()) { o.note = "nothing to run"; return; }
    if (label == "aliases") { o.note = "aliases.cli is the alias list; it is not run"; return; }
    o.out.push_back("> run " + label + (from_editor && o.dirty ? " (the editor text, not saved)" : ""));
    cli::Job job;
    job.origin = cli::Origin::Script;   // (a script: caution commands need force)
    job.to_overlay = true;
    job.label = label;
    for (const std::string& l : lines) job.lines.push_back(l);
    muisc::ConsoleLog::instance().log_basic("CLI: running script " + label + " from the editor");
    cli_step(job);
    if (job.lines.empty() && !job.waiting) { o.out.push_back(job.failed ? "(stopped by an error)" : "(done)"); cli_finish(job); }
    else cli_jobs.push_back(std::move(job));
    while (o.out.size() > 100) o.out.erase(o.out.begin());
}

bool RadioApp::key_cliov(KeyCtx& kc) {
    CliOvModel& o = ui.model.cliov;
    if (!o.open) return false;
    const int key = kc.k;
    const bool arrow = kc.arrow;
    const bool shifted = last_key_was_shifted();
    auto line_end = [&] { o.cx = o.anchor = o.lines[static_cast<size_t>(o.cy)].size(); };
    auto fit_cx = [&] { o.cx = o.anchor = std::min(o.cx, o.lines[static_cast<size_t>(o.cy)].size()); };
    auto select_name = [&](const std::string& n) {
        for (size_t i = 0; i < o.shown.size(); ++i) if (o.names[static_cast<size_t>(o.shown[i])] == n) o.sel = static_cast<int>(i);
    };

    if (o.prompt == 1 || o.prompt == 2) {   // a name: a new script / the new name of the one selected
        if (!arrow && key == 27) { o.prompt = 0; o.note.clear(); return true; }
        if (!arrow && (key == 13 || key == 10)) {
            const std::string n = cli::trim(o.name_buf);
            std::string err;
            if (!cli::valid_name(n)) { o.note = "a name has letters, digits, - _ . only"; return true; }
            if (o.prompt == 1) {
                bool exists = false;
                for (const std::string& x : o.names) if (x == n) exists = true;
                if (exists && cli::script_path(n) != std::filesystem::path() && std::filesystem::exists(cli::script_path(n))) { o.note = "\"" + n + "\" exists already"; return true; }
                cliov_load(n);
                o.lines = {"# " + n, ""};
                o.cy = 1; o.cx = o.anchor = 0;
                o.dirty = true;
                o.focus = 1;
                o.prompt = 0;
                if (!cliov_save()) return true;
                o.dirty = false;
                cliov_refresh();
                select_name(n);
                o.note = "new script " + n;
            } else {
                const std::string from = o.names.empty() || o.shown.empty() ? std::string() : o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])];
                if (!cli::rename_script(from, n, err)) { o.note = err; return true; }
                if (o.cur == from) o.cur = n;
                o.prompt = 0;
                cliov_refresh();
                select_name(n);
                o.note = "renamed to " + n;
            }
            return true;
        }
        edit_text_key(o.name_buf, o.nedit, key, 48, &o.note);
        return true;
    }
    if (o.prompt == 3) {   // delete: y / n (the file is moved to cli/deleted/, nothing is erased)
        if (!arrow && (key == 'y' || key == 'Y')) {
            const std::string n = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])];
            std::string err;
            if (cli::trash_script(n, err)) {
                o.note = n + " moved to the folder deleted/ of the cli folder";
                if (o.cur == n) { o.cur.clear(); o.lines = {""}; o.dirty = false; o.cy = 0; o.cx = o.anchor = 0; }
            } else o.note = err;
            o.prompt = 0;
            cliov_refresh();
        } else if (!arrow) { o.prompt = 0; o.note = "not deleted"; }
        return true;
    }
    if (o.filtering) {   // the search field of the library
        if (!arrow && (key == 27 || key == 13 || key == 10)) { o.filtering = false; return true; }
        if (arrow && (key == 'A' || key == 'B')) o.filtering = false;
        else {
            if (edit_text_key(o.filter, o.fedit, key, 80, &o.note)) { cliov_refresh(); o.sel = 0; o.scroll = 0; }
            return true;
        }
    }
    if (!arrow && key == 27) {
        if (o.dirty && !o.discard_armed) { o.discard_armed = true; o.note = "unsaved changes: CTRL+S saves, ESC again closes without saving"; return true; }
        o.dirty = false; o.discard_armed = false;
        o.open = false;
        o.from_settings = false;
        set_text_entry(ui.model.con.open ? ui.model.con.pane == 2 : ui.model.search_focus);
        return true;
    }
    if (!arrow && key == 9) { o.focus ^= 1; return true; }
    if (!arrow && key == kKeyCtrlS) { cliov_save(); return true; }
    if (!arrow && key == kKeyCtrlR) { cliov_run(o.focus == 1); return true; }
    o.discard_armed = false;

    if (o.focus == 0) {   // the library
        const int n = static_cast<int>(o.shown.size());
        auto pick = [&] {
            if (n == 0) return;
            const std::string name = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])];
            if (o.dirty && name != o.cur && !o.discard_armed) { o.discard_armed = true; o.note = "unsaved changes in " + o.cur + ": CTRL+S first, or the same key again to discard them"; return; }
            cliov_load(name);
            o.focus = 1;
        };
        if (arrow && (key == 'A' || key == 'B')) {
            const int step = shifted ? 8 : 1;
            o.sel = std::clamp(o.sel + (key == 'A' ? -step : step), 0, std::max(0, n - 1));
            return true;
        }
        if (!arrow && (key == 13 || key == 10 || key == 'e' || key == 'E')) { pick(); return true; }
        if (!arrow && key == '/') { o.filtering = true; o.fedit.to_end(o.filter); return true; }
        if (!arrow && (key == 'n' || key == 'N')) { o.prompt = 1; o.name_buf.clear(); o.nedit.reset(); o.note = "name of the new script:"; return true; }
        if (!arrow && (key == 'm' || key == 'M') && n > 0) { o.prompt = 2; o.name_buf = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])]; o.nedit.to_end(o.name_buf); o.note = "new name:"; return true; }
        if (!arrow && (key == 'x' || key == 'X') && n > 0) {
            const std::string nm = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])];
            if (!std::filesystem::exists(cli::script_path(nm))) { o.note = nm + " has no file yet"; return true; }
            o.prompt = 3; o.note = "move " + nm + " to deleted/ ? [y/n]"; return true;
        }
        if (!arrow && (key == 'r' || key == 'R')) { cliov_run(false); return true; }
        if (!arrow && (key == 'c' || key == 'C')) { o.out.clear(); return true; }
        return true;
    }

    // the editor
    std::string& line = o.lines[static_cast<size_t>(o.cy)];
    const int n = static_cast<int>(o.lines.size());
    if (arrow && (key == 'A' || key == 'B')) {
        const int step = shifted ? std::max(1, o.rows - 1) : 1;
        o.cy = std::clamp(o.cy + (key == 'A' ? -step : step), 0, n - 1);
        fit_cx();
        return true;
    }
    if (arrow && key == 'D' && o.cx == 0 && o.cy > 0) { --o.cy; line_end(); return true; }
    if (arrow && key == 'C' && o.cx >= line.size() && o.cy + 1 < n) { ++o.cy; o.cx = o.anchor = 0; return true; }
    if (!arrow && (key == 13 || key == 10)) {
        if (n >= static_cast<int>(kMaxLines)) { o.note = "500 lines at most"; return true; }
        const size_t at = std::min(o.cx, line.size());
        const std::string rest = line.substr(at);
        line.resize(at);
        o.lines.insert(o.lines.begin() + o.cy + 1, rest);
        ++o.cy; o.cx = o.anchor = 0;
        o.dirty = true;
        return true;
    }
    if (!arrow && (key == 127 || key == 8) && o.cx == 0 && o.anchor == 0 && o.cy > 0) {   // BACKSPACE at the start joins with the line above
        const size_t at = o.lines[static_cast<size_t>(o.cy - 1)].size();
        o.lines[static_cast<size_t>(o.cy - 1)] += line;
        o.lines.erase(o.lines.begin() + o.cy);
        --o.cy; o.cx = o.anchor = at;
        o.dirty = true;
        return true;
    }
    if (key == kKeyDelete && o.cx >= line.size() && o.cy + 1 < n) {
        line += o.lines[static_cast<size_t>(o.cy + 1)];
        o.lines.erase(o.lines.begin() + o.cy + 1);
        o.dirty = true;
        return true;
    }
    size_t caret = std::min(o.cx, line.size()), anchor = std::min(o.anchor, line.size());
    if (muisc::edit_text_key(line, caret, anchor, key, 600, &o.note)) o.dirty = true;
    o.cx = caret; o.anchor = anchor;
    return true;
}
