// The Settings screen: its tabs, the colour themes (picker, creator, import / export) and the PATHS and REFERENCE rows.
// Split out of app.cpp. Member functions of App (declared in app.h); the tables are in app_settings_tables.h.
#include "app.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "terminal_font.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "keyboard_layout.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "console_log.h"
#include "karaoke_font.h"
#include "glyph_raster.h"
#include "chiptune.h"
#include "scope_big.h"
#include "scope_window.h"
#include "wizard.h"
#include "creator.h"
#include "mic_capture.h"
#include "process_util.h"
#include "update_check.h"
#include "app_internal.h"
#include "app_settings_shared.h"
#include "app_settings_tables.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <sys/types.h>
#include <sys/stat.h>   // file_added_time(): stat() / _wstat64()
#if defined(_WIN32)
#include "win_compat.h"
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include "app_internal.h"

namespace muisc {

// Part of app_settings.cpp, moved to its own file so that the two compile in parallel (the same App class, nothing else changed).
// Text that starts at column `x` of a screen drawn with absolute positions, cut so that it ends at column `last` at the latest
// (escape sequences are kept whole and cost no column, a wide character is not split). Without this a row written for a wide
// window runs past the right edge of a narrow one and wraps.
static std::string clip_to_columns(const std::string& s, int x, int last) {
    const int room = last - x + 1;
    if (room <= 0) return std::string();
    std::string out;
    int used = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1b && i + 1 < s.size() && s[i + 1] == '[') {   // CSI: ESC [ ... final byte
            size_t j = i + 2;
            while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7e)) ++j;
            out.append(s, i, j + 1 - i);
            i = j + 1;
            continue;
        }
        size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        len = std::min(len, s.size() - i);
        const int w = display_width(s.substr(i, len));
        if (used + w > room) { out += "\x1b[0m"; return out; }
        out.append(s, i, len);
        used += w;
        i += len;
    }
    return out;
}

