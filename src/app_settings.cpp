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

void App::settings_open() {
    text_disengage();
    settings_snapshot_ = settings_;
    settings_snapshot_text_ = settings_to_text(settings_);
    settings_dirty_ = false;
    leave_prompt_ = 0;
    theme_ui_ = 0;
    theme_list_ = themes::load_all();
}

void App::settings_update_dirty() {
    settings_dirty_ = settings_to_text(settings_) != settings_snapshot_text_;
}

// `s`: write config.txt and leave. ESC / q: leave and put back everything the screen changed.
void App::settings_close(bool save) {
    if (save) {
        save_settings(settings_);
        status_line_ = "SAVED";
    } else {
        settings_update_dirty();
        if (settings_dirty_) {
            const Settings before = settings_;
            settings_ = settings_snapshot_;
            // what was applied live while the screen was open
            cache_.set_download_dir(path_from_utf8(settings_.download_folder));
            if (before.history_path != settings_.history_path) {
                history_end_current_play();
                history_.save();
                history_.set_dir(settings_.history_path);
                if (history_.file_exists()) history_.load(); else history_.save();
            }
            player_.set_stereo(settings_.stereo);
            player_.set_normalization(settings_.normalize, static_cast<float>(settings_.normalize_target_lufs),
                                      static_cast<float>(settings_.normalize_max_boost_db));
            set_emoji_mode(settings_.emoji_mode);
            if (before.local_music_paths != settings_.local_music_paths || before.download_folder != settings_.download_folder)
                rescan_library();
            force_redraw_ = true;
        }
        status_line_.clear();
    }
    settings_dirty_ = false;
    theme_ui_ = 0;
    remote_ov_ = remote_ui::State{};
    mode_ = Mode::Browse;
    remote_app_apply();   // a discarded Remote App switch is undone here
}

// Rebuilds the row list of the PATHS tab: a "LOCAL PATH" header +
// one row per configured local music path + a "+ new path" row, then the
// single DOWNLOAD PATH row, then the same three for the playlist paths.
// Headers are display-only (sel stays -1); everything else gets the next
// selectable index in order, starting at 0.
//
// A section whose vector is still empty still shows one (empty) row: an
// unset path has to be an editable blank field, not a missing one,
// otherwise there'd be nowhere to type the very first path.
std::vector<App::PathRow> App::build_path_rows() const {
    std::vector<PathRow> rows;
    rows.reserve(10);

    int sel = 0;

    auto add_path_section = [&](const char* header, bool playlist) {
        PathRow h;
        h.kind = PathRow::Kind::Header;
        h.label = header;
        rows.push_back(h);

        const std::vector<std::string>& paths = playlist ? settings_.playlists_paths
                                                         : settings_.local_music_paths;
        int count = std::max(1, static_cast<int>(paths.size()));
        for (int i = 0; i < count; ++i) {
            PathRow p;
            p.kind = PathRow::Kind::Path;
            p.sel = sel++;
            p.path_index = i;
            p.playlist_path = playlist;
            rows.push_back(p);
        }

        PathRow add;
        add.kind = PathRow::Kind::AddPath;
        add.sel = sel++;
        add.playlist_path = playlist;
        add.label = "+ new path";
        rows.push_back(add);
    };

    add_path_section("LOCAL PATH", false);

    // DOWNLOAD PATH: yt-dlp's single output folder, deliberately NOT run
    // through add_path_section() -- there is only ever one place downloads
    // land, so there is nothing to list and no "+ new path" to add. Reads
    // and writes settings_.download_folder (see settings_get_value() /
    // settings_commit_edit()) and is auto-injected into the local paths at
    // load/rescan, so setting it is all it takes for downloads to appear
    // in the library.
    {
        PathRow d;
        d.label = "DOWNLOAD PATH";
        d.kind = PathRow::Kind::Path;
        d.sel = sel++;
        d.path_index = -1; // no vector index: d.download_folder says who owns it
        d.download_folder = true;
        rows.push_back(d);
    }

    // PLAYLIST PATH: ONE folder (stored as playlists_paths[0]), like the download folder: no list, no "+ new path".
    {
        PathRow p;
        p.label = "PLAYLIST PATH";
        p.kind = PathRow::Kind::Path;
        p.sel = sel++;
        p.path_index = 0;
        p.playlist_path = true;
        rows.push_back(p);

        PathRow n;
        n.kind = PathRow::Kind::Note;
        n.label = "Changing it copies the existing playlists to the new folder.";
        rows.push_back(n);
    }

    // PLAYLIST EXPORT PATH: ONE folder (settings_.playlist_export_path) that 'e' on the Saved Playlists tab exports to.
    {
        PathRow p;
        p.label = "PLAYLIST EXPORT PATH";
        p.kind = PathRow::Kind::Path;
        p.sel = sel++;
        p.path_index = -1;
        p.export_folder = true;
        rows.push_back(p);

        PathRow n;
        n.kind = PathRow::Kind::Note;
        n.label = "Where [e] on the Saved Playlists tab exports M3U8 / M3U files (default: export, inside the playlist folder).";
        rows.push_back(n);
    }

    // PLAYLIST IMPORT PATH: ONE folder (settings_.playlist_import_path) that 'i' on the Saved Playlists tab reads from.
    {
        PathRow p;
        p.label = "PLAYLIST IMPORT PATH";
        p.kind = PathRow::Kind::Path;
        p.sel = sel++;
        p.path_index = -1;
        p.import_folder = true;
        rows.push_back(p);

        PathRow n;
        n.kind = PathRow::Kind::Note;
        n.label = "Where [i] looks for M3U8 / M3U files (default: import, inside the playlist folder).";
        rows.push_back(n);
        PathRow n2;
        n2.kind = PathRow::Kind::Note;
        n2.label = "The files stay there; imported lists are copied into the imported_lists folder of the playlist folder.";
        rows.push_back(n2);
    }

    // CONVERSION EXPORT PATH: ONE folder (settings_.conversion_export_path) the conversion overlay (SHIFT+c) writes to.
    {
        PathRow p;
        p.label = "CONVERSION EXPORT PATH";
        p.kind = PathRow::Kind::Path;
        p.sel = sel++;
        p.path_index = -1;
        p.conversion_folder = true;
        rows.push_back(p);

        PathRow n;
        n.kind = PathRow::Kind::Note;
        n.label = "Where the file conversion (SHIFT+c) saves converted files (empty = \"conversion-export\" in the download folder).";
        rows.push_back(n);
    }

    // HISTORY PATH: ONE folder (settings_.history_path) that holds history.json.
    {
        PathRow p;
        p.label = "HISTORY PATH";
        p.kind = PathRow::Kind::Path;
        p.sel = sel++;
        p.path_index = -1;
        p.history_folder = true;
        rows.push_back(p);

        PathRow n;
        n.kind = PathRow::Kind::Note;
        n.label = "An existing history.json in the new folder is used, otherwise the current one is copied there.";
        rows.push_back(n);
    }

    // AUDIO BOOK PATH: ONE folder (settings_.audiobook_path) -- the library of the audio book mode (SHIFT and +).
    {
        PathRow p;
        p.label = "AUDIO BOOK PATH";
        p.kind = PathRow::Kind::Path;
        p.sel = sel++;
        p.path_index = -1;
        p.audiobook_folder = true;
        rows.push_back(p);

        PathRow n;
        n.kind = PathRow::Kind::Note;
        n.label = "Audio books: every folder in it with audio files is one book.";
        rows.push_back(n);
        PathRow n2;
        n2.kind = PathRow::Kind::Note;
        n2.label = "A .cue file beside each book records where you last stopped listening.";
        rows.push_back(n2);
    }

    return rows;
}

App::PathRow App::path_row(int selectable_row) const {
    PathRow none; // sel == -1 == "no such row"
    if (selectable_row < 0) return none;
    for (const PathRow& r : build_path_rows()) if (r.sel == selectable_row) return r;
    return none;
}

int App::path_row_count() const {
    int n = 0;
    for (const PathRow& r : build_path_rows()) if (r.sel >= 0) ++n;
    return n;
}

bool App::path_row_is_text(int selectable_row) const {
    PathRow r = path_row(selectable_row);
    return r.sel >= 0 && r.kind == PathRow::Kind::Path;
}

// Maps a selectable PATHS row to the line it is drawn on, once the section
// headers (a blank spacer above the title plus the title itself, i.e. two
// lines each) are accounted for. Used by both the render block and the
// ColorEdit cursor placement, so the two always agree on where a given
// row lands.
// Number of display lines of the whole PATHS list (headers 2, notes and rows 1).
int App::path_display_total() const {
    int disp = 0;
    for (const PathRow& r : build_path_rows()) disp += (r.kind == PathRow::Kind::Header || (r.kind == PathRow::Kind::Path && (r.label && *r.label))) ? 2 : 1;
    return disp;
}

int App::path_display_row(int selectable_row) const {
    int disp = 0;
    for (const PathRow& r : build_path_rows()) {
        if (r.kind == PathRow::Kind::Header) { disp += 2; continue; }
        if (r.kind == PathRow::Kind::Note) { disp++; continue; }
        if (r.sel == selectable_row) return disp + (r.kind == PathRow::Kind::Path && (r.label && *r.label) ? 1 : 0);
        disp += (r.kind == PathRow::Kind::Path && (r.label && *r.label)) ? 2 : 1;
    }
    return disp; // out of range: one past the end
}

// Maps a selectable REFERENCE row -- kRefStart..kRefEnd-1 for the hotkeys,
// then the font-map letters -- to the row it's actually drawn on, once the
// section header/divider lines inserted along the way (one for the
// read-only note at the top, one above each hotkey category, one above the
// font map) are accounted for. Every header contributes two display rows
// (a blank spacer above it plus the title itself). Used by both the render
// block and the ColorEdit cursor placement below.
int App::ref_display_row(int selectable_row) const {
    int headers = 0;
    for (int i = 0; i <= selectable_row; ++i) {
        if (i < kRefEnd) {
            if (i == 0) headers += 2;            // the read-only note (blank + text), just before the reset line
            if (i >= kRefStart && kRefRows[i - kRefStart].header) headers += 2;
        } else {
            if (i == kRefEnd) headers += 2;      // "FONT / CHARACTER MAP"
        }
    }
    return selectable_row + headers;
}

static std::string* color_cell(Settings& s, int row, int col) {
    switch (row) {
        case 0: return col == 0 ? &s.border_color : &s.border_color_bottom;
        case 1: return col == 0 ? &s.disk_color : &s.disk_color_end;
        case 2: return col == 0 ? &s.meta_key_color : &s.meta_val_color;
        case 3: return col == 0 ? &s.visualizer_color : &s.visualizer_color_end;
        case 4: return col == 0 ? &s.progress_played_color : &s.progress_remaining_color;
        case 5: return col == 0 ? &s.search_text_color : &s.search_bg_color; // SEARCH: search bar input
        case 6: return col == 0 ? &s.search_inactive_text_color : &s.search_inactive_bg_color; // SEARCH INACTIVE: the idle bar
        case 7: return col == 0 ? &s.list_color : &s.list_inactive_bg_color;
        case 8: return col == 0 ? &s.list_playing_color : &s.list_playing_bg_color;
        case 9: return col == 0 ? &s.list_cursor_color : &s.list_cursor_bg_color;
        case 10: return col == 0 ? &s.queue_color : &s.queue_inactive_bg_color;
        case 11: return col == 0 ? &s.queue_playing_color : &s.queue_playing_bg_color;
        case 12: return col == 0 ? &s.queue_cursor_color : &s.queue_cursor_bg_color;
        case 13: return col == 0 ? &s.inactive_line_color : &s.inactive_line_bg_color;
        case 14: return col == 0 ? &s.active_line_color : &s.active_line_bg_color;
        case 15: return col == 0 ? &s.active_word_color : &s.active_word_bg_color;
        case 16: return col == 0 ? &s.header_color : nullptr; // HEADER: text color only, no background cell
        case 17: return col == 0 ? &s.legend_color : &s.legend_cmd_color; // LEGEND: KEY-CMD (the keys) | COMMAND (their descriptions), both text colors
        case 18: return col == 0 ? &s.tab_current_color : &s.tab_other_color; // TAB NAMES: current / other (both text colours)
        case 19: return col == 0 ? &s.change_text_color : &s.change_bg_color; // CHANGE: a field actively being typed into
        case 20: return col == 0 ? &s.focus_color : nullptr; // FOCUS: the pane focus indicator, text color only
        default: return nullptr;
    }
}

// The Colors tab, top to bottom: row 0 = THEME | CREATE THEME, row 1 = BORDER_COLOR, row 2 = SMOOTHNESS (rough /
// smooth), rows 3.. = the other colour rows. color_cell() and the theme creator count the colour rows alone (0..20).
static int color_row_of_tab(int t) { return t == 1 ? 0 : (t >= 3 ? t - 2 : -1); }
static int tab_row_of_color(int c) { return c == 0 ? 1 : c + 2; }
// The Colours tab is 25 lines long (THEME, a rule, 21 colour rows, SMOOTHNESS and one more rule). A window that is too short for
// all of them shows a part, scrolled so that the chosen row stays in view (the same rule as the PATHS tab: the row in the middle).
constexpr int kColorsLines = 25;
static int colors_line_of(int c) { return 2 + c + (c >= 1 ? 1 : 0) + (c >= 5 ? 1 : 0); }   // the line of colour row c (0 = the first line of the tab)
static int colors_sel_line(int settings_row) {
    if (settings_row <= 0) return 0;
    if (settings_row == 2) return 3;   // SMOOTHNESS
    for (int c = 0; c < 21; ++c) if (tab_row_of_color(c) == settings_row) return colors_line_of(c);
    return 0;
}
static int colors_first_line(int settings_row, int visible) {
    return std::clamp(colors_sel_line(settings_row) - visible / 2, 0, std::max(0, kColorsLines - visible));
}

std::string* App::color_field_ptr(int row, int col) {
    const int c = color_row_of_tab(row);
    return c < 0 ? nullptr : color_cell(settings_, c, col);
}

