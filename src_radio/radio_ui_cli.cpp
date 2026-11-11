// The command line of the radio, drawn: the STATIONS pane while the SEARCH box holds "c:" (the command history with the replies) or
// "cs:" (the scripts that match), the console overlay (log + command line) and the script overlay (library + editor).
// The keys are in radio_app_cli.cpp; the language is the one of cli_core.h (the music player has the same).
#include "radio_ui_internal.h"
#include "console_log.h"
#include "utf8_util.h"

namespace muisc::radio::detail {

namespace {

// The first command of a script: its first line that is not blank and not a comment.
std::string first_command(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        size_t a = i;
        while (a < e && (text[a] == ' ' || text[a] == '\t')) ++a;
        if (a < e && text[a] != '#') return text.substr(a, e - a);
        i = e + 1;
    }
    return std::string();
}

struct OutLine { int kind; std::string text; };   // 0 reply, 1 typed line, 2 error

// The typed lines and their replies, wrapped to `w` columns, the newest last.
std::vector<OutLine> wrapped_out(const std::vector<std::string>& out, int w) {
    std::vector<OutLine> v;
    for (const std::string& o : out) {
        const int kind = o.rfind("> ", 0) == 0 ? 1 : (o.rfind("  error:", 0) == 0 || o.rfind("error:", 0) == 0) ? 2 : 0;
        for (const std::string& piece : wrap_lines(o, std::max(8, w), 6)) v.push_back({kind, piece});
    }
    return v;
}

std::string out_row(const Style& s, const OutLine& l, int inner) {
    const std::string& st = l.kind == 1 ? s.list_playing : l.kind == 2 ? s.legend : s.list;
    return seg_line({{st, pad_right(truncate_str(l.text, inner), inner)}}, inner);
}

}  // namespace

std::vector<std::string> cli_pane_rows(const Style& s, const UiModel& m, int inner, int rows, std::string& title, std::string& foot) {
    std::vector<std::string> v;
    auto blank = [&] { return seg_line({{s.list, spaces(inner)}}, inner); };
    if (search_is_cs(m.search)) {   // "cs:" -- the scripts that match; ENTER opens them in the script overlay
        const CliOvModel& o = m.cliov;
        title = "SCRIPTS (" + std::to_string(o.shown.size()) + ")";
        const int nw = 18;
        for (size_t k = 0; k < o.shown.size() && static_cast<int>(v.size()) < rows; ++k) {
            const size_t i = static_cast<size_t>(o.shown[k]);
            const std::string cmd = i < o.texts.size() ? first_command(o.texts[i]) : std::string();
            v.push_back(seg_line({{s.list, pad_right(truncate_str(o.names[i], nw), nw) + "  "}, {s.header, pad_right(truncate_str(cmd, std::max(1, inner - nw - 2)), std::max(1, inner - nw - 2))}}, inner));
        }
        if (o.shown.empty() && rows > 0) v.push_back(seg_line({{s.header, o.names.empty() ? "no scripts yet: ENTER opens the script overlay, [n] makes one" : "no script matches"}}, inner));
        while (static_cast<int>(v.size()) < rows) v.push_back(blank());
        foot = std::to_string(o.shown.size()) + (o.shown.size() == 1 ? " script" : " scripts") + "   ENTER: open";
        return v;
    }
    title = "COMMAND HISTORY / OUTPUT";   // "c:"
    const std::vector<OutLine> lines = wrapped_out(m.cli.out, inner);
    if (lines.empty()) {
        v.push_back(seg_line({{s.header, "type a command after c: and press ENTER (help lists them)"}}, inner));
        v.push_back(seg_line({{s.header, "UP / DOWN recall earlier lines, ESC leaves the command line"}}, inner));
        while (static_cast<int>(v.size()) < rows) v.push_back(blank());
    } else {
        const int n = static_cast<int>(lines.size());
        for (int r = 0; r < rows; ++r) {
            const int i = n - rows + r;
            v.push_back(i >= 0 ? out_row(s, lines[static_cast<size_t>(i)], inner) : blank());
        }
    }
    foot = "ENTER: run";
    return v;
}