// template) and the full-size CREATE THEME editor. Drawn with absolute positions like the Settings screen.
void App::build_theme_screen(std::ostringstream& frame, int W, int player_h) const {
    if (W < 80) W = 80;
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const int MAX_Y = std::max(player_h - 1, 11);
    auto B = [&](int y) {
        if (!settings_.border_smooth) return ansi_for(y >= MAX_Y ? settings_.border_color_bottom : settings_.border_color, false); // rough: only the bottom line changes
        return gradient_ansi(settings_.border_color, settings_.border_color_bottom,
                             (MAX_Y > 1) ? static_cast<float>(y - 1) / (MAX_Y - 1) : 0.0f, false);
    };
    auto pos = [&](int y, int x, const std::string& s) { if (x <= W) frame << "\x1b[" << y << ";" << x << "H" << clip_to_columns(s, x, (x == 1 || x == W) ? W : W - 1); };   // (a row that does not fit is cut before the right border)
    auto repeat = [](const std::string& s, int n) { std::string r; for (int i = 0; i < n; ++i) r += s; return r; };
    auto pad = [](const std::string& s, int width, bool left_align = true) {
        const int ulen = static_cast<int>(display_width(s));
        if (ulen >= width) return s;
        const std::string sp(static_cast<size_t>(width - ulen), ' ');
        return left_align ? s + sp : sp + s;
    };
    auto box_row = [&](int y) { pos(y, 1, B(y) + "│" + R); pos(y, W, B(y) + "│" + R); };
    auto top_border = [&](const std::string& title) {
        const std::string t = "─ " + title + " ";
        pos(1, 1, B(1) + "┌" + t + repeat("─", W - 2 - static_cast<int>(display_width(t))) + "┐" + R);
    };
    auto bottom_border = [&](int y) { pos(y, 1, B(y) + "└" + repeat("─", W - 2) + "┘" + R); };

    if (theme_ui_ == 4 || theme_ui_ == 5) {   // export / import overlays
        top_border(theme_ui_ == 4 ? "EXPORT THEME" : "IMPORT THEMES");
        const bool editing_io = (mode_ == Mode::ColorEdit);
        int cursor_y = 0, cursor_x = 0;
        auto field = [&](int y, const char* label, const std::string& value, bool sel, int width) {
            pos(y, 3, header_sgr(settings_) + pad(label, 15) + R); pos(y, 18, ":");
            std::string v;
            if (sel && editing_io) {
                const size_t c = std::min(edit_caret_, color_edit_buffer_.size()), a = std::min(edit_anchor_, color_edit_buffer_.size());
                EditPaint p = paint_edit_field(color_edit_buffer_, c, a, width, change_sgr(settings_), false);
                v = change_sgr(settings_) + p.s + std::string(static_cast<size_t>(std::max(0, width + 1 - p.cols)), ' ');
                cursor_y = y;
                cursor_x = 20 + paint_edit_field(color_edit_buffer_, c, a, width, "", false).caret;
            } else {
                v = pad(truncate_str(value, width), width + 1);
            }
            pos(y, 20, (sel && !editing_io ? HI : std::string()) + v + R);
        };
        for (int y = 2; y < MAX_Y; ++y) box_row(y);
        const int fw = std::max(20, W - 24);
        if (theme_ui_ == 4) {
            field(3, "NAME", theme_io_name_, theme_io_row_ == 0, themes::kMaxNameLen);
            field(4, "FOLDER", theme_io_dir_, theme_io_row_ == 1, fw);
            const std::string file = (theme_dir_path(theme_io_dir_) / themes::file_name_for(theme_io_name_)).string();
            pos(6, 3, legend_sgr(settings_) + truncate_str("Exports the colors now set as one theme file: " + file, W - 5) + R);
            pos(7, 3, legend_sgr(settings_) + "(player and radio colors are both kept in the file; import it with SHIFT+i)" + R);
        } else {
            field(3, "FOLDER", theme_io_dir_, theme_io_row_ == 0, fw);
            pos(5, 3, header_sgr(settings_) + "Theme files found (" + std::to_string(theme_io_files_.size()) + ")" + R);
            const int nf = static_cast<int>(theme_io_files_.size());
            const int visible = std::max(1, MAX_Y - 7);
            const int scroll = std::clamp(theme_io_sel_ - visible / 2, 0, std::max(0, nf - visible));
            for (int r = 0; r < visible && scroll + r < nf; ++r) {
                const int i = scroll + r;
                const std::string name = theme_io_files_[static_cast<size_t>(i)].filename().string();
                const size_t nthemes = themes::read_theme_file(theme_io_files_[static_cast<size_t>(i)]).size();
                const bool sel = (theme_io_row_ == 1 && i == theme_io_sel_);
                pos(6 + r, 3, (sel ? HI : std::string()) + pad(truncate_str(name, W - 20), W - 20) + R);
                pos(6 + r, W - 14, legend_sgr(settings_) + std::to_string(nthemes) + (nthemes == 1 ? " theme" : " themes") + R);
            }
            if (nf == 0) pos(6, 3, legend_sgr(settings_) + "no theme files (*.theme, *.txt) in this folder" + R);
        }
        bottom_border(MAX_Y);
        pos(MAX_Y + 1, 1, legend_colored(settings_, std::string(editing_io ? "[ENTER] Apply  [ESC] Cancel  [\u2190\u2192] Cursor  [Ctrl+C/X/V] Copy/Cut/Paste"
            : theme_ui_ == 4 ? "[\u2191\u2193] Field  [ENTER] Edit  [s] Export  [ESC] Cancel"
                             : "[\u2191\u2193] Select  [ENTER] Import the file (or edit the folder)  [a] Import all  [ESC] Cancel")) + R);
        if (!status_line_.empty()) pos(MAX_Y + 2, 1, "\x1b[38;5;246m" + truncate_str(status_line_, W - 2) + R);
        if (editing_io && cursor_y > 0) frame << "\x1b[" << cursor_y << ";" << cursor_x << "H\x1b[?25h";
        return;
    }

    if (theme_ui_ == 1 || theme_ui_ == 3) {
        top_border(theme_ui_ == 1 ? "CHOOSE THEME" : "CHOOSE THEME AS TEMPLATE");
        const int n = static_cast<int>(theme_list_.size());
        const int visible = std::max(1, MAX_Y - 3);
        const int scroll = std::clamp(theme_sel_ - visible / 2, 0, std::max(0, n - visible));
        // the marker: the theme in use (picker), or the one the draft matches / is based on (template picker)
        int cur = -1;
        if (theme_ui_ == 1) cur = theme_current_index();
        else {
            for (int k = 0; k < n && cur < 0; ++k) if (!theme_list_[static_cast<size_t>(k)].builtin && theme_matches(theme_draft_, theme_list_[static_cast<size_t>(k)])) cur = k;
            for (int k = 0; k < n && cur < 0; ++k) if (theme_matches(theme_draft_, theme_list_[static_cast<size_t>(k)])) cur = k;
            for (int k = 0; k < n && cur < 0; ++k) if (theme_list_[static_cast<size_t>(k)].name == theme_base_.name) cur = k;
        }
        // swatches: (key, is background)
        static const struct { const char* key; bool bg; } kSw[] = {
            {"ColorBorderTop", false}, {"ColorBorderBottom", false}, {"ColorDiskTop", false}, {"ColorDiskBottom", false},
            {"ColorMetadataKey", false}, {"ColorMetadataVal", false}, {"ColorProgressBarPlayed", false},
            {"ColorListInactiveFg", false}, {"ColorListPlayingFg", false}, {"ColorListCursorBg", true}, {"ColorHeader", false}};
        int y = 2;
        for (int r = 0; r < visible && y < MAX_Y; ++r, ++y) {
            box_row(y);
            const int i = scroll + r;
            if (i >= n) continue;
            const themes::Theme& t = theme_list_[static_cast<size_t>(i)];
            std::string name = pad(truncate_str(t.name, themes::kMaxNameLen), themes::kMaxNameLen);
            pos(y, 3, (i == cur ? header_sgr(settings_) + "●" + R : " "));
            // your own themes (not built in) are named in the header colour
            pos(y, 5, (t.builtin ? std::string() : header_sgr(settings_)) + (i == theme_sel_ ? HI : std::string()) + name + R);
            std::string sw;
            for (const auto& k : kSw) {
                const std::string* v = t.get(k.key, false);
                const std::string val = v ? *v : "";
                sw += k.bg ? bg_ansi_for(val) + "    " + R + " " : ansi_for(val, false) + "████" + R + " ";
            }
            pos(y, 33, sw);
        }
        while (y < MAX_Y) { box_row(y); ++y; }
        // more themes than rows: a scrollbar in the last column, in the colour of the border gradient at that row
        theme_page_ = visible;
        if (overlay::Thumb th; overlay::scroll_thumb(scroll, visible, n, th)) {
            for (int r = 0; r < visible && 2 + r < MAX_Y; ++r) {
                const bool on = r >= th.at && r < th.at + th.len;
                pos(2 + r, W - 2, B(2 + r) + (on ? "\u2588" : "\u2591") + R);
            }
        }
        bottom_border(MAX_Y);
        if (leave_prompt_ == 4 && theme_sel_ >= 0 && theme_sel_ < n)
            pos(MAX_Y + 1, 1, "\x1b[1;7m" + pad(" Do you want to delete theme \"" + truncate_str(theme_list_[static_cast<size_t>(theme_sel_)].name, themes::kMaxNameLen) + "\"? [y] Yes, delete  [n / ESC] No, keep it", W - 1) + R);
        else
        pos(MAX_Y + 1, 1, legend_colored(settings_, std::string(theme_ui_ == 1
            ? "[↑↓] Select  [SHIFT+↑↓] Page  [SHIFT+p] Preview  [ENTER] Apply  [SHIFT+d] Delete your own theme  [ESC] Back"
            : "[↑↓] Select  [SHIFT+↑↓] Page  [SHIFT+p] Preview  [ENTER] Use as template  [ESC] Back")) + R);
        if (!status_line_.empty()) pos(MAX_Y + 2, 1, "\x1b[38;5;246m" + truncate_str(status_line_, W - 2) + R);
        return;
    }

    // ---- CREATE THEME ----
    static const char* grp[21] = {"BORDER_COLOR", "DISK", "METADATA", "VIZ", "PROGRESS_BAR",
                                  "SEARCH", "", "LIST", "", "", "QUEUE", "", "", "LYRICS", "", "", "HEADER", "LEGEND", "TAB_NAMES", "CHANGE", "FOCUS"};
    static const char* l1n[21] = {"TOP", "TOP", "KEY", "LEFT", "PLAYED", "FG", "INACTIVE  FG",
                                  "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG", "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                  "INACTIVE  FG", "ACTIVE L  FG", "ACTIVE W  FG", "TEXT", "KEY-CMD", "CURRENT", "TEXT", "TEXT"};
    static const char* l2n[21] = {"BOTTOM", "BOTTOM", "VAL", "RIGHT", "PENDING", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG",
                                  "", "COMMAND", "OTHER", "BG", ""};
    top_border("CREATE THEME");

    const bool editing = (mode_ == Mode::ColorEdit);
    auto edit_paint = [&](int width) {
        const size_t c = std::min(edit_caret_, color_edit_buffer_.size());
        const size_t a = std::min(edit_anchor_, color_edit_buffer_.size());
        EditPaint p = paint_edit_field(color_edit_buffer_, c, a, width, change_sgr(settings_), false);
        p.s += std::string(static_cast<size_t>(std::max(0, width - p.cols)), ' ');
        return p;
    };
    int cursor_y = 0, cursor_x = 0;

    // top row: NAME [......]   [ CHOOSE THEME AS TEMPLATE ]   (based on ...)
    box_row(2);
    pos(2, 3, header_sgr(settings_) + pad("NAME", 15) + R); pos(2, 18, ":");
    {
        const bool sel = (theme_row_ < 0 && theme_col_ == 0);
        const bool ed = sel && editing;
        const int fw = themes::kMaxNameLen + 1;
        std::string v;
        if (ed) {
            EditPaint p = edit_paint(fw);
            v = change_sgr(settings_) + p.s;   // paint_edit_field only re-emits the colour after a selection
            cursor_y = 2;
            cursor_x = 20 + paint_edit_field(color_edit_buffer_, std::min(edit_caret_, color_edit_buffer_.size()),
                                             std::min(edit_anchor_, color_edit_buffer_.size()), fw, "", false).caret;
        } else {
            v = pad(theme_name_.empty() ? std::string("(type a name)") : theme_name_, fw);
        }
        pos(2, 20, (sel && !ed ? HI : std::string()) + (!ed && theme_name_.empty() ? legend_sgr(settings_) : std::string()) + v + R);
    }
    {
        const bool sel = (theme_row_ < 0 && theme_col_ == 1);
        pos(2, 49, (sel ? HI : std::string()) + " CHOOSE THEME AS TEMPLATE " + R);
        pos(2, 77, legend_sgr(settings_) + truncate_str("based on " + theme_base_.name, std::max(1, W - 78)) + R);
    }
    pos(3, 1, B(3) + "├" + repeat("─", W - 2) + "┤" + R);

    const int visible = std::max(1, MAX_Y - 4);
    const int scroll = (theme_row_ < 0) ? 0 : std::clamp(theme_row_ - visible / 2, 0, std::max(0, 21 - visible));
    int y = 4;
    for (int r = 0; r < visible && y < MAX_Y; ++r, ++y) {
        box_row(y);
        const int i = scroll + r;
        if (i >= 21) continue;
        pos(y, 3, header_sgr(settings_) + pad(grp[i], 15) + R); pos(y, 18, ":");
        pos(y, 20, pad(l1n[i], 14, false)); pos(y, 35, ":");
        const std::string v0 = [&] { const std::string* c = color_cell(const_cast<Settings&>(theme_draft_), i, 0); return c ? *c : std::string(); }();
        {
            const bool sel = (i == theme_row_ && theme_col_ == 0);
            const bool ed = sel && editing;
            std::string v = ed ? edit_paint(7).s : pad(v0, 7);
            if (ed) { cursor_y = y; cursor_x = 38 + paint_edit_field(color_edit_buffer_, std::min(edit_caret_, color_edit_buffer_.size()),
                                                                     std::min(edit_anchor_, color_edit_buffer_.size()), 7, "", false).caret; }
            pos(y, 38, (sel && !ed ? HI : std::string()) + (ed ? change_sgr(settings_) : std::string()) + v + R);
        }
        pos(y, 46, ansi_for(v0, false) + "██" + R);
        if (l2n[i][0]) {
            const std::string* c1 = color_cell(const_cast<Settings&>(theme_draft_), i, 1);
            const std::string v1 = c1 ? *c1 : std::string();
            pos(y, 50, pad(l2n[i], 7, false)); pos(y, 58, ":");
            const bool sel = (i == theme_row_ && theme_col_ == 1);
            const bool ed = sel && editing;
            std::string v = ed ? edit_paint(7).s : pad(v1, 7);
            if (ed) { cursor_y = y; cursor_x = 61 + paint_edit_field(color_edit_buffer_, std::min(edit_caret_, color_edit_buffer_.size()),
                                                                     std::min(edit_anchor_, color_edit_buffer_.size()), 7, "", false).caret; }
            pos(y, 61, (sel && !ed ? HI : std::string()) + (ed ? change_sgr(settings_) : std::string()) + v + R);
            const bool is_bg = std::string(l2n[i]) == "BG";
            pos(y, 69, is_bg ? bg_ansi_for(v1) + "  " + R : ansi_for(v1, false) + "██" + R);
        }
        // a live sample of the row, drawn from the draft
        const Settings& d = theme_draft_;
        std::string rt;
        const std::string a0 = v0;
        const std::string* c1p = color_cell(const_cast<Settings&>(d), i, 1);
        const std::string a1 = c1p ? *c1p : std::string();
        if (i == 0 || i == 1 || i == 3) rt = gradient_preview_bar(a0, a1, 23);
        else if (i == 2) rt = ansi_for(a0, false) + "NAME : " + R + ansi_for(a1, false) + "SONG.MP3" + R;
        else if (i == 4) rt = ansi_for(a0, false) + "[" + volume_bar_fill(settings_.volume_indicator, 11, volume_clock_angle()) + R + ansi_for(a1, false) + "----------]" + R;
        else if (i == 5 || i == 6 || (i >= 7 && i <= 15)) rt = bg_ansi_for(a1) + ansi_for(a0, false) + "THIS IS AN EXAMPLE TEXT" + R;
        else if (i == 16) { const std::string pr = sgr_params_for(a0); rt = (pr.empty() ? std::string("\x1b[1m") : "\x1b[1;" + pr + "m") + "SECTION HEADER" + R; }
        else if (i == 17) rt = ansi_for(a0, false) + "[ESC]" + R + ansi_for(a1, false) + " Close" + R + "  " + ansi_for(a0, false) + "[ENTER]" + R + ansi_for(a1, false) + " OK" + R;
        else if (i == 18) rt = ansi_for(a0, false) + "[COLORS]" + R + "  " + ansi_for(a1, false) + "GENERAL  ANIMA/VIZ" + R;
        else if (i == 19) rt = ansi_for(a0, false) + bg_ansi_for(a1) + "EDITING..." + R;
        else if (i == 20) { const std::string pr = sgr_params_for(a0); rt = ansi_for(settings_.list_color, false) + "LOCAL AUDIO FILES " + R + (pr.empty() ? std::string("\x1b[1m") : "\x1b[1;" + pr + "m") + "\u25c0" + R; }
        if (!rt.empty()) pos(y, 72, rt);
    }
    while (y < MAX_Y) { box_row(y); ++y; }
    bottom_border(MAX_Y);
    if (leave_prompt_ == 3)
        pos(MAX_Y + 1, 1, "\x1b[1;7m" + pad(" Do you want to overwrite theme \"" + truncate_str(themes::sanitize_name(theme_name_), themes::kMaxNameLen) + "\"? [y] Yes, overwrite  [n / ESC] No, keep editing", W - 1) + R);
    else if (leave_prompt_ == 2)
        pos(MAX_Y + 1, 1, "\x1b[1;7m" + pad(" Unsaved theme! [s] Save it  [d] Discard it  [ESC] Keep editing", W - 1) + R);
    else
    pos(MAX_Y + 1, 1, legend_colored(settings_, std::string(editing
        ? "[ENTER] Apply  [ESC] Cancel  [←→] Cursor  [SHIFT+←→] Mark  [Ctrl+C/X/V] Copy/Cut/Paste"
        : "[ENTER] Edit/Open  [s] Save theme  [ESC] Discard")) + R);
    if (!status_line_.empty()) pos(MAX_Y + 2, 1, "\x1b[38;5;246m" + truncate_str(status_line_, W - 2) + R);
    if (editing && cursor_y > 0) frame << "\x1b[" << cursor_y << ";" << cursor_x << "H\x1b[?25h";
}

static std::string repeat(const std::string& s, int n) {
    std::string r; r.reserve(s.size() * static_cast<size_t>(std::max(0, n)));
    for (int i = 0; i < n; ++i) r += s;
    return r;
}

static std::string pad(const std::string& s, int width, bool left_align = true) {
    int ulen = display_width(s);
    if (ulen >= width) return s;
    std::string spaces(width - ulen, ' ');
    return left_align ? (s + spaces) : (spaces + s);
}

// What the tab functions of the settings screen share with build_settings_screen(): the frame, the sizes and the painting helpers.
struct App::SettingsPaint {
    std::ostringstream& frame;
    int W;
    int player_h;
    int MAX_Y;
    std::string R, HI;
    std::function<std::string(int)> B;                                   // the border colour of a row
    std::function<void(int, int, const std::string&)> pos;               // draw at (row, column)
    std::function<void(int, int, int)> scrollbar;
    std::function<int()> edit_field_width;
    std::function<EditPaint(int)> edit_paint;
    int& clip_rows;
    int& clip_top;
    int& y;                  // the next content row
    int& colors_visible;
    int& colors_first;
};

void App::settings_tab_colors(SettingsPaint& c) const {
    [[maybe_unused]] std::ostringstream& frame = c.frame; [[maybe_unused]] const int W = c.W; [[maybe_unused]] const int player_h = c.player_h; [[maybe_unused]] const int MAX_Y = c.MAX_Y;
    [[maybe_unused]] const std::string& R = c.R; [[maybe_unused]] const std::string& HI = c.HI;
    [[maybe_unused]] auto& B = c.B; [[maybe_unused]] auto& pos = c.pos; [[maybe_unused]] auto& scrollbar = c.scrollbar; [[maybe_unused]] auto& edit_field_width = c.edit_field_width; [[maybe_unused]] auto& edit_paint = c.edit_paint;
    [[maybe_unused]] int& clip_rows = c.clip_rows; [[maybe_unused]] int& clip_top = c.clip_top; [[maybe_unused]] int& y = c.y; [[maybe_unused]] int& colors_visible = c.colors_visible; [[maybe_unused]] int& colors_first = c.colors_first;
    clip_rows = MAX_Y;
    colors_visible = std::max(1, MAX_Y - 3);
    colors_first = colors_first_line(settings_row_, colors_visible);
    y = 3 - colors_first;
    clip_top = 3;
    static const char* grp[21]  = {"BORDER_COLOR", "DISK", "METADATA", "VIZ", "PROGRESS_BAR",
                                    "SEARCH", "", "LIST", "", "", "QUEUE", "", "", "LYRICS", "", "", "HEADER", "LEGEND", "TAB_NAMES", "CHANGE", "FOCUS"};
    static const char* l1n[21]  = {"TOP", "TOP", "KEY", "LEFT", "PLAYED",
                                    "FG", "INACTIVE  FG",
                                    "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                    "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                    "INACTIVE  FG", "ACTIVE L  FG", "ACTIVE W  FG",
                                    "TEXT", "KEY-CMD", "CURRENT", "TEXT", "TEXT"};
    static const char* l2n[21]  = {"BOTTOM", "BOTTOM", "VAL", "RIGHT", "PENDING",
                                    "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG",
                                    "", "COMMAND", "OTHER", "BG", ""}; // HEADER, LEGEND and FOCUS are foreground-only fields: no background cell at all
    // THEME | CREATE THEME (row 0): pick a ready-made colour scheme, or make one of your own.
    {
        pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
        pos(y, 3, header_sgr(settings_) + pad("THEME", 15) + R); pos(y, 18, ":");
        const int ci = theme_current_index();
        const std::string tn = ci >= 0 ? theme_list_[static_cast<size_t>(ci)].name : std::string("(custom)");
        const bool sel0 = (settings_row_ == 0 && settings_col_ == 0);
        const bool sel1 = (settings_row_ == 0 && settings_col_ == 1);
        pos(y, 20, (sel0 ? HI : "") + pad(truncate_str(tn, themes::kMaxNameLen), themes::kMaxNameLen + 1) + R);
        pos(y, 50, (sel1 ? HI : "") + " CREATE THEME " + R);
        y++;
        pos(y, 1, B(y) + "\u251c" + repeat("\u2500", W - 2) + "\u2524" + R); y++;
    }
    for (int i = 0; i < 21; ++i) {
        if (i == 1) { // SMOOTHNESS: how the frame's colour runs from BORDER_COLOR TOP to BOTTOM
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            pos(y, 3, header_sgr(settings_) + pad("", 15) + R); pos(y, 18, ":");
            pos(y, 20, pad("SMOOTHNESS", 14, false)); pos(y, 35, ":");
            const bool sel = (settings_row_ == 2);
            pos(y, 38, (sel ? HI : "") + pad(settings_get_value(2, 0), 7) + R);
            if (sel) pos(y, 46, ansi_for(settings_.tab_current_color, false) + "< \u2194 >" + R);
            pos(y, 72, legend_sgr(settings_) + (settings_.border_smooth ? "frame fades from TOP to BOTTOM" : "only the bottom border line = BOTTOM") + R);
            y++;
        }
        if (i == 5) { pos(y, 1, B(y) + "\u251c" + repeat("\u2500", W - 2) + "\u2524" + R); y++; }
        pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);

        pos(y, 3, header_sgr(settings_) + pad(grp[i], 15) + R); pos(y, 18, ":");

        pos(y, 20, pad(l1n[i], 14, false)); pos(y, 35, ":");
        {
            bool sel = (tab_row_of_color(i) == settings_row_ && settings_col_ == 0 && mode_ != Mode::ColorEdit);
            bool ed = (tab_row_of_color(i) == settings_row_ && settings_col_ == 0 && mode_ == Mode::ColorEdit);
            std::string v = ed ? edit_paint(edit_field_width()).s : pad(settings_get_value(tab_row_of_color(i), 0), 7);
            pos(y, 38, (sel ? HI : "") + (ed ? change_sgr(settings_) : "") + v + R);
        }

        // Second (background) column: the HEADER and LEGEND rows have no background
        // cell at all, so it renders neither a label nor a ":" here --
        // without this guard there'd be a dangling colon at col 56 with
        // nothing between it and the preview.
        // Colour swatches right behind the values (the radio settings do the same):
        // foreground cells show "██" in the colour, background cells a block of it.
        pos(y, 46, ansi_for(settings_get_value(tab_row_of_color(i), 0), false) + "\u2588\u2588" + R);
        if (l2n[i][0]) {
            pos(y, 50, pad(l2n[i], 7, false)); pos(y, 58, ":");
            bool sel = (tab_row_of_color(i) == settings_row_ && settings_col_ == 1 && mode_ != Mode::ColorEdit);
            bool ed = (tab_row_of_color(i) == settings_row_ && settings_col_ == 1 && mode_ == Mode::ColorEdit);
            std::string v = ed ? edit_paint(edit_field_width()).s : pad(settings_get_value(tab_row_of_color(i), 1), 7);
            pos(y, 61, (sel ? HI : "") + (ed ? change_sgr(settings_) : "") + v + R);
            const bool is_bg = std::string(l2n[i]) == "BG";
            pos(y, 69, is_bg ? bg_ansi_for(settings_get_value(tab_row_of_color(i), 1)) + "  " + R
                             : ansi_for(settings_get_value(tab_row_of_color(i), 1), false) + "\u2588\u2588" + R);
        }

        std::string valA = settings_get_value(tab_row_of_color(i), 0), valB = settings_get_value(tab_row_of_color(i), 1);
        std::string rt;
        if (i == 0) rt = gradient_preview_bar(valA, valB, 23);
        else if (i == 1) rt = gradient_preview_bar(valA, valB, 23);
        else if (i == 2) rt = ansi_for(valA, false) + "NAME : " + R + ansi_for(valB, false) + "SONG.MP3" + R;
        else if (i == 3) rt = gradient_preview_bar(valA, valB, 23);
        else if (i == 4) rt = ansi_for(valA, false) + "[" + volume_bar_fill(settings_.volume_indicator, 11, volume_clock_angle()) + R + ansi_for(valB, false) + "----------]" + R;
        else if (i == 5) rt = search_idle_sgr(settings_) + "/l:" + R + search_sgr(settings_) + "QUERY" + R;
        else if (i == 6) rt = bg_ansi_for(valB) + ansi_for(valA, false) + "/l:" + std::string(15, ' ') + R;
        else if (i >= 7 && i <= 14) rt = bg_ansi_for(valB) + ansi_for(valA, false) + "THIS IS AN EXAMPLE TEXT" + R;
        else if (i == 15) {
            std::string aL_F = ansi_for(settings_.active_line_color, false), aL_B = bg_ansi_for(settings_.active_line_bg_color);
            std::string aW_F = ansi_for(valA, false), aW_B = bg_ansi_for(valB);
            std::string iN_F = ansi_for(settings_.inactive_line_color, false), iN_B = bg_ansi_for(settings_.inactive_line_bg_color);
            rt = aL_B + aL_F + "THIS IS " + R + aW_B + aW_F + "AN " + R + iN_B + iN_F + "EXAMPLE TEXT" + R;
        } else if (i == 16) {
            // Same bold + color construction header_sgr() draws the real
            // section titles with, so the swatch shows the exact result.
            std::string params = sgr_params_for(valA);
            rt = (params.empty() ? std::string("\x1b[1m") : "\x1b[1;" + params + "m") + "SECTION HEADER" + R;
        } else if (i == 17) {
            // Same prefix legend_sgr() gives the real hint lines, so the
            // swatch shows the exact result (an unset color is plain text).
            rt = ansi_for(valA, false) + "[ESC]" + R + ansi_for(valB, false) + " Close" + R + "  " + ansi_for(valA, false) + "[ENTER]" + R + ansi_for(valB, false) + " OK" + R;
        } else if (i == 18) {
            rt = ansi_for(valA, false) + "[COLORS]" + R + "  " + ansi_for(valB, false) + "GENERAL  ANIMA/VIZ" + R;
        } else if (i == 19) {
            // Same construction change_sgr() gives the real activated
            // field (a PATHS row, a meta editor tag, ...), so the swatch
            // shows the exact result.
            rt = ansi_for(valA, false) + bg_ansi_for(valB) + "EDITING..." + R;
        } else if (i == 20) {
            // Same construction focus_sgr() gives the real indicator after a pane title.
            const std::string params = sgr_params_for(valA);
            rt = ansi_for(settings_.list_color, false) + "LOCAL AUDIO FILES " + R + (params.empty() ? std::string("\x1b[1m") : "\x1b[1;" + params + "m") + "\u25c0" + R;
        }
        if (!rt.empty()) pos(y, 72, rt);
        y++;
    }
    if (colors_first > 0) pos(3, W - 2, legend_sgr(settings_) + "\u25b2" + R);                              // more above
    if (colors_first + colors_visible < kColorsLines) pos(MAX_Y - 1, W - 2, legend_sgr(settings_) + "\u25bc" + R);   // more below
    clip_rows = 1 << 30;
    clip_top = 0;
    if (y > MAX_Y) y = MAX_Y;   // (a small window: the bottom border and the hint lines go where the panel ends)
}