// ---- colour themes: the Settings fields a theme sets, keyed like config.txt ------------------------------
namespace {
struct ThemeKey { const char* key; std::string Settings::* p; };
const ThemeKey kThemeKeys[] = {
    {"ColorBorderTop", &Settings::border_color},            {"ColorBorderBottom", &Settings::border_color_bottom},
    {"ColorDiskTop", &Settings::disk_color},                {"ColorDiskBottom", &Settings::disk_color_end},
    {"ColorMetadataKey", &Settings::meta_key_color},        {"ColorMetadataVal", &Settings::meta_val_color},
    {"ColorVizLeft", &Settings::visualizer_color},          {"ColorVizRight", &Settings::visualizer_color_end},
    {"ColorProgressBarPlayed", &Settings::progress_played_color}, {"ColorProgressBarPending", &Settings::progress_remaining_color},
    {"ColorSearchFg", &Settings::search_text_color},        {"ColorSearchBg", &Settings::search_bg_color},
    {"ColorSearchInactiveFg", &Settings::search_inactive_text_color}, {"ColorSearchInactiveBg", &Settings::search_inactive_bg_color},
    {"ColorListInactiveFg", &Settings::list_color},         {"ColorListInactiveBg", &Settings::list_inactive_bg_color},
    {"ColorListPlayingFg", &Settings::list_playing_color},  {"ColorListPlayingBg", &Settings::list_playing_bg_color},
    {"ColorListCursorFg", &Settings::list_cursor_color},    {"ColorListCursorBg", &Settings::list_cursor_bg_color},
    {"ColorQueueInactiveFg", &Settings::queue_color},       {"ColorQueueInactiveBg", &Settings::queue_inactive_bg_color},
    {"ColorQueuePlayingFg", &Settings::queue_playing_color}, {"ColorQueuePlayingBg", &Settings::queue_playing_bg_color},
    {"ColorQueueCursorFg", &Settings::queue_cursor_color},  {"ColorQueueCursorBg", &Settings::queue_cursor_bg_color},
    {"ColorLyricsInactiveFg", &Settings::inactive_line_color}, {"ColorLyricsInactiveBg", &Settings::inactive_line_bg_color},
    {"ColorLyricsActiveLineFg", &Settings::active_line_color}, {"ColorLyricsActiveLineBg", &Settings::active_line_bg_color},
    {"ColorLyricsActiveWordFg", &Settings::active_word_color}, {"ColorLyricsActiveWordBg", &Settings::active_word_bg_color},
    {"ColorHeader", &Settings::header_color},               {"ColorLegend", &Settings::legend_color}, {"ColorLegendCommand", &Settings::legend_cmd_color},
    {"ColorTabCurrent", &Settings::tab_current_color},      {"ColorTabOther", &Settings::tab_other_color},
    {"ColorChangeFg", &Settings::change_text_color},        {"ColorChangeBg", &Settings::change_bg_color},
    {"ColorFocus", &Settings::focus_color},
};

themes::Theme theme_capture(const Settings& s, themes::Theme base) {
    base.builtin = false;
    for (const ThemeKey& k : kThemeKeys) base.set(k.key, s.*(k.p));
    return base;
}

bool theme_matches(const Settings& s, const themes::Theme& t) {
    int known = 0;
    for (const ThemeKey& k : kThemeKeys) {
        const std::string* v = t.get(k.key, false);
        if (!v) continue;
        ++known;
        if (*v != s.*(k.p)) return false;
    }
    return known > 0;
}
} // namespace

// (declared in app_internal.h: app.cpp applies a theme too)
void theme_apply(Settings& s, const themes::Theme& t) {
    for (const ThemeKey& k : kThemeKeys) if (const std::string* v = t.get(k.key, false)) s.*(k.p) = *v;
    // a theme written before the focus indicator had a color of its own: it gets the theme's header color
    if (!t.get("ColorFocus", false) && t.get("ColorHeader", false)) s.focus_color = s.header_color;
}

// The theme the audio book mode starts with, until a theme is picked there.
void apply_book_default_theme(Settings& s) {
    const std::vector<themes::Theme> all = themes::load_all();
    const int i = themes::find_name(all, "Rose Pine");
    if (i >= 0) theme_apply(s, all[static_cast<size_t>(i)]);
}

int App::theme_current_index() const {
    // a theme of your own wins over a built-in one with exactly the same colours (a renamed copy keeps its new name)
    for (size_t i = 0; i < theme_list_.size(); ++i) if (!theme_list_[i].builtin && theme_matches(settings_, theme_list_[i])) return static_cast<int>(i);
    for (size_t i = 0; i < theme_list_.size(); ++i) if (theme_matches(settings_, theme_list_[i])) return static_cast<int>(i);
    return -1;
}

void App::theme_open_picker() {
    theme_list_ = themes::load_all();
    theme_sel_ = std::max(0, theme_current_index());
    theme_ui_ = 1;
    force_redraw_ = true;
}

// edit_current (key [e]): the creator opens on the colours in use. If they are a theme of your own, its name is already in the
// NAME field and saving overwrites it (after a question); a built-in theme (or colours that match no theme) needs a new name.
void App::theme_open_creator(bool edit_current) {
    theme_list_ = themes::load_all();
    const int cur = theme_current_index();
    theme_base_ = theme_list_[static_cast<size_t>(cur >= 0 ? cur : 0)];
    theme_draft_ = settings_;
    theme_draft_text_ = settings_to_text(theme_draft_);
    leave_prompt_ = 0;
    theme_name_.clear();
    status_line_.clear();
    if (edit_current) {
        if (cur >= 0 && !theme_list_[static_cast<size_t>(cur)].builtin) theme_name_ = theme_list_[static_cast<size_t>(cur)].name;
        else status_line_ = cur >= 0 ? "\"" + theme_list_[static_cast<size_t>(cur)].name + "\" is built in and cannot be overwritten: give your edit a new name"
                                     : "these colors are not a saved theme: give them a name";
    }
    theme_name_initial_ = theme_name_;
    theme_row_ = -1;
    theme_col_ = 0;
    theme_ui_ = 2;
    force_redraw_ = true;
}

// [s] in the creator: a name is needed, a built-in theme cannot be overwritten, and overwriting a theme of your own is asked first
// (leave_prompt_ 3; [y] then calls theme_save_new()).
void App::theme_try_save() {
    const std::string name = themes::sanitize_name(theme_name_);
    if (name.empty()) { status_line_ = "give the theme a name first (NAME row), then [s]"; force_redraw_ = true; return; }
    const std::vector<themes::Theme> all = themes::load_all();
    const int at = themes::find_name(all, name);
    if (at >= 0) {
        const themes::Theme& t = all[static_cast<size_t>(at)];
        if (t.builtin) { status_line_ = "\"" + t.name + "\" is a built-in theme and cannot be overwritten -- pick another name"; force_redraw_ = true; return; }
        leave_prompt_ = 3;
        force_redraw_ = true;
        return;
    }
    theme_save_new();
}

void App::theme_delete_selected() {
    if (theme_sel_ < 0 || theme_sel_ >= static_cast<int>(theme_list_.size())) return;
    const std::string name = theme_list_[static_cast<size_t>(theme_sel_)].name;
    std::string err;
    if (!themes::delete_theme(name, err)) { status_line_ = err; return; }
    theme_list_ = themes::load_all();
    theme_sel_ = std::clamp(theme_sel_, 0, std::max(0, static_cast<int>(theme_list_.size()) - 1));
    status_line_ = "THEME \"" + name + "\" DELETED";
}

void App::theme_save_new() {
    themes::Theme t = theme_capture(theme_draft_, theme_base_);
    t.name = themes::sanitize_name(theme_name_);
    std::string err;
    if (!themes::save_theme(t, err)) { status_line_ = err; return; }
    theme_list_ = themes::load_all();
    theme_apply(settings_, t);
    theme_ui_ = 0;
    status_line_ = "THEME \"" + t.name + "\" SAVED AND APPLIED (s saves the settings)";
    force_redraw_ = true;
}

static std::filesystem::path theme_dir_path(std::string dir) {
    if (!dir.empty() && dir[0] == '~' && (dir.size() == 1 || dir[1] == '/' || dir[1] == '\\')) {
        const char* home = std::getenv("HOME");
        if (!home || !*home) home = std::getenv("USERPROFILE");
        if (home) dir = std::string(home) + dir.substr(1);
    }
    return path_from_utf8(dir);
}

void App::theme_open_export() {
    theme_list_ = themes::load_all();
    const int cur = theme_current_index();
    theme_io_name_ = cur >= 0 ? theme_list_[static_cast<size_t>(cur)].name : std::string("My Theme");
    if (theme_io_dir_.empty()) theme_io_dir_ = themes::exchange_dir().string();
    theme_io_row_ = 0;
    theme_ui_ = 4;
    force_redraw_ = true;
}

void App::theme_io_rescan() {
    theme_io_files_ = themes::list_theme_files(theme_dir_path(theme_io_dir_));
    theme_io_sel_ = std::clamp(theme_io_sel_, 0, std::max(0, static_cast<int>(theme_io_files_.size()) - 1));
}

void App::theme_open_import() {
    theme_list_ = themes::load_all();
    if (theme_io_dir_.empty()) theme_io_dir_ = themes::exchange_dir().string();
    theme_io_sel_ = 0;
    theme_io_rescan();
    theme_io_row_ = theme_io_files_.empty() ? 0 : 1;
    theme_ui_ = 5;
    force_redraw_ = true;
}

void App::theme_import_files(const std::vector<std::filesystem::path>& files) {
    int n = 0;
    std::string last, err;
    for (const auto& f : files)
        for (themes::Theme& t : themes::read_theme_file(f)) {
            const std::string name = themes::import_theme(t, err);
            if (!name.empty()) { ++n; last = name; }
        }
    theme_list_ = themes::load_all();
    theme_ui_ = 0;
    status_line_ = n == 0 ? (err.empty() ? std::string("no theme found in that file") : err)
                          : "IMPORTED " + std::to_string(n) + " THEME" + (n == 1 ? "" : "S") + (n == 1 ? ": " + last : std::string()) + " -- pick it in the THEME row";
    force_redraw_ = true;
}

void App::handle_theme_key(int key) {
    const int n = static_cast<int>(theme_list_.size());
    if (theme_ui_ == 4) { // export the current colours as a theme file
        if (key == 27 || key == 'q' || key == 'Q') { theme_ui_ = 0; status_line_.clear(); force_redraw_ = true; return; }
        if (key == 'A') { theme_io_row_ = 0; return; }
        if (key == 'B') { theme_io_row_ = 1; return; }
        if (key == '\r' || key == '\n') {
            color_edit_buffer_ = theme_io_row_ == 0 ? theme_io_name_ : theme_io_dir_;
            edit_caret_ = edit_anchor_ = color_edit_buffer_.size();
            edit_owner_ = "settings";
            mode_ = Mode::ColorEdit;
            return;
        }
        if (key == 's' || key == 'S' || key == kKeyCtrlS) {
            const std::string name = themes::sanitize_name(theme_io_name_);
            if (name.empty()) { status_line_ = "give the theme a name first"; return; }
            const int cur = theme_current_index();
            themes::Theme t = theme_capture(settings_, theme_list_[static_cast<size_t>(cur >= 0 ? cur : 0)]);
            t.name = name;
            const std::filesystem::path file = theme_dir_path(theme_io_dir_) / themes::file_name_for(name);
            std::string err;
            if (!themes::write_theme_file(file, t, err)) { status_line_ = err; return; }
            theme_ui_ = 0;
            status_line_ = "EXPORTED " + file.string();
            force_redraw_ = true;
        }
        return;
    }
    if (theme_ui_ == 5) { // import theme files from a folder
        const int nf = static_cast<int>(theme_io_files_.size());
        if (key == 27 || key == 'q' || key == 'Q') { theme_ui_ = 0; status_line_.clear(); force_redraw_ = true; return; }
        if (key == 'A') { if (theme_io_row_ == 1 && theme_io_sel_ > 0) --theme_io_sel_; else theme_io_row_ = 0; return; }
        if (key == 'B') {
            if (theme_io_row_ == 0) { if (nf > 0) theme_io_row_ = 1; }
            else if (theme_io_sel_ < nf - 1) ++theme_io_sel_;
            return;
        }
        if (key == '\r' || key == '\n') {
            if (theme_io_row_ == 0) {
                color_edit_buffer_ = theme_io_dir_;
                edit_caret_ = edit_anchor_ = color_edit_buffer_.size();
                edit_owner_ = "settings";
                mode_ = Mode::ColorEdit;
            } else if (theme_io_sel_ >= 0 && theme_io_sel_ < nf) {
                theme_import_files({theme_io_files_[static_cast<size_t>(theme_io_sel_)]});
            }
            return;
        }
        if (key == 'a' || key == kKeyCtrlS) { if (nf > 0) theme_import_files(theme_io_files_); return; }
        return;
    }
    if (theme_ui_ == 1 || theme_ui_ == 3) {
        if (theme_peek_) { theme_peek_ = false; peek_frame_.clear(); force_redraw_ = true; return; }   // any key closes the preview
        if (key == 'P' && !last_key_was_arrow() && theme_sel_ >= 0 && theme_sel_ < n) { theme_peek_ = true; peek_frame_.clear(); force_redraw_ = true; return; }
        if (key == 27 || key == 'q' || key == 'Q') { theme_ui_ = (theme_ui_ == 3) ? 2 : 0; return; }
        if ((key == 'A' || key == 'B') && last_key_was_arrow() && last_key_was_shifted()) {   // SHIFT+up / down: a page
            const int page = std::max(1, theme_page_ - 1);
            theme_sel_ = std::clamp(theme_sel_ + (key == 'A' ? -page : page), 0, std::max(0, n - 1));
            return;
        }
        if (key == 'A') { if (theme_sel_ > 0) --theme_sel_; return; }
        if (key == 'B') { if (theme_sel_ < n - 1) ++theme_sel_; return; }
        if (key == kKeyHome) { theme_sel_ = 0; return; }
        if (key == kKeyEnd) { theme_sel_ = std::max(0, n - 1); return; }
        if (theme_ui_ == 1 && key == 'D' && !last_key_was_arrow() && theme_sel_ >= 0 && theme_sel_ < n) {   // SHIFT+d: delete a theme of your own
            const themes::Theme& t = theme_list_[static_cast<size_t>(theme_sel_)];
            if (t.builtin) status_line_ = "\"" + t.name + "\" is a built-in theme and cannot be deleted";
            else leave_prompt_ = 4;   // "Do you want to delete theme?" ([y] calls theme_delete_selected())
            force_redraw_ = true;
            return;
        }
        if ((key == '\r' || key == '\n') && theme_sel_ >= 0 && theme_sel_ < n) {
            const themes::Theme& t = theme_list_[static_cast<size_t>(theme_sel_)];
            if (theme_ui_ == 1) {
                theme_apply(settings_, t);
                theme_ui_ = 0;
                status_line_ = "THEME: " + t.name + " (s saves the settings)";
            } else {
                theme_apply(theme_draft_, t);
                theme_base_ = t;
                theme_ui_ = 2;
                status_line_ = "template: " + t.name;
            }
            force_redraw_ = true;
        }
        return;
    }
    // --- the CREATE THEME overlay ---
    if (key == 27 || key == 'q' || key == 'Q') {
        // anything made here is lost on ESC: ask first (the theme is only kept by [s])
        if (theme_name_ != theme_name_initial_ || settings_to_text(theme_draft_) != theme_draft_text_) { leave_prompt_ = 2; force_redraw_ = true; return; }
        theme_ui_ = 0; status_line_.clear(); force_redraw_ = true; return;
    }
    if (key == 's' || key == 'S' || key == kKeyCtrlS) { theme_try_save(); return; }
    if (key == 'A') {
        if (theme_row_ >= 0) { --theme_row_; if (theme_row_ < 0) theme_col_ = 0; }
        return;
    }
    if (key == 'B') {
        if (theme_row_ < 20) { ++theme_row_; if (theme_col_ == 1 && !color_cell(theme_draft_, theme_row_, 1)) theme_col_ = 0; }
        return;
    }
    if (key == 'C') {
        if (theme_row_ < 0) theme_col_ = 1;
        else if (color_cell(theme_draft_, theme_row_, 1)) theme_col_ = 1;
        return;
    }
    if (key == 'D') { theme_col_ = 0; return; }
    if (key == '\r' || key == '\n') {
        if (theme_row_ < 0 && theme_col_ == 1) { theme_ui_ = 3; theme_sel_ = 0;
            for (int i = 0; i < n; ++i) if (theme_list_[static_cast<size_t>(i)].name == theme_base_.name) theme_sel_ = i;
            return; }
        if (theme_row_ < 0) color_edit_buffer_ = theme_name_;
        else { std::string* c = color_cell(theme_draft_, theme_row_, theme_col_); color_edit_buffer_ = c ? *c : ""; }
        edit_caret_ = edit_anchor_ = color_edit_buffer_.size();
        edit_owner_ = "settings";
        mode_ = Mode::ColorEdit;
        return;
    }
}