std::vector<std::string> build_console(const Style& s, const UiModel& m, int W, int rows) {
    const ConsoleModel& c = m.con;
    const int inner = W - 4;
    const bool verbose = ConsoleLog::instance().level() == LogVerbosity::Verbose;
    // the heights, as in the player: the command box keeps 3 rows (2 replies and the line), the status box 2 .. 8, the log gets the rest; a small
    // window leaves the status box out first, then the command line
    constexpr int kCliRows = 3;
    int log_rows = 3, status_rows = 0, cli_rows = 0;
    {
        const int avail3 = rows - 6;
        if (avail3 - kCliRows - 3 >= 2) { cli_rows = kCliRows; status_rows = std::clamp(avail3 - kCliRows - 3, 2, 8); log_rows = avail3 - kCliRows - status_rows; }
        else if (rows - 4 - kCliRows >= 3) { cli_rows = kCliRows; log_rows = rows - 4 - kCliRows; }
        else log_rows = std::max(1, rows - 2);
    }
    c.has[0] = true; c.has[1] = status_rows >= 2; c.has[2] = cli_rows > 0;
    const int fpane = c.has[c.pane] ? c.pane : 0;   // (the box that has the keys, when the window has no room for it: the log)
    const bool two = status_rows >= 2 || cli_rows > 0;   // (more than one box: the focus mark shows)
    std::vector<std::string> out;
    auto pane = [&](const std::string& title, const std::vector<std::string>& lines, int n_rows, int pane_no, const std::string& foot) {
        const int n = static_cast<int>(lines.size());
        const int up = std::clamp(c.up[pane_no], 0, std::max(0, n - n_rows));
        c.up[pane_no] = up;
        const bool focus = two && fpane == pane_no;
        out.push_back(box_top(s, W, title + (focus ? " \xE2\x97\x80" : ""), s.border));
        for (int r = 0; r < n_rows; ++r) {
            const int i = n - n_rows - up + r;
            out.push_back(box_line(s, i >= 0 && i < n ? seg_line({{s.list, pad_right(truncate_str(lines[static_cast<size_t>(i)], inner), inner)}}, inner) : spaces(inner), s.border));
        }
        out.push_back(box_bottom(s, W, up > 0 ? "scrolled up " + std::to_string(up) + (foot.empty() ? std::string() : " | " + foot) : foot, s.border_bottom));
    };
    const std::string tab_hint = "[TAB] other box | [UP/DOWN] scroll | [HOME/END] | [v] verbose log | [ESC] close";
    pane(std::string("CONSOLE / LOGS") + (verbose ? " (verbose)" : ""), ConsoleLog::instance().lines(), log_rows, 0, !two ? "[UP/DOWN] scroll | [v] verbose log | [ESC] close" : std::string());
    if (status_rows >= 2) pane("STATUS MESSAGES", c.status, status_rows, 1, cli_rows == 0 ? tab_hint : std::string());
    if (cli_rows > 0) {
        const bool focus = fpane == 2;
        out.push_back(box_top(s, W, std::string("COMMAND LINE") + (focus ? " \xE2\x97\x80" : ""), s.border));
        const std::vector<OutLine> lines = wrapped_out(m.cli.out, inner);
        const int n = static_cast<int>(lines.size());
        const int rowsO = cli_rows - 1;
        for (int r = 0; r < rowsO; ++r) {
            const int i = n - rowsO + r;
            out.push_back(box_line(s, i >= 0 && i < n ? out_row(s, lines[static_cast<size_t>(i)], inner) : spaces(inner), s.border));
        }
        out.push_back(box_line(s, field_row(s.search_idle, "> ", c.buf, c.edit, focus, inner, c.note, s.legend, false, s.search), s.border));
        out.push_back(box_bottom(s, W, focus ? "[ENTER] run | [UP/DOWN] history | [CTRL+RIGHT] complete | [TAB] next box | [ESC] clear, then close" : tab_hint, s.border_bottom));
    }
    while (static_cast<int>(out.size()) < rows) out.push_back(spaces(W));
    return out;
}