void App::settings_tab_general(SettingsPaint& c) const {
    [[maybe_unused]] std::ostringstream& frame = c.frame; [[maybe_unused]] const int W = c.W; [[maybe_unused]] const int player_h = c.player_h; [[maybe_unused]] const int MAX_Y = c.MAX_Y;
    [[maybe_unused]] const std::string& R = c.R; [[maybe_unused]] const std::string& HI = c.HI;
    [[maybe_unused]] auto& B = c.B; [[maybe_unused]] auto& pos = c.pos; [[maybe_unused]] auto& scrollbar = c.scrollbar; [[maybe_unused]] auto& edit_field_width = c.edit_field_width; [[maybe_unused]] auto& edit_paint = c.edit_paint;
    [[maybe_unused]] int& clip_rows = c.clip_rows; [[maybe_unused]] int& clip_top = c.clip_top; [[maybe_unused]] int& y = c.y; [[maybe_unused]] int& colors_visible = c.colors_visible; [[maybe_unused]] int& colors_first = c.colors_first;
    // GENERAL and ANIMA/VIZ: category headers and their rows (settings_layout()). The list scrolls with the
    // selected row when the terminal is too short for it.
    const auto& lay = settings_layout(settings_tab_);
    const int visible = std::max(1, MAX_Y - 3);
    const int total = static_cast<int>(lay.size());
    const int first = settings_tab_first_line(W, visible);
    int row_of_line = 0;   // selectable row number of the line being drawn
    for (int i = 0; i < first; ++i) if (!lay[static_cast<size_t>(i)].header) ++row_of_line;
    for (int i = first; i < total && y < 3 + visible; ++i) {
        const SetLine& L = lay[static_cast<size_t>(i)];
        pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
        if (L.header) {
            pos(y, 4, header_sgr(settings_) + L.text + R);
        } else {
            const int r = row_of_line++;
            const bool sel = (r == settings_row_ && mode_ != Mode::ColorEdit);
            const bool ed = (r == settings_row_ && mode_ == Mode::ColorEdit);
            pos(y, 6, pad(L.text, 25)); pos(y, 32, ":");
            const bool buf_row = settings_tab_ == 1 && settings_item_id(1, r) == kSidTrackBuffer;
            const bool vol_row = (settings_tab_ == 1 || settings_tab_ == 2) && settings_item_id(settings_tab_, r) == kSidVolIndicator;   // two values: the look | gradient or not
            const bool term_row = settings_tab_ == 1 && settings_item_id(1, r) == kSidTermSize;       // two typed numbers: columns x rows
            std::string shown = settings_get_value(r, 0);
            // the "disk" look of the Volume Indicator is shown as two braille blocks of the animation itself
            if ((settings_tab_ == 1 || settings_tab_ == 2) && settings_item_id(settings_tab_, r) == kSidVolIndicator && settings_.volume_indicator == kVolStyleDisk)
                shown = volume_disk_snippet(volume_clock_angle());
            std::string v = ed ? edit_paint(edit_field_width()).s : pad(shown, 20);
            if (term_row) {   // "[ 120 ] x [ 30 ]": the field that is lit is the one ENTER types into (LEFT / RIGHT switch)
                const std::string c0 = settings_get_value(r, 0), c1 = settings_get_value(r, 1);
                auto field = [&](int which, const std::string& val) {
                    const bool mine = r == settings_row_ && settings_col_ == which;
                    if (ed && mine) return change_sgr(settings_) + edit_paint(5).s + R;
                    return std::string(mine && sel ? HI : "") + pad(val == "0" ? "off" : val, 5) + R;
                };
                pos(y, 35, field(0, c0));
                pos(y, 41, "x");
                pos(y, 43, field(1, c1));
                pos(y, 57, ((sel && settings_col_ == 2) ? HI : legend_sgr(settings_)) + "[ Set back to default ]" + R);   // (right of the two numbers)
            } else
            pos(y, 35, ((sel && !(buf_row && settings_col_ == 1) && !(vol_row && settings_col_ == 1)) ? HI : "") + (ed ? change_sgr(settings_) : "") + (vol_row ? pad(shown, 9) : v) + R);
            if (vol_row) {   // right of the look: "gradient" / "no gradient" (LEFT / RIGHT switch between the two, ENTER changes the one that is lit)
                pos(y, 45, ((sel && settings_col_ == 1) ? HI : "") + (settings_.volume_gradient ? "gradient   " : "no gradient") + R);
                if (sel) pos(y, 56, ansi_for(settings_.tab_current_color, false) + "[ENTER] change\x1b[0m");
            } else
            if (buf_row) {   // the button to the right of the number
                pos(y, 57, ((sel && settings_col_ == 1) ? HI : legend_sgr(settings_)) + "[ Set back to default ]" + R);
            } else
            if (sel && !settings_options_for(settings_tab_, r).empty())
                // Right behind the value, so long values such as "Cover behind Disk (Full Anima)" do not run into a fixed column.
                pos(y, std::max(57, 35 + static_cast<int>(display_width(shown)) + 2), ansi_for(settings_.tab_current_color, false) + "< \u2194 >\x1b[0m");
        }
        y++;
    }
    scrollbar(first, visible, total);   // there is more above / below
    settings_preview_draw(frame, W, MAX_Y, settings_tab_, B);   // the highlighted option, shown live on the right (wide terminals only)
}