// Tabs 1 and 2 are a list of category headers and rows that can be longer than the panel: the first line shown is
// chosen so that the selected row is in the middle (and its category header stays visible when it is right below it).
int App::settings_tab_first_line(int /*W*/, int visible) const {
    const auto& lay = settings_layout(settings_tab_);
    const int total = static_cast<int>(lay.size());
    const int cur = settings_item_line(settings_tab_, settings_row_);
    int first = std::clamp(cur - visible / 2, 0, std::max(0, total - visible));
    const int need = (cur > 0 && lay[static_cast<size_t>(cur - 1)].header) ? cur - 1 : cur;
    if (first > need) first = need;
    return std::max(0, first);
}

int App::settings_max_row() const {
    // Matches get_max_row(): SCHEMA.size() - 1 for each tab, extended for
    // the Reference tab's appended font-map rows and the About tab's
    // scrollable text (both computed dynamically, not hardcoded, so they
    // track the actual font_map/about_app_text() content).
    switch (settings_tab_) {
        case 0: return 22; // THEME row + 21 colour rows + the SMOOTHNESS row (the extras are SEARCH, SEARCH INACTIVE, HEADER, LEGEND, TAB NAMES, CHANGE and FOCUS)
        case 1: case 2: return settings_item_count(settings_tab_) - 1; // GENERAL / ANIMA/VIZ: the rows of the layout
        case 3: return path_row_count() - 1; // PATHS: every selectable path/"+ new path" row
        case 4: {
            int letters = 0;
            for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) ++letters;
            return kRefEnd + letters - 1; // hotkeys + N font-map rows
        }
        case 5: {
            int MAX_Y = std::max(term_rows_ - 2, 11);
            int visible = std::max(1, MAX_Y - 3);
            int total = static_cast<int>(about_app_text().size());
            return std::max(0, total - visible); // scroll range, not a field cursor
        }
        default: return 0;
    }
}

std::string App::settings_get_value(int row, int col) const {
    // Matches getVal(k): config value if set, else the hardcoded default
    // for the 7 keys that have one, else "___" (unset). Colors format
    // their own "___"-equivalent as "none" at render time instead.
    if (settings_tab_ == 0) {
        if (row == 2) return col == 0 ? (settings_.border_smooth ? "smooth" : "rough") : "";
        std::string* p = const_cast<App*>(this)->color_field_ptr(row, col);
        return p ? *p : "";
    }
    if (settings_tab_ == 1 || settings_tab_ == 2) {
        // The toggle rows read straight off their bool; the pick rows return the name of their choice.
        bool v = false;
        switch (settings_item_id(settings_tab_, row)) {
            case kSidDisk: return settings_.element_disk ? "Show Disk / Cover" : "Remove Field";
            case kSidDummy: v = settings_.element_dummy_buttons; break;
            case kSidQueue: v = settings_.element_queue; break;
            case kSidWave: v = settings_.element_waveform; break;
            case kSidVizField: v = settings_.element_vizfield; break;
            case kSidLyricsEngine: v = settings_.element_lyrics; break;
            // "Lyrics/Viz Default" isn't a bool: it cycles sphere / osci / spectro / lyrics (see settings_options_for() and settings_commit_edit()).
            case kSidLyricViz: return settings_.viz_default == 4 ? "last used" : settings_.viz_default == 5 ? "video" : viz_row_name(settings_.viz_default);
            case kSidVisualizer: v = settings_.element_visualizer; break;
            case kSidNoVideoViz: return settings_.novideo_viz == 0 ? "sphere" : settings_.novideo_viz == 1 ? "osci" : settings_.novideo_viz == 2 ? "spectro" : settings_.novideo_viz == 3 ? "none" : "lyrics";
            case kSidVideoSize: return settings_.video_size == 1 ? "cropped" : settings_.video_size == 2 ? "full" : settings_.video_size == 3 ? "framed" : "regular";
            case kSidVideoFx: return video_fx_name(settings_.video_fx);
            case kSidVideoContrast: return std::to_string(std::clamp(settings_.video_contrast, 0, 100)) + "%";
            case kSidVideoColors: return std::to_string(std::clamp(settings_.video_colors, 0, 100)) + "%";
            case kSidVideoChroma: return chroma_name(settings_.video_chroma);
            case kSidVideoRound: return settings_.video_round ? "on" : "off";
            case kSidVideoClap: return settings_.video_clap ? "on" : "off";
            case kSidCoverChroma: return chroma_name(settings_.cover_chroma);
            case kSidVideoRes: return video_res_label(settings_.video_res, gfx_proto_ == GfxProto::Sixel);
            case kSidVideoBuffer: return std::to_string(settings_.video_buffer) + " MB";
            case kSidVideoMp3: return settings_.video_mp3 ? "true" : "false";
            case kSidUseVideo: v = settings_.video_mode; break;
            case kSidNoLyricsViz: return settings_.nolyrics_viz == 1 ? "osci" : settings_.nolyrics_viz == 2 ? "spectro" : settings_.nolyrics_viz == 3 ? "none" : "sphere";
            case kSidStereo: v = settings_.stereo; break;
            case kSidBlackBg: v = settings_.black_background; break;
            case kSidShareProfiles: v = settings_.share_profiles; break;
            case kSidBookAsk: v = settings_.book_ask_bookmark; break;
            case kSidAlbumCover: v = settings_.album_cover; break;
            case kSidCoverMode: return settings_.cover_mode == 0 ? "Just Cover" : settings_.cover_mode == 1 ? "Cover with Gradient" : settings_.cover_mode == 4 ? "Cover behind Disk (Full Anima)" : settings_.cover_mode == 5 ? "Cover ASCII" : settings_.cover_mode == 6 ? "Cover ASCII with Gradient" : settings_.cover_mode == 3 ? "Cover behind Disk v2" : "Cover behind Disk";
            case kSidCoverContrast: return std::to_string(std::clamp(settings_.cover_contrast, 0, 100)) + "%";
            case kSidCoverBrightness: return std::to_string(std::clamp(settings_.cover_brightness, 0, 100)) + "%";
            case kSidCoverColors: return std::to_string(std::clamp(settings_.cover_colors, 0, 100)) + "%";
            case kSidCoverSearch: {
                static const char* names[] = {"Default", "Folder First", "Within File First", "Image in Folder Only", "Within File Only"};
                return names[std::clamp(settings_.cover_search, 0, 4)];
            }
            case kSidBookCover: v = settings_.book_cover; break;
            case kSidBookCoverWave: v = settings_.book_cover_wave; break;
            case kSidUpdateCheck: v = settings_.update_check; break;
            case kSidMediaKeys: v = settings_.media_keys; break;
            case kSidRateCap: v = settings_.rate_cap_96; break;
            case kSidFullScreen: v = settings_.full_screen; break;
            case kSidUseTermSettings: v = settings_.use_terminal_settings; break;
            case kSidTermSize: return col == 1 ? std::to_string(settings_.terminal_rows) : std::to_string(settings_.terminal_cols);   // "cols x rows": two fields
            case kSidLatinLookup: v = settings_.latin_lookup; break;
            case kSidLatinMacrons: v = settings_.latin_macrons; break;
            case kSidLatinWo: v = settings_.latin_wo; break;
            case kSidLatinHanzi: return settings_.latin_hanzi == 1 ? "Chin." : settings_.latin_hanzi == 2 ? "Jap." : "Leave";
            case kSidLyricsVariants: v = settings_.lyrics_name_variants; break;
            case kSidRemoteApp: return settings_.remote_app ? "on   [ENTER] settings" : "off  [ENTER] settings";   // opens the REMOTE APP overlay
            case kSidCheckUpdate: return update_check::check_busy() ? std::string("checking ...") : std::string("[ENTER] to check");
            case kSidBookSpeed: return speed_label(settings_.book_speed);
            case kSidAudioDevice: return audio_device_label() + "  [ENTER] choose";
            case kSidNormalize: v = settings_.normalize; break;
            case kSidMetaOnly: v = settings_.meta_only; break;
            case kSidEmoji: return settings_.emoji_mode == 1 ? "Safe" : settings_.emoji_mode == 2 ? "All" : "Off";
            case kSidMarquee: v = settings_.marquee; break;
            case kSidTermDefaultFont: v = settings_.terminal_default_font; break;
            case kSidUseLyrics: v = settings_.use_lyrics; break;
            case kSidUseOsci: v = settings_.use_osci; break;
            case kSidUseSpectro: v = settings_.use_spectro; break;
            case kSidUseSphere: v = settings_.use_sphere; break;
            case kSidOrder1: case kSidOrder2: case kSidOrder3: case kSidOrder4: case kSidOrder5:   // which view comes at that place of the "." cycle
                return viz_row_name(settings_.viz_order[settings_item_id(settings_tab_, row) - kSidOrder1]);
            case kSidUseModePlayer: v = settings_.use_mode[0]; break;
            case kSidUseModeRadio: v = settings_.use_mode[1]; break;
            case kSidUseModeBook: v = settings_.use_mode[2]; break;
            case kSidModeOrder1: case kSidModeOrder2: case kSidModeOrder3:   // which mode comes at that place of the SHIFT and + cycle
                return mode_row_name(settings_.mode_order[settings_item_id(settings_tab_, row) - kSidModeOrder1]);
            case kSidPlayback: {
                static const char* names[] = {"list", "loop", "shuffle", "stop", "queue then stop"};
                return names[std::clamp(settings_.play_mode, 0, 4)];
            }
            case kSidStartMode: return mode_row_name(std::max(0, mode_index(settings_.start_mode)));
            case kSidVolIndicator: return kVolIndicatorNames[std::clamp(settings_.volume_indicator, 0, kVolStyleCount - 1)];
            case kSidVisFluidity: return std::to_string(settings_.bar_fluidity());
            case kSidBarBehavior: return bars_preset_name(bars_preset_of(settings_.bar_fluidity(), settings_.bar_degradation(), settings_.bar_viscosity()));
            case kSidBarStyle: return settings_.bars_style == 1 ? "hi-fi" : "classic";
            case kSidHifiRes: return settings_.hifi_resolution == 1 ? "rough" : "fine";
            case kSidHifiChan: return settings_.hifi_channels == 1 ? "mono" : "stereo";
            case kSidHifiColor: return settings_.hifi_color == 1 ? "corresponding" : "gradient";
            case kSidHifiScale: return settings_.hifi_scale == 0 ? "full" : settings_.hifi_scale == 2 ? "low" : settings_.hifi_scale == 3 ? "very low" : "medium";
            case kSidHifiAdapt: return settings_.hifi_adapt == 1 ? "on" : "off";
            case kSidHifiSplit: return settings_.hifi_split == 0 ? "remove" : "add";
            case kSidHifiFScale: { static const char* n[] = {"linear", "logarithmic", "mel", "bark", "erb"}; return n[std::clamp(settings_.hifi_fscale, 0, 4)]; }
            case kSidHifiGain: return std::to_string(settings_.hifi_gain);
            case kSidHifiRange: return std::to_string(settings_.hifi_range);
            case kSidWaveStyle: return settings_.waveform_smooth ? "smooth" : "raw";
            case kSidDiskSpeed: return std::to_string(settings_.disk_rotation_speed).substr(0, 4);
            case kSidTrackBuffer: return std::to_string(settings_.track_buffer_mb);
            case kSidVisDegr: return std::to_string(settings_.bar_degradation());
            case kSidVisVisc: return std::to_string(settings_.bar_viscosity());
            case kSidLyrAlign: return settings_.lyrics_alignment == 1 ? "left" : settings_.lyrics_alignment == 2 ? "right" : "center";
            case kSidLyrAnim: {
                static const char* names[] = {"full", "word by word", "letter by letter", "active line only", "active word only", "line by line"};
                return names[std::clamp(settings_.lyrics_animation, 0, 5)];
            }
            default: return "";
        }
        return v ? "true" : "false";
    }
    if (settings_tab_ == 4 && row == 0) return "[ENTER] reset";
    if (settings_tab_ == 4 && row >= kRefStart && row < kRefEnd) {
        auto it = settings_.hotkeys.find(kRefRows[row - kRefStart].action);
        return it != settings_.hotkeys.end() ? it->second : "";
    }
    if (settings_tab_ == 3) {
        // The path rows (LOCAL PATH / DOWNLOAD PATH / PLAYLIST PATH): the
        // string at their index in the owning vector, which is "" both for
        // a not-yet-set path and for a placeholder row shown while the
        // vector is still empty.
        PathRow r = path_row(row);
        if (r.sel < 0) return "";
        if (r.kind == PathRow::Kind::Path) {
            if (r.history_folder) return settings_.history_path.empty() ? history_.effective_dir() : settings_.history_path;
            if (r.export_folder) return settings_.playlist_export_path.empty() ? path_utf8(playlists_dir() / "export") : settings_.playlist_export_path;
            if (r.import_folder) return settings_.playlist_import_path.empty() ? path_utf8(playlists_dir() / "import") : settings_.playlist_import_path;
            if (r.conversion_folder) return conversion_export_dir();
            if (r.audiobook_folder) return settings_.audiobook_path;
            if (r.download_folder) {
                // Show what is actually in effect: while nothing has been
                // configured that is the default cache folder, and the point
                // of this row is that you can see -- and change -- where
                // downloads land, not that you face a blank field.
                return settings_.download_folder.empty() ? path_utf8(cache_.cache_dir())
                                                         : settings_.download_folder;
            }
            const std::vector<std::string>& paths = r.playlist_path ? settings_.playlists_paths
                                                                    : settings_.local_music_paths;
            if (r.path_index >= 0 && r.path_index < static_cast<int>(paths.size()))
                return paths[r.path_index];
            if (r.playlist_path) return path_utf8(playlists_dir());   // nothing configured: show the folder in effect
            return "";
        }
        return ""; // AddPath: the row IS the button, it has no value
    }
    return "";
}