std::vector<std::string> build_cliov(const Style& s, const UiModel& m, int W, int rows_total) {
    const CliOvModel& o = m.cliov;
    const int lw = std::clamp(W / 4, 24, 36);
    const int rw = std::max(30, W - lw);
    const int H = std::max(8, rows_total);
    const int inner_rows = H - 2;
    const int out_rows = std::clamp(inner_rows / 5, 2, 4);
    const int ed_rows = std::max(2, inner_rows - out_rows - 1);
    o.rows = ed_rows;
    const int linner = lw - 4, rinner = rw - 4;

    std::vector<std::string> L, R;
    L.push_back(box_top(s, lw, "SCRIPTS (" + std::to_string(o.shown.size()) + ")" + (o.focus == 0 ? " \xE2\x97\x80" : ""), s.border));
    if (o.prompt == 1 || o.prompt == 2) L.push_back(box_line(s, field_row(s.search_idle, "name: ", o.name_buf, o.nedit, true, linner, "", s.legend, false, s.search), s.border));
    else if (o.filtering) L.push_back(box_line(s, field_row(s.search_idle, "/ ", o.filter, o.fedit, true, linner, "", s.legend, false, s.search), s.border));
    else {
        const std::string t = o.filter.empty() ? (o.note.empty() ? std::string("[/] search names and text") : o.note) : "/ " + o.filter + (o.note.empty() ? std::string() : "  " + o.note);
        L.push_back(box_line(s, seg_line({{s.header, pad_right(truncate_str(t, linner), linner)}}, linner), s.border));
    }
    const int list_rows = inner_rows - 1;
    const int total = static_cast<int>(o.shown.size());
    const int sel = std::clamp(o.sel, 0, std::max(0, total - 1));
    int scroll = std::clamp(o.scroll, 0, std::max(0, total - list_rows));
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + list_rows) scroll = sel - list_rows + 1;
    o.scroll = scroll;
    for (int r = 0; r < list_rows; ++r) {
        const int i = scroll + r;
        if (i >= total) { L.push_back(box_line(s, spaces(linner), s.border)); continue; }
        const std::string& nm = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(i)])];
        std::string label = nm;
        if (nm == "startup") label += "  (runs at the start)";
        else if (nm == "aliases") label += "  (the aliases)";
        if (nm == o.cur) label += o.dirty ? " *" : " \xE2\x80\xA2";
        const std::string text = std::string(i == sel ? (o.focus == 0 ? "> " : "- ") : "  ") + truncate_str(label, linner - 2);
        L.push_back(box_line(s, seg_line({{i == sel && o.focus == 0 ? s.list_cursor : s.list, pad_right(text, linner)}}, linner), s.border));
    }
    L.push_back(box_bottom(s, lw, o.focus == 0 ? "[n]ew [m]ove [x] del [r]un" : "[TAB] library", s.border_bottom));

    R.push_back(box_top(s, rw, std::string("EDITOR  ") + (o.cur.empty() ? "(no script: [n] in the library)" : o.cur + ".cli") + (o.dirty ? " *" : "") + (o.focus == 1 ? " \xE2\x97\x80" : ""), s.border));
    const int total_lines = static_cast<int>(o.lines.size());
    int top = std::clamp(o.top, 0, std::max(0, total_lines - ed_rows));
    if (o.cy < top) top = o.cy;
    if (o.cy >= top + ed_rows) top = o.cy - ed_rows + 1;
    o.top = top;
    const int text_w = std::max(4, rinner - 4);
    for (int r = 0; r < ed_rows; ++r) {
        const int i = top + r;
        if (i >= total_lines) { R.push_back(box_line(s, spaces(rinner), s.border)); continue; }
        char num[16];
        std::snprintf(num, sizeof num, "%3d ", i + 1);
        if (i == o.cy && o.focus == 1) {
            EditState e; e.caret = o.cx; e.anchor = o.anchor;
            const EditPaint p = paint_edit_field(o.lines[static_cast<size_t>(i)], e, text_w, std::string(), true);
            R.push_back(box_line(s, s.header + num + kReset + p.s + spaces(std::max(0, text_w - p.cols)), s.border));
        } else {
            R.push_back(box_line(s, s.header + num + kReset + seg_line({{s.list, pad_right(truncate_str(o.lines[static_cast<size_t>(i)], text_w), text_w)}}, text_w), s.border));
        }
    }
    {
        std::string sep = "\xE2\x94\x80\xE2\x94\x80 OUTPUT ";
        const int fill = std::max(0, rinner - 10);
        for (int i = 0; i < fill; ++i) sep += "\xE2\x94\x80";
        R.push_back(box_line(s, seg_line({{s.header, pad_right(truncate_str(sep, rinner), rinner)}}, rinner), s.border));
        const int n = static_cast<int>(o.out.size());
        for (int r = 0; r < out_rows; ++r) {
            const int i = n - out_rows + r;
            R.push_back(box_line(s, seg_line({{s.list, pad_right(truncate_str(i >= 0 && i < n ? o.out[static_cast<size_t>(i)] : std::string(), rinner), rinner)}}, rinner), s.border));
        }
    }
    const std::vector<std::string> legs = {"[CTRL+S] save | [CTRL+R] run | [TAB] library | [ESC] close", "[CTRL+S] save | [CTRL+R] run | [ESC] close", "[CTRL+R] run | [ESC] close"};
    std::string leg = legs.back();
    for (const std::string& x : legs) if (display_width(x) <= rw - 4) { leg = x; break; }
    R.push_back(box_bottom(s, rw, o.focus == 1 && !o.note.empty() && o.prompt == 0 ? truncate_str(o.note, rw - 6) : leg, s.border_bottom));

    std::vector<std::string> out;
    for (size_t i = 0; i < L.size() && i < R.size(); ++i) out.push_back(L[i] + R[i]);
    while (static_cast<int>(out.size()) < rows_total) out.push_back(spaces(W));
    return out;
}

}  // namespace muisc::radio::detail