void App::settings_tab_paths(SettingsPaint& c) const {
    [[maybe_unused]] std::ostringstream& frame = c.frame; [[maybe_unused]] const int W = c.W; [[maybe_unused]] const int player_h = c.player_h; [[maybe_unused]] const int MAX_Y = c.MAX_Y;
    [[maybe_unused]] const std::string& R = c.R; [[maybe_unused]] const std::string& HI = c.HI;
    [[maybe_unused]] auto& B = c.B; [[maybe_unused]] auto& pos = c.pos; [[maybe_unused]] auto& scrollbar = c.scrollbar; [[maybe_unused]] auto& edit_field_width = c.edit_field_width; [[maybe_unused]] auto& edit_paint = c.edit_paint;
    [[maybe_unused]] int& clip_rows = c.clip_rows; [[maybe_unused]] int& clip_top = c.clip_top; [[maybe_unused]] int& y = c.y; [[maybe_unused]] int& colors_visible = c.colors_visible; [[maybe_unused]] int& colors_first = c.colors_first;
    // PATHS tab: the LOCAL PATH / DOWNLOAD PATH / PLAYLIST PATH
    // sections (build_path_rows()) -- editable path rows, each list
    // ending in a "+ new path" row except the single download folder.
    // Scrolls as one list (viewport follows settings_row_, centered)
    // so a long list never grows the panel past player_h; see
    // path_display_row() for how a selectable row maps to the line it
    // is drawn on.
    int display_count = path_display_total();
    int visible = std::max(1, MAX_Y - 3);
    int cur_display = path_display_row(settings_row_);
    int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));

    int disp = 0;
    auto in_view = [&]() { return disp >= scroll && y < MAX_Y; };
    auto draw_header = [&](const char* text, const char* note = "") {
        // empty spacer row (just the side borders)
        if (in_view()) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            y++;
        }
        disp++;

        // the header itself, in the configurable Header color (bold;
        // palette index 10 by default, exactly what it always was)
        if (in_view()) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            pos(y, 6, header_sgr(settings_) + text + R);
            if (*note) pos(y, 6 + static_cast<int>(std::strlen(text)) + 3, legend_sgr(settings_) + note + R);
            y++;
        }
        disp++;
    };
    auto draw_path_row = [&](const PathRow& r) {
        if (r.kind == PathRow::Kind::Path && (r.label && *r.label)) {   // blank spacer above DOWNLOAD / PLAYLIST / HISTORY PATH
            if (in_view()) { pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R); y++; }
            disp++;
        }
        if (in_view()) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            const bool sel = (r.sel == settings_row_ && mode_ != Mode::ColorEdit);
            const bool ed = (r.sel == settings_row_ && mode_ == Mode::ColorEdit);
            if (r.kind == PathRow::Kind::Path) {
                // DOWNLOAD / PLAYLIST / HISTORY PATH are single paths: the section header says what they are, so the value
                // sits right under it (no second "Download Path :" label). Local paths are numbered.
                const bool single = (r.label && *r.label);   // DOWNLOAD / PLAYLIST / HISTORY PATH: the header name is the row label
                const int vx = 35;
                if (single) pos(y, 6, header_sgr(settings_) + pad(r.label, 25) + R);
                else pos(y, 6, pad("Local Path " + std::to_string(r.path_index + 1), 25));
                pos(y, 32, ":");
                // The value column runs from col 35 right up to the border --
                // a folder path is far longer than a hotkey's 20 columns, and
                // nothing may spill over the panel edge.
                int val_w = std::max(10, W - (vx + 1));
                std::string v;
                if (ed) {
                    // While editing the field is only as wide as what has
                    // been typed (20 minimum, growing with the text, never
                    // past the panel edge -- edit_field_width()), so an
                    // empty "+ new path" line no longer draws a red bar all
                    // the way to the right border. The window inside it
                    // follows the caret, not the tail, so a selection
                    // anywhere in a long path stays visible.
                    v = edit_paint(edit_field_width()).s;
                } else {
                    // The cursor highlight covers the entry, not the whole
                    // column: a path is highlighted exactly as wide as what
                    // is typed into it, the way a hotkey field highlights
                    // its own key. An empty row -- a not-yet-typed new path
                    // -- keeps the full-width bar instead, so there is a
                    // visible place to start typing.
                    std::string raw = settings_get_value(r.sel, 0);
                    int w = raw.empty()
                            ? val_w
                            : std::clamp(static_cast<int>(display_width(truncate_str(raw, val_w))), 1, val_w);
                    v = pad(truncate_str(raw, w), w);
                }
                pos(y, vx, (sel ? HI : "") + (ed ? change_sgr(settings_) : "") + v + R);
            } else { // AddPath
                pos(y, 6, pad(r.label, 25)); pos(y, 32, ":");
                pos(y, 35, (sel ? HI : "") + std::string(20, ' ') + R);
                if (sel) pos(y, 57, legend_sgr(settings_) + "[ENTER] add a line\x1b[0m");
            }
            y++;
        }
        disp++;
    };
    for (const PathRow& r : build_path_rows()) {
        if (y >= MAX_Y) break;
        if (r.kind == PathRow::Kind::Header) {
            const std::string lab = r.label;
            draw_header(r.label); (void)lab;
        }
        else if (r.kind == PathRow::Kind::Note) {
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, legend_sgr(settings_) + truncate_str(r.label, W - 8) + R);
                y++;
            }
            disp++;
        }
        else draw_path_row(r);
    }
    scrollbar(scroll, visible, display_count);
}