// Matches g_options: the fixed value lists that Left/Right cycles
// through. Empty return = not cyclable (Colors and Reference rows,
// exactly like the reference's g_options map has no entries for those).
std::vector<std::string> App::settings_options_for(int tab, int row) const {
    if (tab == 1 || tab == 2) {
        // Bool toggles cycle true / false; the pick rows have lists of their own.
        switch (settings_item_id(tab, row)) {
            case -1: return {};
            case kSidDisk: return {"Show Disk / Cover", "Remove Field"};   // (true / false)
            case kSidLyricViz: return {"sphere", "osci", "spectro", viz_row_name(3), "video", "last used"};   // the view the field starts with, not a bool
            case kSidNoLyricsViz: return {"sphere", "osci", "spectro", "none"};
            case kSidNoVideoViz: return {"lyrics", "sphere", "osci", "spectro", "none"};
            case kSidVideoSize: return {"regular", "cropped", "full", "framed"};
            case kSidVideoFx: { std::vector<std::string> o; for (int i = 0; i < kVideoFxShown; ++i) o.push_back(video_fx_name(kVideoFxOrder[i])); return o; }
            case kSidVideoContrast: case kSidVideoColors: { std::vector<std::string> o; for (int i = 0; i <= 100; i += 5) o.push_back(std::to_string(i) + "%"); return o; }
            case kSidVideoChroma: case kSidCoverChroma: return {"off", "subtle", "medium", "strong"};
            case kSidVideoRound: case kSidVideoClap: return {"off", "on"};
            case kSidVideoRes: return {video_res_label(0, gfx_proto_ == GfxProto::Sixel), video_res_label(1, gfx_proto_ == GfxProto::Sixel), video_res_label(2, gfx_proto_ == GfxProto::Sixel)};
            case kSidVideoMp3: return {"false", "true"};
            case kSidVideoBuffer: { std::vector<std::string> o; for (int c : kVideoBufferChoices) o.push_back(std::to_string(c) + " MB"); return o; }
            case kSidOrder1: case kSidOrder2: case kSidOrder3: case kSidOrder4: case kSidOrder5: return {"sphere", "osci", "spectro", viz_row_name(3), "video"};
            case kSidModeOrder1: case kSidModeOrder2: case kSidModeOrder3: return {mode_row_name(0), mode_row_name(1), mode_row_name(2)};
            case kSidPlayback: return {"list", "loop", "shuffle", "stop", "queue then stop"};
            case kSidStartMode: {   // only modes that are switched on can start
                std::vector<std::string> o;
                for (int i = 0; i < 3; ++i) if (settings_.use_mode[settings_.mode_order[i]]) o.push_back(mode_row_name(settings_.mode_order[i]));
                return o;
            }
            case kSidVolIndicator: return {kVolIndicatorNames[0], kVolIndicatorNames[1], kVolIndicatorNames[2], kVolIndicatorNames[3], kVolIndicatorNames[4]};
            case kSidCoverMode: return {"Just Cover", "Cover with Gradient", "Cover behind Disk", "Cover behind Disk v2", "Cover behind Disk (Full Anima)", "Cover ASCII", "Cover ASCII with Gradient"};
            case kSidCoverBrightness: case kSidCoverColors:
            case kSidCoverContrast: { std::vector<std::string> o; for (int i = 0; i <= 100; i += 5) o.push_back(std::to_string(i) + "%"); return o; }
            case kSidCoverSearch: return {"Default", "Folder First", "Within File First", "Image in Folder Only", "Within File Only"};
            case kSidTrackBuffer: return {};   // a number (ENTER types it) and a "Set back to default" button (RIGHT, then ENTER)
            case kSidRemoteApp: return {};   // not a switch: ENTER / LEFT / RIGHT open the REMOTE APP overlay
            case kSidCheckUpdate: return {};   // an action: ENTER asks GitHub (handle_settings_key)
            case kSidAudioDevice: return {};   // not a switch: ENTER opens the CHOOSE AUDIO DEVICE overlay
            case kSidBookSpeed: {   // 0.5x .. 3.0x in steps of 0.05
                std::vector<std::string> o;
                for (int h = 50; h <= 300; h += 5) o.push_back(speed_label(h / 100.0));
                return o;
            }
            case kSidBarBehavior: return {"lively", "default", "smooth", "slow"};
            case kSidVisFluidity: case kSidVisDegr: case kSidVisVisc: return {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
            case kSidLatinHanzi: return {"Leave", "Chin.", "Jap."};
            case kSidEmoji: return {"Off", "Safe", "All"};
            case kSidTermSize: return {};   // two typed numbers (ENTER types the one that is lit)
            case kSidWaveStyle: return {"raw", "smooth"};
            case kSidBarStyle: return {"classic", "hi-fi"};
            case kSidHifiRes: return {"fine", "rough"};
            case kSidHifiChan: return {"stereo", "mono"};
            case kSidHifiColor: return {"gradient", "corresponding"};
            case kSidHifiScale: return {"full", "medium", "low", "very low"};
            case kSidHifiAdapt: return {"off", "on"};
            case kSidHifiSplit: return {"add", "remove"};
            case kSidHifiFScale: return {"linear", "logarithmic", "mel", "bark", "erb"};
            case kSidHifiGain: { std::vector<std::string> g; for (int i = -20; i <= 20; i += 2) g.push_back(std::to_string(i)); return g; }
            case kSidHifiRange: { std::vector<std::string> g; for (int i = 40; i <= 100; i += 4) g.push_back(std::to_string(i)); return g; }
            case kSidDiskSpeed: return {"0.01", "0.05", "0.10", "0.17", "0.25", "0.50", "0.75", "1.00"};
            case kSidLyrAlign: return {"left", "center", "right"};
            case kSidLyrAnim: return {"full", "word by word", "line by line", "letter by letter", "active line only", "active word only"};
            default: return {"true", "false"};
        }
    }
    return {};
}

// Matches g_config[k] = edit_buffer -- stored close to verbatim, no
// clamping. A bool/enum field that doesn't recognize the typed text
// just leaves the setting unchanged, since there's no way to store
// arbitrary text in a typed field the way the reference's string map can.
// Every change on the GENERAL and ANIMA/VIZ tabs goes through here: the value is stored, then the switches that depend on
// each other are brought in line (enforce_visual_switches()), and the lyrics are fetched when they have just come back.
void App::settings_commit_edit() {
    const bool rows_tab = settings_tab_ == 1 || settings_tab_ == 2;
    const int id = rows_tab ? settings_item_id(settings_tab_, settings_row_) : -1;
    const bool lyrics_before = settings_.lyrics_on();
    settings_commit_edit_impl();
    if (!rows_tab) return;
    settings_note_ = enforce_visual_switches(id);
    if (!lyrics_before && settings_.lyrics_on()) lyrics_view_nudge();   // lyrics switched on: fetch them for the track that plays now
}

void App::settings_commit_edit_impl() {
    const std::string& buf = color_edit_buffer_;
    auto to_lower = [](std::string v) { for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return v; };

    if (theme_ui_ == 4) { if (theme_io_row_ == 0) theme_io_name_ = themes::sanitize_name(buf); else theme_io_dir_ = buf; return; }
    if (theme_ui_ == 5) { theme_io_dir_ = buf; theme_io_rescan(); theme_io_sel_ = 0; theme_io_row_ = theme_io_files_.empty() ? 0 : 1; return; }
    if (theme_ui_ == 2) { // the CREATE THEME overlay edits its own draft
        if (theme_row_ < 0) theme_name_ = themes::sanitize_name(buf);
        else if (std::string* c = color_cell(theme_draft_, theme_row_, theme_col_)) *c = buf;
        return;
    }
    if (settings_tab_ == 0) {
        std::string* p = color_field_ptr(settings_row_, settings_col_);
        if (p) *p = buf;
    } else if (settings_tab_ == 1 || settings_tab_ == 2) {
        const int id = settings_item_id(settings_tab_, settings_row_);
        std::string v = to_lower(buf);
        // "Lyrics/Viz Default" and "When No Lyrics Show" are picks among views (sphere / osci / spectro, plus lyrics or none), not
        // bools: they have to be handled BEFORE the true/false gate below, or their values would bounce off it and the row
        // would never change.
        if (id == kSidVideoSize) { settings_.video_size = v == "cropped" ? 1 : v == "full" ? 2 : v == "framed" ? 3 : 0; video_pic_key_.clear(); return; }
        if (id == kSidVideoFx) { settings_.video_fx = video_fx_from(v); video_pic_key_.clear(); return; }
        if (id == kSidVideoContrast || id == kSidVideoColors) {
            std::string d = v;
            if (!d.empty() && d.back() == '%') d.pop_back();
            (id == kSidVideoContrast ? settings_.video_contrast : settings_.video_colors) = std::clamp((std::atoi(d.c_str()) + 2) / 5 * 5, 0, 100);
            video_pic_key_.clear();
            return;
        }
        if (id == kSidVideoClap) { settings_.video_clap = v == "on"; video_pic_key_.clear(); return; }
        if (id == kSidVideoRound) { settings_.video_round = v == "on"; video_pic_key_.clear(); return; }
        if (id == kSidVideoChroma) { settings_.video_chroma = chroma_from(v); video_pic_key_.clear(); return; }
        if (id == kSidCoverChroma) { settings_.cover_chroma = chroma_from(v); cover_pic_key_.clear(); return; }
        if (id == kSidVideoRes) { settings_.video_res = video_res_from(v); video_pic_key_.clear(); return; }
        if (id == kSidVideoMp3) { settings_.video_mp3 = v == "true"; return; }
        if (id == kSidVideoBuffer) { const int mb = std::atoi(v.c_str()); int best = kVideoBufferChoices[0]; for (int c : kVideoBufferChoices) if (mb >= c) best = c; settings_.video_buffer = best; return; }
        if (id == kSidNoVideoViz) {
            if (v == "lyrics") settings_.novideo_viz = 4;
            else if (v == "sphere") settings_.novideo_viz = 0;
            else if (v == "osci" || v == "oscilloscope") settings_.novideo_viz = 1;
            else if (v == "spectro" || v == "spectrogram") settings_.novideo_viz = 2;
            else if (v == "none" || v == "nothing") settings_.novideo_viz = 3;
            return;
        }
        if (id == kSidLyricViz || id == kSidNoLyricsViz) {
            int& pick = id == kSidLyricViz ? settings_.viz_default : settings_.nolyrics_viz;
            if (v == "sphere") pick = 0;
            else if (v == "osci" || v == "oscilloscope") pick = 1;
            else if (v == "spectro" || v == "spectrogram") pick = 2;
            else if (v == "none" && id == kSidNoLyricsViz) pick = 3;
            else if ((v == "lyrics" || v == "notes") && id == kSidLyricViz) pick = 3;
            else if (v == "video" && id == kSidLyricViz) pick = 5;       // the lyrics view with the video picture (needs the Video mode)
            else if (v == "last used" && id == kSidLyricViz) pick = 4;   // starts with the view it was left on
            if (id == kSidLyricViz && settings_.viz_default <= 3) {   // the default is also the view shown now ("last used" changes nothing)
                settings_.element_lyrics = settings_.viz_default == 3;
                if (settings_.viz_default < 3) settings_.lyric_viz = settings_.viz_default;
                settings_.video_view = false;
            } else if (id == kSidLyricViz && settings_.viz_default == 5) {
                settings_.element_lyrics = true;
                settings_.video_view = settings_.video_mode;
            }
            return; // anything unrecognized leaves the pick as it was
        }
        // "Viz Order 1..4": naming a view puts it at this place; the view that was there takes the place it left
        // (so the order stays a permutation of the four).
        if (id >= kSidOrder1 && id <= kSidOrder5) {
            int want = -1;
            for (int i = 0; i < 5; ++i) if (v == kVizNames[i]) want = i;
            if (v == "notes") want = 3;
            if (v == "oscilloscope") want = 1;
            if (v == "spectrogram") want = 2;
            if (want < 0) return;
            const int place = id - kSidOrder1;
            order_swap_from_ = -1;
            for (int i = 0; i < 5; ++i)
                if (settings_.viz_order[i] == want) {
                    if (i != place) order_swap_from_ = i;
                    std::swap(settings_.viz_order[i], settings_.viz_order[place]);
                    break;
                }
            return;
        }
        // "Mode Order 1..3": naming a mode puts it at this place; the mode that was there takes the place it left.
        if (id >= kSidModeOrder1 && id <= kSidModeOrder3) {
            const int want = mode_index(v);
            if (want < 0) return;
            const int place = id - kSidModeOrder1;
            for (int i = 0; i < 3; ++i)
                if (settings_.mode_order[i] == want) { std::swap(settings_.mode_order[i], settings_.mode_order[place]); break; }
            return;
        }
        switch (id) {   // the rows that are neither bools nor views
            case kSidPlayback:
                settings_.play_mode = (v == "loop") ? 1 : (v == "shuffle") ? 2 : (v == "stop") ? 3 : (v == "queue then stop") ? 4 : 0;
                queue_reset_lap();
                return;
            case kSidCoverMode:
                settings_.cover_mode = v == "just cover" ? 0 : v == "cover with gradient" ? 1 : v == "cover behind disk" ? 2 : v == "cover behind disk v2" ? 3 : v == "cover behind disk (full anima)" ? 4 : v == "cover ascii" ? 5 : v == "cover ascii with gradient" ? 6 : settings_.cover_mode;
                return;
            case kSidCoverContrast: {
                std::string d = v;
                if (!d.empty() && d.back() == '%') d.pop_back();
                settings_.cover_contrast = std::clamp((std::atoi(d.c_str()) + 2) / 5 * 5, 0, 100);
                return;
            }
            case kSidCoverBrightness: case kSidCoverColors: {   // behind the disk: brightness floor / the cover's own colours
                std::string d = v;
                if (!d.empty() && d.back() == '%') d.pop_back();
                (id == kSidCoverBrightness ? settings_.cover_brightness : settings_.cover_colors) = std::clamp((std::atoi(d.c_str()) + 2) / 5 * 5, 0, 100);
                return;
            }
            case kSidCoverSearch:
                settings_.cover_search = v == "default" ? 0 : v == "folder first" ? 1 : v == "within file first" ? 2
                                       : v == "image in folder only" ? 3 : v == "within file only" ? 4 : settings_.cover_search;
                return;
            case kSidTermSize: {   // typed "columns" / "rows" (the field that is lit); 0 = leave the terminal as it is
                const bool is_cols = settings_col_ == 0;
                const TerminalLimits L = terminal_size_limits();
                try {
                    std::string d = buf;
                    d.erase(std::remove_if(d.begin(), d.end(), [](unsigned char ch) { return std::isspace(ch) != 0; }), d.end());
                    const int typed = d.empty() ? 0 : std::stoi(d);
                    const int lo = is_cols ? L.min_cols : L.min_rows, hi = is_cols ? L.max_cols : L.max_rows;
                    const int use = typed <= 0 ? 0 : std::clamp(typed, lo, hi);
                    (is_cols ? settings_.terminal_cols : settings_.terminal_rows) = use;
                    status_line_ = std::string("terminal ") + (is_cols ? "columns" : "rows") + ": " + (use == 0 ? "0 -- the terminal is left as it is" : std::to_string(use));
                    if (use != 0 && use != typed) status_line_ += " (allowed: " + std::to_string(lo) + " - " + std::to_string(hi) + (L.screen_known ? ", what fits on the screen)" : ")");
                    if (settings_.terminal_cols > 0 && settings_.terminal_rows > 0 && !settings_.full_screen && !settings_.use_terminal_settings) {
                        terminal_apply_size(settings_.terminal_cols, settings_.terminal_rows, false);
                        status_line_ += " -- asked the terminal for " + std::to_string(settings_.terminal_cols) + " x " + std::to_string(settings_.terminal_rows) + " (it may refuse)";
                    }
                } catch (...) { status_line_ = "terminal size: type a number (0 = leave the terminal as it is)"; }
                return;
            }
            case kSidEmoji:   // Off / Safe (keep plain emoji, "?" for the risky ones) / All
                settings_.emoji_mode = v == "safe" ? 1 : v == "all" ? 2 : v == "off" ? 0 : settings_.emoji_mode;
                set_emoji_mode(settings_.emoji_mode);   // every width calculation follows from the next frame on
                force_redraw_ = true;
                break;
            case kSidLatinHanzi:   // Chinese characters that could be Japanese or Chinese (東京): leave / pinyin / kanji reading
                settings_.latin_hanzi = v == "chin." ? 1 : v == "jap." ? 2 : v == "leave" ? 0 : settings_.latin_hanzi;
                translit_set_default_hanzi(settings_.latin_hanzi);
                force_redraw_ = true;
                return;
            case kSidStartMode: { const int m = mode_index(v); if (m >= 0 && settings_.use_mode[m]) settings_.start_mode = kModeNames[m]; return; }
            case kSidBookSpeed: {   // "1.25x" or a typed "1.25"
                std::string d = v;
                if (!d.empty() && d.back() == 'x') d.pop_back();
                try { book_speed_set(std::stod(d), false); } catch (...) { status_line_ = "playback speed: type a number between 0.5 and 3.0"; }
                return;
            }
            case kSidVolIndicator:
                settings_.volume_indicator = std::max(0, volume_style_from_name(v));
                return;
            case kSidVisFluidity: try { settings_.bar_fluidity() = std::clamp(std::stoi(buf), 1, 10); } catch (...) {} return;
            case kSidBarBehavior: for (int i = 0; i < 4; ++i) if (v == kBarsPresets[i].name) {
                settings_.bar_fluidity() = kBarsPresets[i].fluidity; settings_.bar_degradation() = kBarsPresets[i].degradation; settings_.bar_viscosity() = kBarsPresets[i].viscosity; }
                return;
            case kSidWaveStyle: settings_.waveform_smooth = (v == "smooth"); return;
            case kSidBarStyle: settings_.bars_style = (v == "hi-fi") ? 1 : 0; g_settings_hifi_rows = settings_.bars_style == 1; return;
            case kSidHifiRes: settings_.hifi_resolution = (v == "rough") ? 1 : 0; return;
            case kSidHifiChan: settings_.hifi_channels = (v == "mono") ? 1 : 0; return;
            case kSidHifiColor: settings_.hifi_color = (v == "corresponding") ? 1 : 0; return;
            case kSidHifiScale: settings_.hifi_scale = v == "full" ? 0 : v == "low" ? 2 : v == "very low" ? 3 : 1; return;
            case kSidHifiAdapt: settings_.hifi_adapt = v == "on" ? 1 : 0; return;
            case kSidHifiSplit: settings_.hifi_split = v == "remove" ? 0 : 1; return;
            case kSidHifiFScale: settings_.hifi_fscale = v == "linear" ? 0 : v == "logarithmic" ? 1 : v == "bark" ? 3 : v == "erb" ? 4 : 2; return;
            case kSidHifiGain: try { settings_.hifi_gain = std::clamp(std::stoi(v), -20, 20); } catch (...) {} return;
            case kSidHifiRange: try { settings_.hifi_range = std::clamp(std::stoi(v), 40, 100); } catch (...) {} return;
            case kSidDiskSpeed: try { settings_.disk_rotation_speed = std::stod(buf); } catch (...) {} return;
            case kSidTrackBuffer: {   // typed MB; the next track that is loaded uses it
                try {
                    const int mb = std::stoi(buf);
                    settings_.track_buffer_mb = std::clamp(mb, Settings::kTrackBufferMinMB, Settings::kTrackBufferMaxMB);
                    status_line_ = "track buffer: " + std::to_string(settings_.track_buffer_mb) + " MB" +
                                   (mb != settings_.track_buffer_mb ? " (allowed: " + std::to_string(Settings::kTrackBufferMinMB) + " - " + std::to_string(Settings::kTrackBufferMaxMB) + ")" : "") +
                                   " -- applies to the next track that is loaded";
                } catch (...) { status_line_ = "track buffer: type a number of MB"; }
                return;
            }
            case kSidVisDegr: try { settings_.bar_degradation() = std::clamp(std::stoi(buf), 1, 10); } catch (...) {} return;
            case kSidVisVisc: try { settings_.bar_viscosity() = std::clamp(std::stoi(buf), 0, 10); } catch (...) {} return;
            case kSidDisk: if (v == "show disk / cover" || v == "true") settings_.element_disk = true; else if (v == "remove field" || v == "false") settings_.element_disk = false; return;
            case kSidLyrAlign: settings_.lyrics_alignment = (v == "left") ? 1 : (v == "right") ? 2 : 0; return;
            case kSidLyrAnim:
                if (v == "word by word") settings_.lyrics_animation = 1;
                else if (v == "letter by letter") settings_.lyrics_animation = 2;
                else if (v == "active line only") settings_.lyrics_animation = 3;
                else if (v == "active word only") settings_.lyrics_animation = 4;
                else if (v == "line by line") settings_.lyrics_animation = 5;
                else settings_.lyrics_animation = 0;
                return;
            default: break;
        }
        bool is_true = (v == "true"), is_false = (v == "false");
        if (!is_true && !is_false) return;
        switch (id) {
            case kSidDisk: settings_.element_disk = is_true; break;
            case kSidAlbumCover: settings_.album_cover = is_true; break;
            case kSidDummy: settings_.element_dummy_buttons = is_true; break;
            case kSidQueue: settings_.element_queue = is_true; break;
            case kSidWave: settings_.element_waveform = is_true; break;
            case kSidVizField: settings_.element_vizfield = is_true; break;
            case kSidLyricsEngine: settings_.element_lyrics = is_true; break;
            case kSidVisualizer: settings_.element_visualizer = is_true; break;
            case kSidStereo:
                settings_.stereo = is_true;
                player_.set_stereo(is_true); // audible immediately for a stereo-decoded track
                break;
            case kSidNormalize:
                settings_.normalize = is_true;
                player_.set_normalization(settings_.normalize,
                                          static_cast<float>(settings_.normalize_target_lufs),
                                          static_cast<float>(settings_.normalize_max_boost_db));
                break;
            case kSidMetaOnly: settings_.meta_only = is_true; break;
            case kSidMarquee: settings_.marquee = is_true; break;
            case kSidTermDefaultFont:   // applied and saved at once (one value for all modes), like the audio device
                settings_.terminal_default_font = is_true;
                set_config_value("TerminalDefaultFont", is_true ? "true" : "false");
                settings_snapshot_.terminal_default_font = is_true;
                settings_snapshot_text_ = settings_to_text(settings_snapshot_);
                terminal_font::apply(is_true ? std::string(terminal_font::kTerminalDefault) : settings_.terminal_font);
                break;
            case kSidBlackBg: settings_.black_background = is_true; break;   // the render loop applies it
            case kSidShareProfiles: settings_.share_profiles = is_true; break;   // the radio reads it when it starts
            case kSidBookAsk: settings_.book_ask_bookmark = is_true; break;
            case kSidBookCover: settings_.book_cover = is_true; break;
            case kSidBookCoverWave: settings_.book_cover_wave = is_true; break;
            case kSidUpdateCheck: settings_.update_check = is_true; break;   // read at the next start
            case kSidRateCap: settings_.rate_cap_96 = is_true; break;        // the next track that is loaded uses it
            case kSidMediaKeys: settings_.media_keys = is_true; break;       // read at the next start
            case kSidFullScreen: settings_.full_screen = is_true; if (!settings_.use_terminal_settings) terminal_apply_size(settings_.terminal_cols, settings_.terminal_rows, is_true); force_redraw_ = true; break;
            case kSidUseTermSettings:   // true: the terminal is left alone (full screen is given back); false: Terminal Size / Full Screen apply again
                settings_.use_terminal_settings = is_true;
                if (is_true) terminal_apply_size(0, 0, false);
                else terminal_apply_size(settings_.terminal_cols, settings_.terminal_rows, settings_.full_screen);
                force_redraw_ = true;
                break;
            case kSidLatinLookup: settings_.latin_lookup = is_true; name_lookup_broken_ = false; break;
            case kSidLatinMacrons: settings_.latin_macrons = is_true; translit_set_style(settings_.latin_macrons, settings_.latin_wo); force_redraw_ = true; break;
            case kSidLyricsVariants: settings_.lyrics_name_variants = is_true; set_lyrics_name_variants(is_true); break;
            case kSidLatinWo: settings_.latin_wo = is_true; translit_set_style(settings_.latin_macrons, settings_.latin_wo); force_redraw_ = true; break;
            case kSidRemoteApp:
                settings_.remote_app = is_true;
                if (is_true && settings_.remote_pin.empty()) settings_.remote_pin = remote::net::make_pin();
                remote_app_apply();   // starts / stops the web server right away
                break;
            case kSidUseVideo: if (settings_.video_mode != is_true) video_toggle(); break;   // the same as the hotkey (lists the videos, rescans)
            case kSidUseLyrics: settings_.use_lyrics = is_true; break;
            case kSidUseOsci: settings_.use_osci = is_true; break;
            case kSidUseSpectro: settings_.use_spectro = is_true; break;
            case kSidUseSphere: settings_.use_sphere = is_true; break;
            case kSidUseModePlayer: case kSidUseModeRadio: case kSidUseModeBook: {
                const int m = id - kSidUseModePlayer;
                if (!is_true && !settings_.use_mode[(m + 1) % 3] && !settings_.use_mode[(m + 2) % 3]) {   // the last one stays on
                    status_line_ = "one mode always stays on";
                    return;
                }
                settings_.use_mode[m] = is_true;
                settings_.mode_normalize();   // a start mode that went off moves to the first one that is on
                break;
            }
            default: break;
        }
    } else if (settings_tab_ == 4 && settings_row_ >= kRefStart && settings_row_ < kRefEnd) {
        settings_.hotkeys[kRefRows[settings_row_ - kRefStart].action] = buf;
    } else if (settings_tab_ == 3) {
        PathRow r = path_row(settings_row_);
        if (r.kind == PathRow::Kind::Path && r.sel >= 0) {
            // A path row commits a string instead of a bool. Trim the stray
            // spaces a careless Enter can leave behind (a trailing one would
            // otherwise point at a directory that doesn't exist) and expand
            // a leading ~ exactly the way load_settings() does, so
            // "~/Music" means the same thing whether it was typed into this
            // field or written into config.txt by hand. Committing an empty
            // value deletes the line rather than storing a blank
            // LocalMusicPath=/PlaylistsPath= -- which is also what keeps
            // the placeholder row a still-empty list shows harmless.
            std::string p = buf;
            while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
            while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
            if (!p.empty() && p[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) p = std::string(home) + p.substr(1);
            }
            if (r.export_folder) {
                // One folder, nothing moves: only where the next export goes (empty = export, inside the playlist folder).
                settings_.playlist_export_path = p;
                status_line_ = p.empty() ? "playlist export folder: export, inside the playlist folder" : "playlist export folder: " + p;
                return;
            }
            if (r.import_folder) {
                // One folder, nothing moves: only where the next import looks (empty = import, inside the playlist folder).
                settings_.playlist_import_path = p;
                status_line_ = p.empty() ? "playlist import folder: import, inside the playlist folder" : "playlist import folder: " + p;
                return;
            }
            if (r.audiobook_folder) {
                // One folder: the audio book library. Nothing moves; the audio book mode scans it (when it is the running mode,
                // right away, otherwise the next time it opens).
                if (book_path_conflicts(settings_.local_music_paths, p)) {
                    status_line_ = "CONFLICT: that folder is also a LOCAL MUSIC PATH. Pick a sub folder of it (the music player then leaves it out) or another folder";
                    return;
                }
                settings_.audiobook_path = p;
                status_line_ = p.empty() ? "audio book folder removed" : "audio book folder: " + p;
                rescan_library();   // the music player's library leaves the book folder out, the book player lists it
                return;
            }
            if (r.conversion_folder) {
                // One folder, nothing moves: only where the next conversion writes (empty = conversion-export in the download folder).
                settings_.conversion_export_path = p;
                status_line_ = "conversion export folder: " + conversion_export_dir();
                return;
            }
            if (r.history_folder) {
                // One folder: the history moves with it (an existing history.json there is read, otherwise the current one is written there).
                history_end_current_play();
                history_.save();
                settings_.history_path = p;
                history_.set_dir(p);
                if (history_.file_exists()) history_.load(); else history_.save();
                status_line_ = p.empty() ? "history folder: default" : "history folder: " + p;
                return;
            }
            if (r.download_folder) {
                // One folder, never a list: store it (an emptied field goes
                // back to the default cache folder), point yt-dlp at it right
                // away, and -- when one is set -- add it to the local paths so
                // the rescan below picks it up. That injection is the whole
                // "no extra LocalMusicPath line needed" half of this setting.
                if (same_folder_path(p, settings_.audiobook_path)) {
                    status_line_ = "CONFLICT: that folder is the AUDIO BOOK PATH (the download folder is a music folder)";
                    return;
                }
                settings_.download_folder = p;
                const fs::path old_archive = cache_.download_archive_path();
                cache_.set_download_dir(path_from_utf8(p));
                {   // the shared yt-dlp archive (history/downloaded_history.txt) is COPIED along, never moved; one that
                    // already exists in the new folder is left alone
                    std::error_code aec;
                    const fs::path new_archive = cache_.download_archive_path();
                    if (old_archive != new_archive && fs::exists(old_archive, aec) && !fs::exists(new_archive, aec)) {
                        fs::create_directories(new_archive.parent_path(), aec);
                        fs::copy_file(old_archive, new_archive, aec);
                    }
                }
                if (p.empty()) {
                    status_line_ = "download folder: default (cache dir)";
                } else {
                    if (std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), p)
                        == settings_.local_music_paths.end())
                        settings_.local_music_paths.push_back(p);
                    status_line_ = "download folder: " + p;
                }
                rescan_library(); // a path row that does not rescan is a path row you have to restart for
                return;
            }
            if (!r.playlist_path && same_folder_path(p, settings_.audiobook_path)) {
                status_line_ = "CONFLICT: that folder is the AUDIO BOOK PATH. A local music path may not be the audio book folder (a folder around it is fine)";
                return;
            }
            std::vector<std::string>& paths = r.playlist_path ? settings_.playlists_paths
                                                              : settings_.local_music_paths;
            // A changed PLAYLIST path takes its playlists along: the .txt files of the folder it replaces are COPIED to the
            // new one (only ones missing there, nothing is overwritten or deleted). Music and download folders are never copied.
            fs::path copy_from;
            if (r.playlist_path && !p.empty()) {
                if (r.path_index < static_cast<int>(paths.size()) && !paths[r.path_index].empty()) copy_from = path_from_utf8(paths[r.path_index]);
                else if (paths.empty() || r.path_index == 0) copy_from = playlists_dir();   // the default folder it replaces
            }
            if (static_cast<int>(paths.size()) <= r.path_index) paths.resize(r.path_index + 1);
            if (p.empty()) {
                paths.erase(paths.begin() + r.path_index);
                status_line_ = r.playlist_path ? "playlist path removed" : "local path removed";
            } else {
                paths[r.path_index] = p;
                status_line_ = std::string(r.playlist_path ? "playlist path: " : "local path: ") + p;
                if (!copy_from.empty()) {
                    const fs::path to = path_from_utf8(p);
                    std::error_code ec;
                    int copied = 0;
                    if (fs::is_directory(copy_from, ec) && copy_from != to) {
                        fs::create_directories(to, ec);
                        for (const auto& e : fs::directory_iterator(copy_from, ec)) {
                            if (!e.is_regular_file(ec) || ascii_lower_str(path_utf8(e.path().extension())) != ".txt") continue;
                            const fs::path dst = to / e.path().filename();
                            if (fs::exists(dst, ec)) continue;
                            std::error_code cec;
                            fs::copy_file(e.path(), dst, cec);
                            if (!cec) ++copied;
                        }
                    }
                    if (copied > 0) status_line_ += "  (" + std::to_string(copied) + " playlist" + (copied == 1 ? "" : "s") + " copied from the old folder)";
                }
            }
            if (!r.playlist_path) rescan_library(); // the whole point: paths apply without a restart
            settings_row_ = std::min(settings_row_, settings_max_row()); // the list may have shrunk by one
            return;
        }
    }
}