void App::settings_tab_reference(SettingsPaint& c) const {
    [[maybe_unused]] std::ostringstream& frame = c.frame; [[maybe_unused]] const int W = c.W; [[maybe_unused]] const int player_h = c.player_h; [[maybe_unused]] const int MAX_Y = c.MAX_Y;
    [[maybe_unused]] const std::string& R = c.R; [[maybe_unused]] const std::string& HI = c.HI;
    [[maybe_unused]] auto& B = c.B; [[maybe_unused]] auto& pos = c.pos; [[maybe_unused]] auto& scrollbar = c.scrollbar; [[maybe_unused]] auto& edit_field_width = c.edit_field_width; [[maybe_unused]] auto& edit_paint = c.edit_paint;
    [[maybe_unused]] int& clip_rows = c.clip_rows; [[maybe_unused]] int& clip_top = c.clip_top; [[maybe_unused]] int& y = c.y; [[maybe_unused]] int& colors_visible = c.colors_visible; [[maybe_unused]] int& colors_first = c.colors_first;
    // Reference tab: a one-line read-only note pointing at the cheat
    // sheet first, then the rebindable hotkeys grouped under
    // category headers (kRefRows), then a read-only display of the font-mapping table (section 4 of the config, "A={A,a}" style)
    // loaded from config.txt -- as "A = A, a" rows. Combined they're
    // usually taller than the player view, so this scrolls as one list
    // (viewport follows settings_row_, centered) rather than ever
    // growing the panel past player_h. See ref_display_row() for how a
    // selectable row maps to the row it's drawn on.
    std::vector<char> letters;
    for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) letters.push_back(c);
    int total_selectable = kRefEnd + static_cast<int>(letters.size());
    int display_count = ref_display_row(total_selectable - 1) + 1;
    int visible = std::max(1, MAX_Y - 3);
    int cur_display = ref_display_row(settings_row_);
    int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));

    // Walk every display line (headers + rows) in order, only
    // actually drawing (and advancing y) once we're inside the
    // visible scroll window -- same "disp/scroll/visible" shape the
    // rest of this file's scrolling panels use.
    int disp = 0;
    auto in_view = [&]() { return disp >= scroll && y < MAX_Y; };
    auto draw_header = [&](const char* text) {
        // empty spacer row (just the side borders)
        if (in_view()) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            y++;
        }
        disp++;

        // the header itself, in the configurable Header color (bold;
        // palette index 10 by default, exactly what it always was)
        if (in_view()) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            pos(y, 6, header_sgr(settings_) + text + R);
            y++;
        }
        disp++;
    };
    auto draw_hotkey_row = [&](int selectable_row) {
        const RefHotkeyRow& row = kRefRows[selectable_row - kRefStart];
        if (in_view()) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            pos(y, 6, pad(row.label, 25)); pos(y, 32, ":");
            bool sel = (selectable_row == settings_row_ && mode_ != Mode::ColorEdit);
            bool ed = (selectable_row == settings_row_ && mode_ == Mode::ColorEdit);
            std::string v = ed ? edit_paint(edit_field_width()).s : pad(pretty_key(settings_get_value(selectable_row, 0)), 20);
            pos(y, 35, (sel ? HI : "") + (ed ? change_sgr(settings_) : "") + v + R);
            y++;
        }
        disp++;
    };
    auto draw_note = [&](const char* text) {
        // blank spacer line, then the read-only note in the legend color.
        // Two display lines, matching the `headers += 2` that
        // ref_display_row() adds just before the hotkeys.
        if (in_view()) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            y++;
        }
        disp++;
        if (in_view()) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            pos(y, 6, legend_sgr(settings_) + text + R);
            y++;
        }
        disp++;
    };
    auto draw_reset_row = [&]() {
        if (in_view()) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            pos(y, 6, pad("Reset All Keys To Default", 25)); pos(y, 32, ":");
            const bool sel = settings_row_ == 0 && mode_ != Mode::ColorEdit;
            pos(y, 35, (sel ? HI : "") + pad("[ENTER] reset", 20) + R);
            y++;
        }
        disp++;
    };
    auto draw_font_row = [&](char c, int selectable_row) {
        if (in_view()) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            const auto& pair = settings_.font_map.at(c);
            bool sel = (selectable_row == settings_row_);
            std::string line = std::string(1, c) + " = " + pair.first + ", " + pair.second;
            pos(y, 6, (sel ? HI : "") + line + R);
            y++;
        }
        disp++;
    };

    if (y < MAX_Y) draw_note(kRefNote);
    if (y < MAX_Y) draw_reset_row();
    for (int i = 0; i < kRefRowCount && y < MAX_Y; ++i) {
        if (kRefRows[i].header) draw_header(kRefRows[i].header);
        if (y < MAX_Y) draw_hotkey_row(kRefStart + i);
    }
    if (y < MAX_Y) draw_header("FONT / CHARACTER MAP");
    for (size_t li = 0; li < letters.size() && y < MAX_Y; ++li)
        draw_font_row(letters[li], kRefEnd + static_cast<int>(li));
    scrollbar(scroll, visible, display_count);
}

void App::settings_tab_about(SettingsPaint& c) const {
    [[maybe_unused]] std::ostringstream& frame = c.frame; [[maybe_unused]] const int W = c.W; [[maybe_unused]] const int player_h = c.player_h; [[maybe_unused]] const int MAX_Y = c.MAX_Y;
    [[maybe_unused]] const std::string& R = c.R; [[maybe_unused]] const std::string& HI = c.HI;
    [[maybe_unused]] auto& B = c.B; [[maybe_unused]] auto& pos = c.pos; [[maybe_unused]] auto& scrollbar = c.scrollbar; [[maybe_unused]] auto& edit_field_width = c.edit_field_width; [[maybe_unused]] auto& edit_paint = c.edit_paint;
    [[maybe_unused]] int& clip_rows = c.clip_rows; [[maybe_unused]] int& clip_top = c.clip_top; [[maybe_unused]] int& y = c.y; [[maybe_unused]] int& colors_visible = c.colors_visible; [[maybe_unused]] int& colors_first = c.colors_first;
    // About App: shows the hard coded about_app_text() (about_text.h),
    // scrolled so it never exceeds player_h.
    int visible = std::max(1, MAX_Y - 3);
    int total = static_cast<int>(about_app_text().size());
    int scroll = std::clamp(settings_row_, 0, std::max(0, total - visible));
    // the text is centred: each line in the middle of the box (by the longest line) and the block sits vertically in the
    // middle of the rows, moved two rows up; a longer text scrolls as before (then it starts at the top)
    int bw = 0;
    for (const std::string& s : about_app_text()) bw = std::max(bw, display_width(s));
    const int x0 = 2 + std::max(0, (W - 2 - bw) / 2);
    const int top = total >= visible ? 0 : std::max(0, (visible - total) / 2 - 2);
    for (int r = 0; r < visible; ++r) {
        pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
        int idx = scroll + r - top;
        if (r >= top && idx >= 0 && idx < total) pos(y, x0, about_app_text()[idx]);
        y++;
    }
    scrollbar(scroll, visible, total);
}