// Matches cycle_option(): find the current value's index in its options
// list and step by `dir`, wrapping. No-op if this row has no options
// list at all.
void App::settings_cycle(int dir) {
    auto opts = settings_options_for(settings_tab_, settings_row_);
    if (opts.empty()) return;
    const int id = (settings_tab_ == 1 || settings_tab_ == 2) ? settings_item_id(settings_tab_, settings_row_) : -1;   // the row, as its SetId
    std::string cur = settings_get_value(settings_row_, 0);
    int idx = -1;
    for (size_t i = 0; i < opts.size(); ++i) if (opts[i] == cur) { idx = static_cast<int>(i); break; }
    idx = (idx == -1) ? 0 : (idx + dir + static_cast<int>(opts.size())) % static_cast<int>(opts.size());
    color_edit_buffer_ = opts[idx];
    edit_caret_ = edit_anchor_ = color_edit_buffer_.size(); // the buffer was replaced, not edited
    settings_commit_edit();
    status_line_ = "TOGGLED -> " + opts[idx];
    if (id == kSidWaveStyle) recompute_waveform_for_current_track();
    const std::string note = settings_note_.empty() ? std::string() : "  (" + settings_note_ + ")";
    if (id == kSidLyricViz) {
        status_line_ = settings_.viz_default == 4
            ? "starts with: the view it was left on" + note
            : settings_.viz_default == 5
            ? std::string("starts with: video") + (settings_.video_mode ? std::string() : std::string(" (switch the Video mode on with ") + hotkey_text("HKeyVideo", "&") + ", until then it shows the lyrics)") + note
            : std::string("starts with: ") + kVizNames[std::clamp(settings_.viz_default, 0, 3)] + " (the \".\" key changes the view while Mousiki runs)" + note;
    }
    if (id == kSidNoLyricsViz) {
        static const char* const names[4] = {"sphere", "oscilloscope", "spectrogram", "nothing"};
        status_line_ = std::string("no lyrics: ") + names[std::clamp(settings_.nolyrics_viz, 0, 3)] + " is shown" + note;
    }
    if (id == kSidVideoSize) status_line_ = std::string("video size: ") + (settings_.video_size == 1 ? "cropped -- two columns are cut off each side of the picture" : settings_.video_size == 2 ? "full -- the picture fills the field, cropped to fit, the lyric line as a small box over it" : settings_.video_size == 3 ? "framed -- like full, one row and two columns smaller on every side" : "regular -- the whole picture") + note;
    if (id == kSidVideoContrast) status_line_ = "video contrast: " + std::to_string(settings_.video_contrast) + "% -- more contrast for the video's picture (0 = as it is)" + note;
    if (id == kSidVideoColors) status_line_ = "video colors: " + std::to_string(settings_.video_colors) + "% -- 0 = greyscale, 100 = the video's own colours" + note;
    if (id == kSidVideoClap) status_line_ = settings_.video_clap ? "show clap loading: on -- a small clapperboard shows while a video loads" : "show clap loading: off -- the text \"video: starting ...\" shows while a video loads";
    if (id == kSidVideoRound) status_line_ = settings_.video_round ? "video rounded corners: on -- the corners of the picture are cut round like the corners of the UI" : "video rounded corners: off";
    if (id == kSidVideoChroma) status_line_ = std::string("video chromatic aberration: ") + chroma_name(settings_.video_chroma) + " -- red and blue are pushed apart, more towards the corners; works with every effect" + note;
    if (id == kSidCoverChroma) status_line_ = std::string("cover chromatic aberration: ") + chroma_name(settings_.cover_chroma) + " -- for a cover that shows its own colours (Just Cover, Cover Colors above 0, the ASCII looks)" + note;
    if (id == kSidVideoRes) status_line_ = std::string("video resolution: ") + video_res_label(settings_.video_res, gfx_proto_ == GfxProto::Sixel) + (settings_.video_res == 0 ? " -- the soft look, light on the terminal" : gfx_proto_ == GfxProto::Kitty ? " -- a sharper picture in true colour; needs more of the terminal and the processor" : " -- a wider picture (Sixel has at most 256 colours)") + note;
    if (id == kSidVideoBuffer) status_line_ = "video buffer: " + std::to_string(settings_.video_buffer) + " MB of decoded frames kept ahead (about " + std::to_string(video_buffer_seconds(settings_.video_buffer, settings_.video_res, gfx_proto_ == GfxProto::Sixel)) + " s of a 16:9 picture at this resolution)" + note;
    if (id == kSidVideoMp3) status_line_ = settings_.video_mp3 ? "yt-dlp mp3 from mp4: true -- SEARCH YT also saves the sound of a video as an mp3 song when no song is the target (CTRL+o in SEARCH YT changes it for one download)" : "yt-dlp mp3 from mp4: false -- SEARCH YT downloads only the video (CTRL+o in SEARCH YT asks for the mp3 for one download; CTRL+3 in FETCH VIDEOS -> VIDEO FILES makes one from a file)";
    if (id == kSidVideoFx) status_line_ = std::string("video effect: ") + video_fx_name(settings_.video_fx) + note;
    if (id == kSidNoVideoViz) {
        static const char* const names[5] = {"the sphere", "the oscilloscope", "the spectrogram", "nothing", "the lyrics"};
        status_line_ = std::string("no video: the video view shows ") + names[std::clamp(settings_.novideo_viz, 0, 4)] + note;
    }
    if (id == kSidUseVideo) {
        status_line_ = settings_.video_mode ? "video: on -- video files are listed, the video view is in the \".\" cycle" + note
                                            : "video: off -- no video files in the lists, no video view" + note;
    }
    if (id == kSidVizField) {
        status_line_ = settings_.element_vizfield ? "viz/lyrics field: on" + note
                                                  : "viz/lyrics field: off -- the meta data take its place, no lyrics are fetched for it";
    }
    if (id == kSidUseLyrics) {
        status_line_ = settings_.use_lyrics ? "lyrics engine: on (lyrics are fetched when the field shows them)" + note
                                            : "lyrics engine: off, no lyrics are fetched" + note;
    }
    if (id == kSidStereo) {
        // A track that was decoded as mono can't become stereo without being
        // decoded again, so switching stereo ON only applies from the next
        // track. Switching it OFF is immediate.
        if (settings_.stereo && has_track_ && current_pcm_ && current_pcm_->channels == 1)
            status_line_ = "stereo: on -- applies from the next track";
        else
            status_line_ = settings_.stereo ? "stereo: on" : "stereo: off";
    }
    if (id == kSidNormalize) {
        status_line_ = settings_.normalize ? "normalize: on" : "normalize: off";
    }
    if (id == kSidMetaOnly) {
        status_line_ = settings_.meta_only ? "lists: metadata only (no filename)"
                                           : "lists: filename + metadata";
        resort_local_view_keep_selection();
    }
    if (id == kSidTermDefaultFont)
        status_line_ = settings_.terminal_default_font ? "terminal default font: on -- Mousiki leaves the terminal's own font alone"
                                                       : "terminal default font: off -- Mousiki's window uses " + settings_.terminal_font + " (Windows Terminal: the profile \"Mousiki\")";
    if (id == kSidMarquee) {
        status_line_ = settings_.marquee ? "marquee: long titles scroll" : "marquee: off, long titles are cut with ...";
    }
    if (id == kSidEmoji) {
        status_line_ = settings_.emoji_mode == 2 ? "emoji: all drawn as a single ? (layout stays aligned)"
                     : settings_.emoji_mode == 1 ? "emoji: plain ones are kept, joined / flag / skin-tone / keycap ones drawn as a single ?"
                                                 : "emoji: drawn as they are (alignment depends on your terminal)";
    }
    if (id >= kSidUseLyrics && id <= kSidUseSphere) {
        static const char* const names[4] = {"lyrics", "oscilloscope", "spectrogram", "sphere"};
        const int r = id - kSidUseLyrics;
        const bool on = id == kSidUseLyrics ? settings_.use_lyrics : id == kSidUseOsci ? settings_.use_osci : id == kSidUseSpectro ? settings_.use_spectro : settings_.use_sphere;
        status_line_ = std::string(names[r]) + (on ? ": in the \".\" cycle" : ": switched off (not in the \".\" cycle; full screen and the windows " +
                              hotkey_text("HKeySpectroWindow", "(") + " / " + hotkey_text("HKeyScopeWindow", ")") + " still work)") + note;
    }
    if (id >= kSidOrder1 && id <= kSidOrder5) {
        std::string o = "\".\" cycle: ";
        for (int i = 0; i < 5; ++i) o += std::string(i ? " > " : "") + viz_row_name(settings_.viz_order[i]);
        // every view is in the cycle once: the one that held the place now holds the place the chosen view left
        if (order_swap_from_ >= 0) o += "   (Viz Order " + std::to_string(order_swap_from_ + 1) + " took " + viz_row_name(settings_.viz_order[order_swap_from_]) + " in return)";
        status_line_ = o;
    }
    if (id == kSidPlayback) status_line_ = std::string("playback: ") + play_mode_name(settings_.play_mode);
    if (id == kSidBlackBg) status_line_ = settings_.black_background ? "black background: on -- the terminal is black while Mousiki runs (given back on exit)"
                                                                     : "black background: off -- the terminal keeps its own background";
    if (id == kSidShareProfiles) status_line_ = settings_.share_profiles ? "modes share settings: on -- the radio uses these oscilloscope / spectrogram settings (its own come back when this is off)"
                                                                         : "modes share settings: off -- the radio keeps its own oscilloscope / spectrogram settings";
    if (id == kSidBookCover) status_line_ = settings_.book_cover ? "book cover: on"
                                                                 : "book cover: off -- the standard picture";
    if (id == kSidBookCoverWave) status_line_ = settings_.book_cover_wave ? "book gradient wave: true -- colour wave over the cover"
                                                                          : "book gradient wave: false -- the cover in its own colours";
    if ((id == kSidBookCover || id == kSidBookCoverWave) && settings_.book_cover) status_line_ += " | " + book_cover_report();
    if (id == kSidBookAsk) status_line_ = settings_.book_ask_bookmark ? "ask for bookmark: on -- leaving a chapter, the mode or the program asks first (the audio pauses meanwhile)"
                                                                      : "ask for bookmark: off -- only SHIFT+b sets a bookmark";
    if (id == kSidUpdateCheck) status_line_ = settings_.update_check ? "update notice: on -- at start-up Mousiki looks once a day for a newer release and tells you once (nothing is installed)"
                                                                     : "update notice: off -- no check for a newer release";
    if (id == kSidCheckUpdate) status_line_ = "check update: [ENTER] asks GitHub for the newest release now (needs Python 3 and a network connection)";
    if (id == kSidBookSpeed) status_line_ = "playback speed: " + speed_label(settings_.book_speed) + " -- the voice keeps its pitch; SHIFT+LEFT / SHIFT+RIGHT change it while you listen";
    if (id == kSidAudioDevice) status_line_ = "choose audio device: " + audio_device_label() + " -- [ENTER] lists the devices (also % on the main screen)";
    if (id == kSidLatinLookup) status_line_ = settings_.latin_lookup ? "latin name lookup: on -- artists written in Chinese characters are looked up on MusicBrainz in the background (their name is sent to musicbrainz.org, one at a time)"
                                                                    : "latin name lookup: off -- nothing is sent anywhere";
    if (id == kSidLatinMacrons) status_line_ = settings_.latin_macrons ? "latin macrons: on -- Japanese long vowels as macrons (koohii -> kōhī, ou -> ō)" : "latin macrons: off -- Japanese long vowels spelled out (ou, uu, koohii)";
    if (id == kSidLyricsVariants) status_line_ = settings_.lyrics_name_variants ? "lyrics name variants: on -- a search that finds nothing also tries the Latin spelling, the title / artist without their Latin or Japanese part, and the title alone" : "lyrics name variants: off -- only the title and artist as they are";
    if (id == kSidLatinHanzi) status_line_ = settings_.latin_hanzi == 1 ? "translit. kanji: Chin. -- Chinese characters that could be Japanese or Chinese (東京) are read as Chinese (pinyin)"
                                           : settings_.latin_hanzi == 2 ? "translit. kanji: Jap. -- such characters are read as Japanese (they stay as they are unless the optional kanji reader knows their reading)"
                                           : "translit. kanji: Leave -- such characters are left as they are (Japanese kana and Korean are still converted)";
    if (id == kSidLatinWo) status_line_ = settings_.latin_wo ? "latin wo: on -- the particle を is written \"wo\"" : "latin wo: off -- the particle を is written \"o\"";
    if (id == kSidUseTermSettings) status_line_ = settings_.use_terminal_settings ? "use terminal settings: true -- Mousiki leaves the terminal as it is; Terminal Size and Full Screen are ignored (they keep their values)"
                                                                                   : "use terminal settings: false -- the terminal gets the Terminal Size / Full Screen below";
    if (id == kSidFullScreen) status_line_ = settings_.full_screen ? "full screen: on -- the terminal is asked for full screen (now and at every start); Terminal Size is ignored meanwhile (the terminal may refuse)"
                                                                 : "full screen: off -- the terminal gets the Terminal Size";
    if (id == kSidRateCap) status_line_ = settings_.rate_cap_96 ? "rate cap at 96 kHz: on -- a track above 96 kHz is brought down to 96 kHz (the next track that is loaded)"
                                                                 : "rate cap at 96 kHz: off -- a track plays at its own rate: more RAM, and the audio device has to accept it (the next track that is loaded)";
    if (id == kSidMediaKeys) status_line_ = settings_.media_keys ? "media keys: on -- Mousiki shows in the system's now-playing and answers its media keys (from the next start)"
                                                                 : "media keys: off -- Mousiki stays out of the system's media controls (from the next start)";
    if (id == kSidRemoteApp) {
        const std::string err = remote::net::last_error();
        status_line_ = !settings_.remote_app ? "remote app: off"
                     : !err.empty() ? "remote app: " + err
                     : "remote app: " + remote::net::url() + "  PIN " + settings_.remote_pin + "  (Mousiki Remote app or the phone's browser, same network; s saves)";
    }
    if (id == kSidStartMode) status_line_ = settings_.start_mode == "radio" ? "Mousiki starts in the radio (SHIFT and + / SHIFT and - switch modes)"
                                       : settings_.start_mode == "audiobook" ? "Mousiki starts with the audio books (SHIFT and + / SHIFT and - switch modes)"
                                       : "Mousiki starts with the music player (SHIFT and + / SHIFT and - switch modes)";
    if (id >= kSidUseModePlayer && id <= kSidUseModeBook) {
        const int m = id - kSidUseModePlayer;
        status_line_ = std::string(mode_row_name(m)) + (settings_.use_mode[m] ? ": in the SHIFT and + cycle" : ": left out of the SHIFT and + cycle") + " (one mode always stays on)";
    }
    if (id >= kSidModeOrder1 && id <= kSidModeOrder3) {
        std::string o = "SHIFT and + cycle: ";
        for (int i = 0; i < 3; ++i) o += std::string(i ? " > " : "") + mode_row_name(settings_.mode_order[i]);
        status_line_ = o + "  (SHIFT and - goes back)";
    }
}

// ---- REFERENCE tab: reset to default + undo (the radio settings work the same way) ----
void App::ref_undo_push(int row) {
    ref_undo_.push_back({settings_.hotkeys, row});
    if (ref_undo_.size() > 5) ref_undo_.erase(ref_undo_.begin());
}

void App::ref_reset_keys(int row) {
    Settings defaults;
    apply_default_hotkeys(defaults);
    ref_undo_push(row);
    const auto before = settings_.hotkeys;
    for (int i = 0; i < kRefRowCount; ++i) {
        if (row != 0 && kRefStart + i != row) continue;
        auto it = defaults.hotkeys.find(kRefRows[i].action);
        if (it != defaults.hotkeys.end()) settings_.hotkeys[kRefRows[i].action] = it->second;
    }
    if (settings_.hotkeys == before) {
        ref_undo_.pop_back();
        status_line_ = row == 0 ? "all keys already have their default" : "this key already has its default";
        return;
    }
    status_line_ = row == 0 ? "ALL KEYS RESET TO DEFAULT (Ctrl+Shift+U undoes it)"
                            : std::string("default key restored for ") + kRefRows[row - kRefStart].label;
}

void App::ref_undo_pop() {
    if (ref_undo_.empty()) { status_line_ = "nothing to undo"; return; }
    const RefUndo u = ref_undo_.back();
    ref_undo_.pop_back();
    int where = u.row;
    if (where == 0) {   // a full reset: go to the first key that comes back
        for (int i = 0; i < kRefRowCount; ++i) {
            auto a = u.hotkeys.find(kRefRows[i].action);
            auto b = settings_.hotkeys.find(kRefRows[i].action);
            const std::string va = a == u.hotkeys.end() ? "" : a->second, vb = b == settings_.hotkeys.end() ? "" : b->second;
            if (va != vb) { where = kRefStart + i; break; }
        }
    }
    settings_.hotkeys = u.hotkeys;
    settings_tab_ = 4;
    settings_col_ = 0;
    settings_row_ = std::clamp(where, 0, kRefEnd - 1);
    status_line_ = u.row == 0 ? "UNDONE: reset of all keys"
                              : std::string("UNDONE: ") + kRefRows[u.row - kRefStart].label + " is now \"" + settings_get_value(u.row, 0) + "\"";
    force_redraw_ = true;
}