void App::build_settings_screen(std::ostringstream& frame, int W, int player_h) const {
    g_settings_hifi_rows = settings_.bars_style == 1;   // (the Hi-Fi rows of SPECTRUM BARS are listed only with that style)
    // Literal port of the reference SettingsEngine::render() -- same
    // columns, same labels, same schema text, same group-blanking, same
    // divider, same tab-wrap algorithm, same gradient border (applied to
    // this panel's own chrome too, not just the preview swatches), same
    // preview formulas, same bottom hint/status lines, same cursor
    // placement formula. Deliberately not "improved" or restructured.
    if (W < 80) W = 80;
    if (theme_ui_ != 0) { build_theme_screen(frame, W, player_h); return; }
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    // MAX_Y is the row of this panel's own bottom border; the hint line
    // and status line render below it (rows MAX_Y+1, MAX_Y+2), so the
    // total screen rows used here is MAX_Y+2 -- set to exactly match
    // player_h (total rows the Browse-mode view renders), never taller,
    // never shorter, per explicit requirement. Floored modestly so a
    // pathologically small player view still leaves room to render the
    // tab bar/hint/status at all.
    int MAX_Y = std::max(player_h - 1, 11);
    auto B = [&](int y) {
        if (!settings_.border_smooth) return ansi_for(y >= MAX_Y ? settings_.border_color_bottom : settings_.border_color, false); // rough: only the bottom line changes
        return gradient_ansi(settings_.border_color, settings_.border_color_bottom,
                             (MAX_Y > 1) ? static_cast<float>(y - 1) / (MAX_Y - 1) : 0.0f, false);
    };
    int clip_rows = 1 << 30;   // the colours tab is longer than a small window: its rows from here on are not drawn
    int clip_top = 0;          // ... and the ones above this row were scrolled off
    auto pos = [&](int y, int x, const std::string& s) { if (x <= W && y < clip_rows && y >= clip_top) frame << "\x1b[" << y << ";" << x << "H" << clip_to_columns(s, x, (x == 1 || x == W) ? W : W - 1); };   // (a row that does not fit is cut before the right border)
    // The scrollbar of a tab that is longer than the panel, the same as in the help wizard: the last column inside the
    // frame, a thumb as tall as the visible part is of the whole, in the disk gradient. `first` is the first line shown.
    auto scrollbar = [&](int first, int visible, int total) {
        overlay::Thumb th;
        if (!overlay::scroll_thumb(first, visible, total, th)) return;
        for (int r = 0; r < visible && 3 + r < MAX_Y; ++r) {
            const bool on = r >= th.at && r < th.at + th.len;
            pos(3 + r, W - 1, gradient_ansi(settings_.disk_color, settings_.disk_color_end,
                                            visible > 1 ? static_cast<float>(r) / static_cast<float>(visible - 1) : 0.0f) + (on ? "\u2588" : "\u2591") + R);
        }
    };
    // Matches pad(): no truncation if s is already >= width, just like
    // the reference -- a longer-than-expected value overflows into the
    // next column rather than getting cut off. Values here are always
    // short in practice (numbers, true/false, single-char hotkeys).

    // --- in-place value editing (mode_ == Mode::ColorEdit) -----------------
    // Width of the value field on the row currently being edited. The Colors
    // and non-path rows keep their fixed 7/20 columns; a path row is 20
    // columns minimum and grows with what has actually been typed, never
    // past the panel's right edge. It is not drawn at full width when still empty, because that would show
    // a red bar to the far right.
    auto edit_field_width = [&]() -> int {
        if (settings_tab_ == 0) return 7;
        if (settings_tab_ == 3 && path_row_is_text(settings_row_)) {
            const int edge = std::max(10, W - 36);
            const int want = display_width(color_edit_buffer_) + 1;
            return std::min(edge, std::max(20, want));
        }
        return 20;
    };
    // The painted value itself: selection reversed, window scrolled so the
    // caret stays inside the field, then padded to the full field width.
    // pad() cannot do this -- it measures with display_width(), which does
    // not see through the ANSI codes paint_edit_field() emits, so the column
    // count comes back with them included.
    auto edit_paint = [&](int width) {
        size_t c = std::min(edit_caret_, color_edit_buffer_.size());
        size_t a = std::min(edit_anchor_, color_edit_buffer_.size());
        EditPaint p = paint_edit_field(color_edit_buffer_, c, a, width, change_sgr(settings_), false);
        p.s += std::string(std::max(0, width - p.cols), ' ');
        return p;
    };

    static const char* kTabNames[] = {"COLORS", "GENERAL", "ANIMA/VIZ", "PATHS", "REFERENCE", "ABOUT APP"};

    // 1. Tab-wrap algorithm.
    std::vector<int> top_tabs, bot_tabs;
    // Uses the widths the top border line really occupies (see the drawing
    // loop below): the "SETTINGS" header is 22 columns; every tab is
    // "  " + label + "  " + "┌" = 5 + label columns, followed by a 3-column
    // separator ("──┐") -- or a single "─" if it is the last tab on the
    // line. The active tab's label also carries "[" "]" (+2). The row must
    // end at column W-1 at the latest (the final "┐" sits in column W).
    // The old estimate (name + 10 per tab, strict "<") over-counted by 2+
    // columns per tab, so with six tabs ABOUT APP was pushed onto the
    // bottom border although the top border had room.
    int w_track = 22; // columns used so far, every tab counted with its 3-col separator
    bool top_full = false;
    for (int i = 0; i < kSettingsTabCount; ++i) {
        const int lab_cols = static_cast<int>(std::string(kTabNames[i]).size())
                             + (i == settings_tab_ ? 2 : 0);
        const int tab_cols = 5 + lab_cols;
        // Assume this tab is the last on the line (trailing "─" = 1 column).
        if (!top_full && w_track + tab_cols + 1 <= W - 1) {
            top_tabs.push_back(i);
            w_track += tab_cols + 3;
        } else {
            top_full = true; // keep the tab order: once one spills, all later ones do too
            bot_tabs.push_back(i);
        }
    }
    int w = 0;
    std::string l1, l2;
    auto add = [&](const std::string& t, const std::string& b, int width) { l1 += t; l2 += b; w += width; };
    add("\u250c\u2500 SETTINGS \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2510", "\u2502                    \u2514", 22);
    for (size_t idx = 0; idx < top_tabs.size(); ++idx) {
        int i = top_tabs[idx];
        std::string lab = (i == settings_tab_) ? ("[" + std::string(kTabNames[i]) + "]") : kTabNames[i];
        int lab_len = static_cast<int>(lab.size());
        bool is_last = (idx == top_tabs.size() - 1);
        add("  " + R + ansi_for(i == settings_tab_ ? settings_.tab_current_color : settings_.tab_other_color, false) + lab + R + B(1) + "  \u250c",
            repeat("\u2500", 4 + lab_len) + "\u2518", 5 + lab_len);
        if (is_last) add("\u2500", " ", 1);
        else add("\u2500\u2500\u2510", "  \u2514", 3);
    }
    if (w < W - 1) add(repeat("\u2500", W - 1 - w), repeat(" ", W - 1 - w), W - 1 - w);
    add("\u2510", "\u2502", 1);
    pos(1, 1, B(1) + l1 + R);
    pos(2, 1, B(2) + l2 + R);

    // 2. Content.
    int y = 3;
    int colors_visible = 0, colors_first = 0;
    SettingsPaint c{frame, W, player_h, MAX_Y, R, HI, B, pos, scrollbar, edit_field_width, edit_paint, clip_rows, clip_top, y, colors_visible, colors_first};
    if (settings_tab_ == 0) settings_tab_colors(c);
    else if (settings_tab_ == 1 || settings_tab_ == 2) settings_tab_general(c);
    else if (settings_tab_ == 3) settings_tab_paths(c);
    else if (settings_tab_ == 4) settings_tab_reference(c);
    else if (settings_tab_ == 5) settings_tab_about(c);

    while (y < MAX_Y) { pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R); y++; }

    // 3. Bottom frame & overflow tabs.
    if (bot_tabs.empty()) {
        pos(y, 1, B(y) + "\u2514" + repeat("\u2500", W - 2) + "\u2518" + R);
    } else {
        // Measured in display COLUMNS, not bytes: "\u2514" and "\u2500" are 3 bytes
        // but one column each, so bot.size() over-counted by 2 per box-drawing
        // character and the row came out ~4 columns short of the right border.
        // Every tab segment is name + 5 columns: " [" NAME "] " or "  " NAME "  ",
        // plus the trailing "\u2500". A tab that would not fit before the closing
        // "\u2518" is dropped rather than letting the row wrap.
        std::string bot = "\u2514\u2500";
        int bot_cols = 2;
        for (int i : bot_tabs) {
            const int seg_cols = static_cast<int>(std::string(kTabNames[i]).size()) + 5;
            if (bot_cols + seg_cols > W - 1) break;
            bot += (i == settings_tab_ ? " [" + std::string(kTabNames[i]) + "] \u2500" : "  " + std::string(kTabNames[i]) + "  \u2500");
            bot_cols += seg_cols;
        }
        const int rem_bot = std::max(0, W - 1 - bot_cols); // columns of "\u2500" before the corner
        pos(y, 1, B(y) + bot + repeat("\u2500", rem_bot) + "\u2518" + R);
    }
    y++;

    // While a field is being edited (ENTER), TAB and the arrows stay in it:
    // LEFT/RIGHT move the caret, and only ENTER (apply) or ESC (cancel) leave.
    if (leave_prompt_ == 1) pos(y, 1, "\x1b[1;7m" + pad(" Unsaved changes! [s] Save and leave  [d] Discard and leave  [ESC] Keep editing", W - 1) + "\x1b[0m");
    else
    pos(y, 1, legend_colored(settings_, truncate_str(mode_ == Mode::ColorEdit
        ? "[ENTER] Apply  [ESC] Cancel  [\u2190\u2192] Cursor  [SHIFT+\u2190\u2192] Mark  [HOME/END] Jump  [Ctrl+C/X/V] Copy/Cut/Paste"
        : settings_tab_ == 4
        ? "[TAB] Switch  [ENTER] Change  [DEL] Default  [Ctrl+Shift+U] Undo  [s] Save settings  [SHIFT+\u2191\u2193] Page  [ESC/q] Discard"
        : settings_tab_ == 0 ? "[TAB] Switch  [ENTER] Edit  [e] Edit/Save current state  [SHIFT+e/i] Ex-/Import theme  [s] Save settings  [SHIFT+\u2191\u2193] Page  [ESC/q] Discard"
        : "[TAB] Switch  [ENTER] Edit  [s] Save settings  [SHIFT+\u2191\u2193] Page  [ESC/q] Discard", std::max(1, W - 1))) + "\x1b[0m");
    y++;
    // The log/status line lives here now -- render_frame() deliberately no
    // longer prints it under the Browse list (an untruncated message there
    // could exceed the terminal width, wrap, and push a full-height frame
    // into a scroll, which is what made the terminal's own scrollbar appear
    // and shift the whole UI left by a column). This row sits on the panel's
    // own last screen line, so it is truncated to the panel width: a long
    // path or error message just ends early instead of wrapping.
    // (the message of the last change is shown even when there are unsaved changes, so a change that made
    // the settings differ from the saved ones still shows its message)
    if (settings_dirty_ && status_line_.empty()) pos(y, 1, "\x1b[38;5;246mUnsaved changes! Save with `s` or discard with `ESC`.\x1b[0m");
    else if (settings_dirty_) pos(y, 1, "\x1b[38;5;246m" + truncate_str(status_line_, std::max(1, W - 2 - 11)) + "\x1b[0m\x1b[33m (unsaved)\x1b[0m");
    else if (!status_line_.empty()) pos(y, 1, "\x1b[38;5;246m" + truncate_str(status_line_, W - 2) + "\x1b[0m");

    // 4. In-place text editing cursor placement.
    if (mode_ == Mode::ColorEdit) {
        // The caret column comes from the same window the renderer painted
        // (edit_field_width() + paint_edit_field()), so it cannot disagree
        // with what is on screen: byte length could -- a multi-byte
        // character is one column but many bytes, and a path longer than the
        // field is scrolled to the caret rather than showing its tail.
        const size_t ec = std::min(edit_caret_, color_edit_buffer_.size());
        const size_t ea = std::min(edit_anchor_, color_edit_buffer_.size());
        const int caret_col = paint_edit_field(color_edit_buffer_, ec, ea, edit_field_width(), "", false).caret;
        if (settings_tab_ == 0) {
            const int cc = std::max(0, color_row_of_tab(settings_row_));
            int cy = 3 + colors_line_of(cc) - colors_first_line(settings_row_, std::max(1, MAX_Y - 3));
            int cx = ((settings_col_ == 0) ? 38 : 61) + caret_col;
            frame << "\x1b[" << cy << ";" << cx << "H\x1b[?25h";
        } else {
            int cy = 3 + settings_row_;
            int cx = 35 + caret_col;
            if (settings_tab_ == 3) { const PathRow cr = path_row(settings_row_); (void)cr; }
            if (settings_tab_ == 1 || settings_tab_ == 2) {
                // GENERAL and ANIMA/VIZ scroll when the terminal is short: recompute the exact scroll the renderer used.
                const int visible = std::max(1, MAX_Y - 3);
                cy = 3 + (settings_item_line(settings_tab_, settings_row_) - settings_tab_first_line(W, visible));
            } else if (settings_tab_ == 3) {
                // PATHS tab scrolls once its row list exceeds the visible
                // window. Recompute the same scroll offset used when
                // rendering (see the settings_tab_==3 branch above, and
                // path_display_row()) so the text cursor lands on the row
                // actually drawn there. Reached only for path rows -- an
                // AddPath row opens a new empty path row before editing.
                int visible = std::max(1, MAX_Y - 3);
                int cur_display = path_display_row(settings_row_);
                int display_count = path_display_total();
                int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));
                cy = 3 + (cur_display - scroll);
            } else if (settings_tab_ == 4) {
                // Reference tab scrolls once its row list exceeds the
                // visible window -- the common case, since it holds every
                // rebindable hotkey plus any font-map rows. Recompute the
                // same scroll offset used when rendering (see the
                // settings_tab_==4 branch above, and ref_display_row()) so
                // the text cursor lands on the row actually drawn there
                // instead of one that's already scrolled off-screen. Reached
                // only for the rebindable hotkeys -- the Enter handler
                // blocks ColorEdit for the read-only font-map rows.
                int visible = std::max(1, MAX_Y - 3);
                int cur_display = ref_display_row(settings_row_);
                std::vector<char> letters;
                for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) letters.push_back(c);
                int total_selectable = kRefEnd + static_cast<int>(letters.size());
                int display_count = ref_display_row(total_selectable - 1) + 1;
                int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));
                cy = 3 + (cur_display - scroll);
            }
            frame << "\x1b[" << cy << ";" << cx << "H\x1b[?25h";
        }
    }
}


} // namespace muisc