void App::handle_settings_key(int key) {
    g_settings_hifi_rows = settings_.bars_style == 1;   // (the Hi-Fi rows of SPECTRUM BARS are listed only with that style)
    if (leave_prompt_ != 0 && mode_ != Mode::ColorEdit) {
        // "unsaved changes" question: [s] / ENTER save, [d] discard, anything else (ESC) goes back to editing
        const int kind = leave_prompt_;
        leave_prompt_ = 0;
        force_redraw_ = true;
        const bool save = key == 's' || key == 'S' || key == '\r' || key == '\n' || key == kKeyCtrlS;
        const bool discard = key == 'd' || key == 'D';
        if (kind == 3) { if (key == 'y' || key == 'Y') theme_save_new(); return; }
        if (kind == 4) { if (key == 'y' || key == 'Y') theme_delete_selected(); return; }   // "delete the theme?": only y says yes   // "overwrite the theme?": only y says yes
        if (kind == 1) { if (save) settings_close(true); else if (discard) settings_close(false); }
        else if (save) theme_try_save();   // may ask once more (overwriting a theme of your own)
        else if (discard) { theme_ui_ = 0; status_line_.clear(); }
        return;
    }
    if (theme_ui_ != 0 && mode_ != Mode::ColorEdit) { handle_theme_key(key); return; }
    if (remote_ov_.open) {   // the REMOTE APP overlay has the keys
        remote_ui::Values v{settings_.remote_app, settings_.remote_port, settings_.remote_pin};
        bool close = false;
        if (remote_ui::key(remote_ov_, v, key, last_key_was_arrow(), close)) {
            settings_.remote_app = v.enabled; settings_.remote_port = v.port; settings_.remote_pin = v.pin;
            remote_app_apply();
            settings_update_dirty();
        }
        if (close) { remote_ov_ = remote_ui::State{}; status_line_ = settings_.remote_app ? "remote app on (s saves)" : "remote app off (s saves)"; }
        force_redraw_ = true;
        return;
    }
    if (mode_ == Mode::Settings && settings_tab_ == 1 && settings_item_id(1, settings_row_) == kSidAudioDevice &&
        (key == '\r' || key == '\n')) {
        audio_device_open(true);   // drawn over this screen; ESC comes back here
        return;
    }
    if (mode_ == Mode::Settings && settings_tab_ == 1 && settings_item_id(1, settings_row_) == kSidCheckUpdate &&
        (key == '\r' || key == '\n')) {
        status_line_ = update_check::check_now(locate_script("check_update.py")) ? "checking for a newer release ..."
                                                                                  : "a check is already running";
        force_redraw_ = true;
        return;
    }
    if (mode_ == Mode::Settings && settings_tab_ == 1 && settings_item_id(1, settings_row_) == kSidRemoteApp &&
        (key == '\r' || key == '\n' || ((key == 'C' || key == 'D') && last_key_was_arrow()))) {
        remote_ov_ = remote_ui::State{};
        remote_ov_.open = true;
        force_redraw_ = true;
        return;
    }
    if (mode_ == Mode::ColorEdit) {
        // The caret can be stale by one keypress after the buffer was
        // replaced wholesale (an option cycle, a rejected hotkey) -- clamp
        // before anything reads it, exactly like the meta editor does.
        le_clamp(color_edit_buffer_, edit_caret_, edit_anchor_);
        if (key == 27) { mode_ = Mode::Settings; return; } // cancel, discard buffer
        // A modifier combination (ALT+L, CTRL+L, CTRL+SHIFT+U / Z) cannot be typed as a character: pressing it in a
        // key field of the REFERENCE tab writes its name into the field.
        if (settings_tab_ == 4 && settings_row_ >= kRefStart && settings_row_ < kRefEnd) {
            const std::string cn = combo_key_name(key);
            if (!cn.empty()) { color_edit_buffer_ = cn; edit_caret_ = edit_anchor_ = cn.size(); return; }
        }
        if (key == '\r' || key == '\n') {
            std::string key_name = (settings_tab_ == 4 && settings_row_ >= kRefStart && settings_row_ < kRefEnd)
                                  ? kRefRows[settings_row_ - kRefStart].action : "";
            // Hotkey overlap fix: if this is a Reference-tab hotkey being
            // rebound and the typed key is already owned by a different
            // action, reject the commit instead of silently creating a
            // collision where two actions fire on the same key. Clear the
            // buffer, force a redraw, and stay in ColorEdit mode so the
            // user can just repeat entry with a different key -- same
            // flow as a normal edit, just not accepted yet.
            if (!key_name.empty()) {
                // What was typed has to be something the key reader can produce: one character, or one of the names
                // (hotkey_string_to_key()). Empty = unbound. "*" and ESC stay reserved: the first switches to the
                // radio mode and the second is the "back to the home view" key, both caught before any binding.
                color_edit_buffer_ = normalize_hotkey_string(color_edit_buffer_);
                edit_caret_ = edit_anchor_ = color_edit_buffer_.size();
                const std::string reject =
                    (!color_edit_buffer_.empty() && hotkey_string_to_key(color_edit_buffer_) == 0)
                        ? std::string("NOT A KEY -- one character, or ENTER TAB SPACE BACKSPACE ARROW_KEY_UP/DOWN/LEFT/RIGHT; "
                                      "press the combination itself for ") + pretty_key("ALT+L") + " / CTRL+l / CTRL+SHIFT+u / z"
                    : color_edit_buffer_ == "ESC"
                        ? std::string("\"ESC\" IS RESERVED (back to the home view) -- try another key")
                        : std::string();
                if (!reject.empty()) {
                    status_line_ = reject;
                    color_edit_buffer_.clear();
                    edit_caret_ = edit_anchor_ = 0;
                    force_redraw_ = true;
                    return;
                }
                std::string conflict = hotkey_conflict(color_edit_buffer_, key_name);
                if (!conflict.empty()) {
                    std::string who = conflict;   // shown by its label on this tab ("Read and Translate Overlay"), the action name otherwise
                    for (int i = 0; i < kRefRowCount; ++i) if (conflict == kRefRows[i].action) who = kRefRows[i].label;
                    status_line_ = "KEY \"" + pretty_key(color_edit_buffer_) + "\" ALREADY USED BY \"" + who + "\" -- try another key";
                    color_edit_buffer_.clear();
                    edit_caret_ = edit_anchor_ = 0;
                    force_redraw_ = true;
                    return; // stay in ColorEdit: redraw and repeat
                }
            }
            // Path commits write their own status (which path changed, that
            // the library was rescanned) -- don't bury it under the generic
            // "UPDATED".
            bool wrote_own_status = (settings_tab_ == 3 && path_row_is_text(settings_row_)) ||
                                    (settings_tab_ == 1 && settings_item_id(1, settings_row_) == kSidTermSize);
            if (!key_name.empty()) ref_undo_push(settings_row_);
            const auto hk_before = settings_.hotkeys;
            settings_commit_edit();
            if (!key_name.empty() && settings_.hotkeys == hk_before && !ref_undo_.empty()) ref_undo_.pop_back();   // nothing changed
            if (!wrote_own_status) {
                std::string what = key_name;   // the row's label, not the action name
                for (int i = 0; i < kRefRowCount; ++i) if (key_name == kRefRows[i].action) what = kRefRows[i].label;
                status_line_ = key_name.empty() ? "UPDATED" : ("UPDATED " + what);
            }
            mode_ = Mode::Settings;
            return;
        }
        // --- caret, selection, clipboard ---------------------------------
        // Shift+arrows mark, Ctrl+C/X/V copy/cut/paste, Home/End jump. These
        // are modifier combinations that arrive as their own sentinel values
        // (terminal_ui.h) precisely because the bare ones are already taken:
        // an arrow press collapses to the letter it would otherwise type, and
        // a typed 'c' has to stay a typed 'c'.
        if (le_undo_key(&color_edit_buffer_, color_edit_buffer_, edit_caret_, edit_anchor_, key, &status_line_)) return;   // CTRL+z / CTRL+y
        LeUndoScope undo_scope(&color_edit_buffer_, color_edit_buffer_, edit_caret_, key);
        if (key == kKeyHome) { edit_caret_ = edit_anchor_ = 0; return; }
        if (key == kKeyEnd) { edit_caret_ = edit_anchor_ = color_edit_buffer_.size(); return; }
        if (key == kKeyShiftLeft) { le_move(color_edit_buffer_, edit_caret_, edit_anchor_, -1, true); return; }
        if (key == kKeyShiftRight) { le_move(color_edit_buffer_, edit_caret_, edit_anchor_, +1, true); return; }
        if (key == kKeyDelete) { le_delete_forward(color_edit_buffer_, edit_caret_, edit_anchor_); return; }
        if (key == kKeyCtrlC) {
            size_t a = 0, b = 0;
            le_range(edit_caret_, edit_anchor_, a, b);
            std::string t = (a == b) ? color_edit_buffer_ : color_edit_buffer_.substr(a, b - a);
            clipboard_set(t);
            status_line_ = t.empty() ? "nothing selected" : "COPIED";
            return;
        }
        if (key == kKeyCtrlX) {
            size_t a = 0, b = 0;
            le_range(edit_caret_, edit_anchor_, a, b);
            if (a == b) return; // no selection: cut must never empty the field
            clipboard_set(color_edit_buffer_.substr(a, b - a));
            le_erase_selection(color_edit_buffer_, edit_caret_, edit_anchor_);
            status_line_ = "CUT";
            return;
        }
        if (key == kKeyCtrlV) {
            // A path gets far more room than a color/hotkey field does --
            // the same cap typing enforces below.
            const int limit = ((theme_ui_ == 2 && theme_row_ < 0) || (theme_ui_ == 4 && theme_io_row_ == 0)) ? themes::kMaxNameLen
                              : (theme_ui_ == 4 || theme_ui_ == 5) ? 240
                              : (settings_tab_ == 3 && path_row_is_text(settings_row_)) ? 240
                              : (settings_tab_ == 1 && settings_item_id(1, settings_row_) == kSidTrackBuffer) ? 6 : 18;
            le_paste(color_edit_buffer_, edit_caret_, edit_anchor_, clipboard_get(), static_cast<size_t>(limit));
            status_line_ = "PASTED";
            return;
        }
        if (key == 127 || key == 8) { le_backspace(color_edit_buffer_, edit_caret_, edit_anchor_); return; }
        // Arrow keys collapse to the letters 'A'-'D' (same collision as
        // Mode::Search below), which sit inside 32-126 and would otherwise
        // be typed as literal letters -- for a hotkey rebind that would
        // mean an arrow press could silently become the new binding. The
        // value on its own can't tell a real capital A from an Up press,
        // so ask the input layer instead: a genuinely typed A-D goes into
        // the buffer now (without it a Windows path could never be
        // entered -- "C:\" has no way past this check), while an arrow
        // press moves the caret instead (Left/Right) or is dropped (Up/Down
        // -- a single line has nowhere to go vertically).
        if (last_key_was_arrow() && (key == 'A' || key == 'B')) return;
        if (last_key_was_arrow() && (key == 'C' || key == 'D')) {
            le_move(color_edit_buffer_, edit_caret_, edit_anchor_, key == 'D' ? -1 : +1, false);
            return;
        }
        // A path needs far more room than a color/hotkey field does.
        const int edit_limit = ((theme_ui_ == 2 && theme_row_ < 0) || (theme_ui_ == 4 && theme_io_row_ == 0)) ? themes::kMaxNameLen
                             : (theme_ui_ == 4 || theme_ui_ == 5) ? 240
                             : (settings_tab_ == 3 && path_row_is_text(settings_row_)) ? 240
                             : (settings_tab_ == 1 && settings_item_id(1, settings_row_) == kSidTrackBuffer) ? 6 : 18;   // Track Buffer (MB): 6 digits at most
        if (is_text_key(key)) {
            le_insert(color_edit_buffer_, edit_caret_, edit_anchor_, static_cast<char>(key),
                      static_cast<size_t>(edit_limit));
        }
        return;
    }

    // SHIFT+up / down: a page, on every tab that scrolls
    if ((key == 'A' || key == 'B') && last_key_was_arrow() && last_key_was_shifted()) {
        const int visible = std::max(1, std::max(term_rows_ - 2, 11) - 3);
        const int page = std::max(1, visible - 1);
        settings_row_ = std::clamp(settings_row_ + (key == 'A' ? -page : page), 0, settings_max_row());
        if (settings_tab_ != 0) settings_col_ = 0;
        return;
    }
    if (key == kKeyCtrlShiftU) { ref_undo_pop(); return; }   // undo the last key change (up to 5, newest first)
    if (key == kKeyDelete && settings_tab_ == 4 && settings_row_ >= kRefStart && settings_row_ < kRefEnd) {
        ref_reset_keys(settings_row_);                           // the default key of this row
        return;
    }
    if (key == 9) { // Tab
        settings_tab_ = (settings_tab_ + 1) % kSettingsTabCount;
        settings_row_ = 0;
        settings_col_ = 0;
        return;
    }
    // Explicit spec from the user, overriding the reference's own
    // key semantics for this exact case (reference's 's' saves without
    // closing; here 's' saves AND exits, Esc/q just exits without saving).
    if (key == 's' || key == 'S') { settings_close(true); return; }
    if (key == 27 || key == 'q' || key == 'Q') {
        settings_update_dirty();
        if (settings_dirty_) { leave_prompt_ = 1; force_redraw_ = true; return; }   // unsaved changes: ask first
        settings_close(false);
        return;
    }
    if (settings_tab_ == 5) {
        // About App: no fields to edit, but Up/Down still scroll the text.
        if (key == 'A') { if (settings_row_ > 0) --settings_row_; return; }
        if (key == 'B') { if (settings_row_ < settings_max_row()) ++settings_row_; return; }
        return;
    }

    if (settings_tab_ == 0 && !last_key_was_arrow()) {   // [e] edit the theme in use, SHIFT+e / SHIFT+i: export / import a theme
        if (key == 'e') { theme_open_creator(true); return; }
        if (key == 'E') { theme_open_export(); return; }
        if (key == 'I') { theme_open_import(); return; }
    }
    if (key == '\r' || key == '\n') {
        // Reference tab: the hotkeys (kRefStart..kRefEnd-1) are editable;
        // the note above them and the font-map rows after them are
        // read-only display and never enter ColorEdit at all.
        if (settings_tab_ == 4 && settings_row_ == 0) { ref_reset_keys(0); return; }
        if (settings_tab_ == 4 && settings_row_ >= kRefEnd) {
            return;
        }
        // PATHS tab: every path row edits in place; "+ new path" adds a
        // line first.
        if (settings_tab_ == 3) {
            PathRow r = path_row(settings_row_);
            if (r.kind == PathRow::Kind::AddPath && r.sel >= 0) {
                // "+ new path": append an empty line to the list this row
                // belongs to and go straight into editing it. No row
                // bookkeeping needed -- build_path_rows() gives the new
                // entry exactly the selectable index this AddPath row just
                // vacated (every row after it shifts down by one), so
                // settings_row_ already points at the new empty field.
                std::vector<std::string>& paths = r.playlist_path ? settings_.playlists_paths
                                                                  : settings_.local_music_paths;
                paths.push_back("");
                color_edit_buffer_.clear();
                edit_caret_ = edit_anchor_ = 0;
                edit_owner_ = "settings"; // claim the shared caret -- see edit_focus()
                mode_ = Mode::ColorEdit;
                status_line_ = r.playlist_path
                    ? "new playlist path -- type it, ENTER to confirm"
                    : "new local music path -- type it, ENTER to confirm";
                force_redraw_ = true;
                return;
            }
        }
        if (settings_tab_ == 1 && settings_item_id(1, settings_row_) == kSidTermSize && settings_col_ == 2) {   // "Set back to default"
            settings_.terminal_cols = Settings::kTermColsDefault;
            settings_.terminal_rows = Settings::kTermRowsDefault;
            settings_col_ = 0;
            status_line_ = "terminal size: back to the default, " + std::to_string(Settings::kTermColsDefault) + " x " + std::to_string(Settings::kTermRowsDefault);
            if (!settings_.full_screen && !settings_.use_terminal_settings) {
                terminal_apply_size(settings_.terminal_cols, settings_.terminal_rows, false);
                status_line_ += " -- asked the terminal for it (it may refuse)";
            }
            return;
        }
        if (settings_tab_ == 1 && settings_item_id(1, settings_row_) == kSidTrackBuffer && settings_col_ == 1) {   // "Set back to default"
            settings_.track_buffer_mb = Settings::kTrackBufferDefaultMB;
            settings_col_ = 0;
            status_line_ = "track buffer: back to the default, " + std::to_string(Settings::kTrackBufferDefaultMB) + " MB -- applies to the next track that is loaded";
            return;
        }
        if ((settings_tab_ == 1 || settings_tab_ == 2) && settings_item_id(settings_tab_, settings_row_) == kSidVolIndicator) {   // ENTER changes the value that is lit
            if (settings_col_ == 1) {
                settings_.volume_gradient = !settings_.volume_gradient;
                status_line_ = settings_.volume_gradient ? "volume bar: gradient (the VIZ colours from left to right)" : "volume bar: no gradient (one colour)";
            } else {
                settings_cycle(1);
            }
            return;
        }
        if (settings_tab_ == 0 && settings_row_ == 2) { settings_.border_smooth = !settings_.border_smooth; return; }   // SMOOTHNESS
        if (settings_tab_ == 0 && settings_row_ == 0) { // THEME | CREATE THEME
            if (settings_col_ == 0) theme_open_picker(); else theme_open_creator();
            return;
        }
        color_edit_buffer_ = settings_get_value(settings_row_, settings_col_);
        edit_caret_ = edit_anchor_ = color_edit_buffer_.size(); // caret starts at the end, as usual
        edit_owner_ = "settings"; // claim the shared caret -- see edit_focus()
        mode_ = Mode::ColorEdit;
        return;
    }
    if (key == 'A') { if (settings_row_ > 0) --settings_row_; if (settings_tab_ != 0) settings_col_ = 0; return; }
    if (key == 'B') { if (settings_row_ < settings_max_row()) ++settings_row_; if (settings_tab_ != 0) settings_col_ = 0; return; }
    if ((settings_tab_ == 1 || settings_tab_ == 2) && settings_item_id(settings_tab_, settings_row_) == kSidVolIndicator && (key == 'C' || key == 'D')) {
        settings_col_ = key == 'C' ? 1 : 0;   // the look | gradient / no gradient
        status_line_ = "[ENTER] changes the value that is lit; [<-] [->] switch between the look and the gradient";
        return;
    }
    if (settings_tab_ == 1 && settings_item_id(1, settings_row_) == kSidTermSize && (key == 'C' || key == 'D')) {
        settings_col_ = std::clamp(settings_col_ + (key == 'C' ? 1 : -1), 0, 2);   // the columns | the rows | the "Set back to default" button
        status_line_ = "[ENTER] types the number that is lit (or sets the size back to the default); [<-] [->] switch between columns, rows and the button; 0 = leave the terminal as it is";
        return;
    }
    if (settings_tab_ == 1 && settings_item_id(1, settings_row_) == kSidTrackBuffer && (key == 'C' || key == 'D')) {
        settings_col_ = key == 'C' ? 1 : 0;   // the number | the "Set back to default" button
        return;
    }
    if (key == 'C') { // right
        if (settings_tab_ == 0) {
            if (settings_row_ == 2) settings_.border_smooth = !settings_.border_smooth;
            else if (color_field_ptr(settings_row_, 1) || settings_row_ == 0) settings_col_ = 1;
        }
        else settings_cycle(1);
        return;
    }
    if (key == 'D') { // left
        if (settings_tab_ == 0) {
            if (settings_row_ == 2) settings_.border_smooth = !settings_.border_smooth;
            else settings_col_ = 0;
        }
        else settings_cycle(-1);
        return;
    }
}

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
    auto repeat = [](const std::string& s, int n) {
        std::string r; r.reserve(s.size() * static_cast<size_t>(std::max(0, n)));
        for (int i = 0; i < n; ++i) r += s;
        return r;
    };
    // Matches pad(): no truncation if s is already >= width, just like
    // the reference -- a longer-than-expected value overflows into the
    // next column rather than getting cut off. Values here are always
    // short in practice (numbers, true/false, single-char hotkeys).
    auto pad = [](const std::string& s, int width, bool left_align = true) {
        int ulen = display_width(s);
        if (ulen >= width) return s;
        std::string spaces(width - ulen, ' ');
        return left_align ? (s + spaces) : (spaces + s);
    };

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
    if (settings_tab_ == 0) {
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
    } else if (settings_tab_ == 1 || settings_tab_ == 2) {
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
    } else if (settings_tab_ == 3) {
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
    } else if (settings_tab_ == 4) {
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
    } else if (settings_tab_ == 5) {
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
