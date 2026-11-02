// app_overlays.cpp
// Part of class App (src/app.h): The big list overlay and the big folder overlay.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "book_art.h"
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
#include "app_internal.h"
#include "update_check.h"
#include "open_url.h"
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
#include <unordered_map>
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

namespace muisc {


// The THEME overlays of the Settings screen (see theme_ui_ in app.h): the theme picker (also used to choose a
// ---------------------------------------------------------------------
// Console / log overlay (HKeyConsole)
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Bulk add overlay (paste-a-playlist-link panel, "a" while Queue focused)
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Bulk add overlay (paste-a-playlist-link panel, "a" while Queue focused)
// ---------------------------------------------------------------------
// Deliberately NOT sized like Console/Settings/Cheatsheet -- those match
// the player view or the terminal on purpose (real overlays meant to
// take over the screen). This one is a small floating panel that only
// takes as many rows as it actually has content for: an input row while
// typing, then a short starred checklist once results come in. It never
// grows to fill the terminal.

// ---------------------------------------------------------------------
// Big list overlay (HKeyListOverlay, SHIFT+l) -- see app.h for the design.
// ---------------------------------------------------------------------

namespace {
inline int list_total_for(ListSource src, size_t local, size_t online, size_t playlists, size_t folders) {
    return static_cast<int>(src == ListSource::Local ? local : src == ListSource::Online ? online
                          : src == ListSource::Folder ? folders : playlists);
}
} // namespace

// The overlay is only shown (and only steers scrolling) in the modes that
// have the main UI underneath it. In the full-screen takeovers (Settings,
// Playlist, ...) the flag simply stays set and the overlay is back when
// they close.
bool App::list_overlay_active() const {
    if (!list_overlay_open_) return false;
    if (mode_ == Mode::AudioDevice) return !adev_from_settings_;
    switch (mode_) {
        case Mode::Browse: case Mode::Search: case Mode::BulkAdd:
        case Mode::RetryLyrics: case Mode::ClearQueue: case Mode::UpdateNotice: case Mode::QueueToPlaylist: case Mode::OsciMenu: case Mode::SpectroMenu: case Mode::NormMenu: case Mode::Equalizer: case Mode::SleepTimer: case Mode::MenuIndex: case Mode::Creator: case Mode::LyricsEdit: case Mode::BookOverlay: return true;
        default: return false;
    }
}

// The key as it should read in a legend: what is actually bound (so a
// rebinding shows up), with the SHIFT+ spelling used in the README for an
// uppercase letter and for "$" / "%" (Shift+4 / Shift+5 on US and German
// layouts alike).
std::string App::hotkey_text(const char* action, const char* fallback) const {
    auto it = settings_.hotkeys.find(action);
    std::string k = (it != settings_.hotkeys.end()) ? it->second : std::string(fallback);
    if (k.empty()) return "-"; // unbound
    if (k == "$") return "SHIFT+4";
    if (k == "%") return "SHIFT+5";
    if (k == "/") return symbol_key_label(k);   // the search key: SHIFT+7 on a German keyboard
    if (k == "(" || k == ")" || k == "=") return symbol_key_label(k);   // the window keys and the scope overlay key, named for this keyboard
    if (k == "*" || k == "_") return pretty_key(k);   // the mode / karaoke size keys, named for this keyboard
    if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z') return std::string("SHIFT+") + static_cast<char>(k[0] + 32);   // SHIFT+x, like the literal legends
    if (k.find('+') != std::string::npos && k.size() > 1) return pretty_key(k);   // ALT+L, CTRL+L, CTRL+SHIFT+U ...
    return k;
}

namespace {
// "[a] first  [b] second ..." packed greedily into lines of at most `inner_w`
// columns, at most `max_lines` of them (items that no longer fit are dropped
// from the end, which is why the most important ones come first).
std::vector<std::string> pack_legend(const std::vector<std::string>& items, int inner_w, int max_lines) {
    std::vector<std::string> lines;
    if (max_lines <= 0) return lines;
    std::string cur;
    for (const auto& it : items) {
        const std::string candidate = cur.empty() ? it : cur + "   " + it;
        if (cur.empty() || display_width(candidate) <= inner_w) { cur = candidate; continue; }
        lines.push_back(cur);
        if (static_cast<int>(lines.size()) >= max_lines) return lines;
        cur = it;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}
// "SHIFT+4" + "SHIFT+5" -> "SHIFT+4/5", anything else -> "a/b".
std::string join_keys(const std::string& a, const std::string& b) {
    const std::string pre = "SHIFT+";
    if (a.rfind(pre, 0) == 0 && b.rfind(pre, 0) == 0) return a + "/" + b.substr(pre.size());
    return a + "/" + b;
}
} // namespace

// Key legend of the big list overlay: ONE OR MORE plain lines BELOW the frame
// (like the playlist / meta editor footers), in their gray. Every entry is
// kept whole -- an entry that no longer fits the panel width moves to the next
// line instead of being cut in two -- and the lines are padded to the panel
// width so they also wipe the background underneath. Never more lines than
// leave >= 3 list rows. Geometry (which shortens the frame by these lines)
// and painter both ask this.
std::vector<std::string> App::list_overlay_legend(int panel_w) const {
    const int max_lines = std::clamp(term_rows_ - kListOverlayChromeRows - 5, 1, 3);
    std::vector<std::string> items = {
        "[" + hotkey_text("HKeyAddHoveringSongToQueue", "a") + "] add to queue (next)",
        "[" + hotkey_text("HKeyQueueAddEnd", "e") + "] add to end of queue",
        "[SHIFT+UP/DOWN] page",
        "[ESC] close",
    };
    return pack_legend(items, std::max(0, panel_w), max_lines);
}

// Key legend of the big queue overlay: a small tree table (Edit / Order / Other), see layout_legend_tree().
std::vector<LegendRow> App::queue_overlay_legend(int panel_w) const {
    const int max_lines = std::clamp(term_rows_ - kQueueOverlayChromeRows - 5, 1, 3);
    std::vector<LegendGroup> groups = {
        {"Edit", {"[" + hotkey_text("HKeyRemoveHoveringSongFromQueue", "d") + "] Delete",
                  "[" + hotkey_text("HKeyClearQueue", "X") + "] Clear all",
                  "[" + hotkey_text("HKeyQueueUndoClear", "CTRL+SHIFT+Z") + "] Undo clear"}},
        {"Order", {"[" + join_keys(hotkey_text("HKeyQueueMoveUp", "4"), hotkey_text("HKeyQueueMoveDown", "5")) + "] Move up/down",
                   "[" + join_keys(hotkey_text("HKeyQueueMoveTop", "$"), hotkey_text("HKeyQueueMoveBottom", "%")) + "] Move to top/bottom\001Top/Bottom",
                   "[" + hotkey_text("HKeyQueueLock", "!") + "] " + (queue_locked_ ? "Unlock" : "Lock"),
                   "[SHIFT+s] Shuffle order\001Shuffle", "[SHIFT+u] Undo shuffle\001Unshuffle"}},
        {"Other", {"[" + hotkey_text("HKeyQueueToPlaylist", "CTRL+SHIFT+U") + "] Queue to playlist\001To playlist",
                   "[SHIFT+\u2191\u2193] Next/Prev. page\001Page", "[" + hotkey_text("HKeySearch", "/") + "] Search\001Search", "[ESC] Close"}},
    };
    return layout_legend_tree(groups, std::max(0, panel_w), max_lines);
}

namespace {
// One legend line as drawn below an overlay frame: in the legend color (the
// Colors tab's LEGEND row, `sgr` = legend_sgr()), padded to the panel width
// (so it overwrites whatever the background has under it).
std::string legend_row(const std::string& text, int panel_w, const std::string& sgr) {
    const int pad = std::max(0, panel_w - display_width(text));
    return sgr + text + std::string(pad, ' ') + "\x1b[0m";
}
// A legend with several lines hangs under the frame in the border colour, centred on the main screen's bottom border line (see
// overlay_legend_shift()); a single line keeps the key / command colours of the Colors tab.
std::string overlay_legend_line(const Settings& s, const std::string& text, int panel_w, bool several) {
    if (several) return legend_row(text, panel_w, ansi_for(s.border_color, false));
    return legend_colored(s, text) + std::string(static_cast<size_t>(std::max(0, panel_w - display_width(text))), ' ');
}
} // namespace

void App::list_overlay_geometry(int W, int& panel_w, int& list_rows) const {
    panel_w = std::min(W, std::max(40, std::min(W - 6, 160)));
    // Rows 2 .. term_rows_-1 belong to the panel: search bar (3) + list box
    // border (2) + the list rows themselves.
    // ...minus the key legend lines below the frame, so the whole overlay
    // still ends on row term_rows_ - 1 and never clips (30-row terminals
    // included).
    const int legend = static_cast<int>(list_overlay_legend(panel_w).size());
    list_rows = std::max(1, term_rows_ - 2 + overlay_legend_shift(legend) - kListOverlayChromeRows - legend);
}

// Keeps selected_ inside a window of `rows` rows and the window inside the
// list (no empty rows after the last entry when the list is long enough).
void App::list_overlay_fit_scroll(int rows) {
    rows = std::max(1, rows);
    const int total = list_total_for(list_source_, local_view_.size(), online_view_.size(), playlist_view_.size(), folder_view_.size());
    if (total <= 0) { selected_ = 0; scroll_ = 0; return; }
    selected_ = std::clamp(selected_, 0, total - 1);
    scroll_ = std::clamp(scroll_, 0, std::max(0, total - rows));
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + rows) scroll_ = selected_ - rows + 1;
}

void App::list_overlay_open() {
    if (convert_overlay_open_) convert_overlay_close();
    folder_overlay_open_ = false; // the overlays are mutually exclusive
    queue_overlay_open_ = false; // the two overlays are mutually exclusive (focus is reset just below)
    list_overlay_open_ = true;
    queue_focus_ = false; // the queue pane is hidden while the overlay is up
    list_overlay_fit_scroll(overlay_list_rows_);
}

void App::list_overlay_close() {
    list_overlay_open_ = false;
    // Both panes share selected_/scroll_: pull the window back so the cursor
    // is inside the small pane's (much shorter) view again.
    list_overlay_fit_scroll(list_visible_rows_);
}

// One page up/down. The cursor keeps its row within the window, so the eye
// stays in the same place while the list flips underneath it. At either end
// (window can't move any further) the cursor jumps to the first/last entry.
void App::list_overlay_page(int dir) {
    const int total = list_total_for(list_source_, local_view_.size(), online_view_.size(), playlist_view_.size(), folder_view_.size());
    if (total <= 0) return;
    const int rows = std::max(1, overlay_list_rows_);
    const int max_scroll = std::max(0, total - rows);
    const int rel = std::clamp(selected_ - scroll_, 0, rows - 1);
    const int new_scroll = std::clamp(scroll_ + dir * rows, 0, max_scroll);
    if (new_scroll == scroll_) {
        selected_ = (dir > 0) ? total - 1 : 0;
    } else {
        scroll_ = new_scroll;
        selected_ = std::clamp(new_scroll + rel, 0, total - 1);
    }
    list_overlay_fit_scroll(rows);
}

// The search bar and the list pane at overlay size. The list itself is the
// normal build_list_panel() (same rows, colours, sort/folder title), just
// given the wider/taller box -- only its bottom border is replaced, to carry
// the page counter and the queue size (the queue pane is hidden, so this is
// the only feedback for "add to queue"); the key legend follows as separate
// gray lines BELOW the frame.
std::vector<std::string> App::build_list_overlay_panel(int panel_w, int list_rows) const {
    std::vector<std::string> out = build_search_bar(panel_w);
    std::vector<std::string> list = build_list_panel(panel_w, list_rows);
    if (!list.empty()) {
        const int total = list_total_for(list_source_, local_view_.size(), online_view_.size(), playlist_view_.size(), folder_view_.size());
        const int rows = std::max(1, list_rows);
        const int pages = std::max(1, (total + rows - 1) / rows);
        const int page = std::clamp((scroll_ + rows / 2) / rows + 1, 1, pages);
        const int remaining = total - (scroll_ + rows);

        std::vector<std::string> parts; // dropped from the back until it fits
        if (remaining > 0) parts.push_back("( " + std::to_string(remaining) + " more )");
        parts.push_back("page " + std::to_string(page) + "/" + std::to_string(pages));
        parts.push_back("queue: " + std::to_string(queue_.size()));
        std::string footer;
        for (;;) {
            footer.clear();
            for (size_t i = 0; i < parts.size(); ++i) footer += (i ? "  " : "") + parts[i];
            if (parts.empty() || display_width(footer) + 5 <= panel_w) break;
            parts.pop_back();
        }
        if (parts.empty()) footer.clear();
        list.back() = box_bottom(panel_w, footer, ansi_for(settings_.border_color_bottom, false));
        // Key legend: below the frame, outside it, in the legend color.
        {
            const auto lines = list_overlay_legend(panel_w);
            for (const auto& l : lines) list.push_back(overlay_legend_line(settings_, l, panel_w, lines.size() > 1));
        }
    }
    out.insert(out.end(), list.begin(), list.end());
    return out;
}

// ---------------------------------------------------------------------
// Big folder overlay (HKeyFolderOverlay, SHIFT+f) -- see app.h for the design.
// ---------------------------------------------------------------------

// true when `dir` is `root` or lies below it (component-wise, so "/a/bc" is not under "/a/b").
bool fo_is_under(const fs::path& dir, const fs::path& root) {
    auto d = dir.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++d) {
        if (d == dir.end() || *d != *r) return false;
    }
    return true;
}
fs::path fo_normal_root(const std::string& s) {
    fs::path r = path_from_utf8(s).lexically_normal();
    if (!r.has_filename() && r.has_parent_path()) r = r.parent_path();   // "/music/" -> "/music"
    return r;
}
std::string fo_name_of(const std::string& p) {
    const std::string n = path_utf8(path_from_utf8(p).filename());
    return n.empty() ? p : n;
}

bool App::folder_overlay_active() const {
    return folder_overlay_open_ && mode_ == Mode::Browse;
}

// Builds the folder tree from the scanned library: the roots are the configured local music folders (a folder holding
// tracks outside every root becomes a root of its own); only folders with tracks at or below them are listed.
void App::folder_overlay_build_tree() {
    fo_subdirs_.clear(); fo_files_.clear(); fo_count_.clear(); fo_roots_.clear();
    std::vector<fs::path> roots;
    for (const auto& r : library_roots()) if (!r.empty()) roots.push_back(fo_normal_root(r));

    std::map<std::string, std::set<std::string>> sub;
    std::set<std::string> root_keys, seen;
    for (const auto& t : all_local_tracks_) {
        const fs::path dir = t.path.parent_path();
        const std::string key = path_utf8(dir);
        fo_files_[key].push_back(path_utf8(t.path));
        if (!seen.insert(key).second) continue;
        const fs::path* best = nullptr;
        size_t best_len = 0;
        for (const auto& r : roots) {
            if (!fo_is_under(dir, r)) continue;
            size_t n = 0;
            for (auto it = r.begin(); it != r.end(); ++it) ++n;
            if (!best || n > best_len) { best = &r; best_len = n; }
        }
        const fs::path root = best ? *best : dir;
        root_keys.insert(path_utf8(root));
        fs::path cur = dir;
        while (cur != root) {
            const fs::path par = cur.parent_path();
            if (par.empty() || par == cur) break;
            if (!sub[path_utf8(par)].insert(path_utf8(cur)).second) break;   // the rest of the chain exists already
            cur = par;
        }
    }
    auto by_name = [](const std::string& a, const std::string& b) {
        const std::string x = ascii_lower_str(fo_name_of(a)), y = ascii_lower_str(fo_name_of(b));
        return x != y ? x < y : a < b;
    };
    for (auto& kv : sub) {
        std::vector<std::string> v(kv.second.begin(), kv.second.end());
        std::sort(v.begin(), v.end(), by_name);
        fo_subdirs_[kv.first] = std::move(v);
    }
    for (auto& kv : fo_files_) std::sort(kv.second.begin(), kv.second.end(), by_name);
    fo_roots_.assign(root_keys.begin(), root_keys.end());
    std::sort(fo_roots_.begin(), fo_roots_.end(), by_name);

    std::function<int(const std::string&)> count = [&](const std::string& d) -> int {
        auto c = fo_count_.find(d);
        if (c != fo_count_.end()) return c->second;
        int n = 0;
        auto f = fo_files_.find(d);
        if (f != fo_files_.end()) n += static_cast<int>(f->second.size());
        auto s = fo_subdirs_.find(d);
        if (s != fo_subdirs_.end()) for (const auto& sd : s->second) n += count(sd);
        fo_count_[d] = n;
        return n;
    };
    for (const auto& r : fo_roots_) count(r);
}

// The visible rows: the tree walk (a folder's children only when it is open), narrowed by the search words. With a
// query a folder is listed when its name or anything below it matches; the matches below it are shown, and a folder
// that matches by itself shows all of its entries once it is opened.
void App::folder_overlay_rebuild_view() {
    std::string keep;
    if (fo_sel_ >= 0 && fo_sel_ < static_cast<int>(fo_view_.size())) keep = fo_view_[static_cast<size_t>(fo_sel_)].path;
    fo_view_.clear();

    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : fo_query_) {
            if (c == ' ') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
    }
    const bool filtering = !words.empty();
    auto name_match = [&](const std::string& p) {
        const std::string n = fo_name_of(p);
        for (const auto& w : words) if (!contains_ci(n, w)) return false;
        return true;
    };
    std::map<std::string, bool> below;
    std::function<bool(const std::string&)> has_below = [&](const std::string& d) -> bool {
        auto c = below.find(d);
        if (c != below.end()) return c->second;
        bool hit = false;
        auto f = fo_files_.find(d);
        if (f != fo_files_.end()) for (const auto& file : f->second) if (name_match(file)) { hit = true; break; }
        auto s = fo_subdirs_.find(d);
        if (!hit && s != fo_subdirs_.end())
            for (const auto& sd : s->second) if (name_match(sd) || has_below(sd)) { hit = true; break; }
        return below[d] = hit;
    };

    std::function<void(const std::string&, int)> emit = [&](const std::string& d, int depth) {
        const bool open = fo_expanded_.count(d) > 0;
        const bool forced = filtering && has_below(d);
        FoRow r;
        r.path = d;
        r.label = depth == 0 ? d : fo_name_of(d);
        r.is_dir = true;
        r.depth = depth;
        r.expanded = open || forced;
        auto cnt = fo_count_.find(d);
        r.count = cnt == fo_count_.end() ? 0 : cnt->second;
        fo_view_.push_back(r);
        if (!open && !forced) return;
        const bool everything = !filtering || (open && name_match(d));
        auto s = fo_subdirs_.find(d);
        if (s != fo_subdirs_.end())
            for (const auto& sd : s->second)
                if (everything || name_match(sd) || has_below(sd)) emit(sd, depth + 1);
        auto f = fo_files_.find(d);
        if (f != fo_files_.end())
            for (const auto& file : f->second) {
                if (!everything && !name_match(file)) continue;
                FoRow fr;
                fr.path = file;
                fr.label = fo_name_of(file);
                fr.depth = depth + 1;
                fo_view_.push_back(fr);
            }
    };
    for (const auto& r : fo_roots_)
        if (!filtering || name_match(r) || has_below(r)) emit(r, 0);

    fo_sel_ = 0;
    if (!keep.empty())
        for (size_t i = 0; i < fo_view_.size(); ++i) if (fo_view_[i].path == keep) { fo_sel_ = static_cast<int>(i); break; }
}

void App::folder_overlay_open() {
    if (convert_overlay_open_) convert_overlay_close();
    if (list_overlay_open_) list_overlay_close();   // the overlays are mutually exclusive
    if (queue_overlay_open_) queue_overlay_close();
    folder_overlay_open_ = true;
    fo_search_ = false;
    fo_query_.clear();
    folder_overlay_build_tree();
    // Start on the folder of the hovered track, with the way down to it opened.
    std::string target;
    fo_marked_.clear();
    if (audiobook_) {   // the audio book player: the open book (else the book under the cursor), with its chapters shown
        if (!book_dir_.empty()) { target = book_dir_; fo_expanded_.insert(book_dir_); }
        else if (selected_ >= 0 && selected_ < static_cast<int>(folder_view_.size())) target = folder_view_[static_cast<size_t>(selected_)].path;
        for (const auto& kv : fo_files_) {   // chapters with a bookmark get a mark
            BookCue cue;
            if (!cue.load(BookCue::path_for(path_from_utf8(kv.first)))) continue;
            for (const std::string& raw : cue.get_all_raw("REM BOOKMARK")) {
                BookMark m;
                if (parse_bookmark(raw, m)) fo_marked_.insert(path_utf8(path_from_utf8(kv.first) / path_from_utf8(m.file)));
            }
        }
    } else if (list_source_ == ListSource::Local && selected_ >= 0 && selected_ < static_cast<int>(local_view_.size()))
        target = path_utf8(local_view_[static_cast<size_t>(selected_)].path.parent_path());
    else if (!folder_filter_.empty()) target = folder_filter_;
    if (!target.empty() && fo_files_.count(target)) {
        fs::path cur = path_from_utf8(target).parent_path();
        std::set<std::string> roots(fo_roots_.begin(), fo_roots_.end());
        std::string k = path_utf8(cur);
        while (!k.empty()) {
            if (fo_subdirs_.count(k)) fo_expanded_.insert(k);
            if (roots.count(k)) break;
            const fs::path par = path_from_utf8(k).parent_path();
            if (par.empty() || path_utf8(par) == k) break;
            k = path_utf8(par);
        }
    }
    fo_sel_ = 0; fo_scroll_ = 0;
    folder_overlay_rebuild_view();
    if (!target.empty())
        for (size_t i = 0; i < fo_view_.size(); ++i)
            if (fo_view_[i].is_dir && fo_view_[i].path == target) { fo_sel_ = static_cast<int>(i); break; }
}

void App::folder_overlay_close() {
    folder_overlay_open_ = false;
    fo_search_ = false;
}

// `f` in the overlay: the hovered folder (or the folder of the hovered file) goes into the LOCAL AUDIO FILES pane,
// exactly as `f` on a file does there; the file itself ends up hovered. Closes the overlay.
void App::folder_overlay_filter() {
    if (fo_sel_ < 0 || fo_sel_ >= static_cast<int>(fo_view_.size())) return;
    const FoRow row = fo_view_[static_cast<size_t>(fo_sel_)];
    const std::string dir = row.is_dir ? row.path : path_utf8(path_from_utf8(row.path).parent_path());
    if (audiobook_) {   // `f`: the book opens in the chapter pane
        folder_overlay_close();
        if (!book_open_dir(dir)) { status_line_ = "that folder holds no audio files"; return; }
        if (!row.is_dir) for (size_t i = 0; i < chapters_.size(); ++i) if (path_utf8(chapters_[i].path) == row.path) { queue_selected_ = static_cast<int>(i); book_fit_scroll(); break; }
        return;
    }
    folder_filter_ = dir;
    last_local_query_.clear();   // otherwise an old query would hide files of the folder
    local_sort_mode_ = 0;        // folder order, like `f`
    list_source_ = ListSource::Local;
    refresh_local_view();
    if (!row.is_dir) {
        for (size_t i = 0; i < local_view_.size(); ++i)
            if (path_utf8(local_view_[i].path) == row.path) { selected_ = static_cast<int>(i); break; }
        scroll_ = std::max(0, selected_ - list_visible_rows_ / 2);
    }
    folder_overlay_close();
    queue_focus_ = false;
    if (local_view_.empty()) status_line_ = "no files directly in this folder -- open one of its subfolders";
    log_event("filtered: " + fo_name_of(dir));
}

bool App::folder_overlay_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int total = static_cast<int>(fo_view_.size());
    const int rows = std::max(1, fo_rows_);
    auto move_to = [&](int idx) { fo_sel_ = total > 0 ? std::clamp(idx, 0, total - 1) : 0; };
    auto page = [&](int dir) {
        if (total <= 0) return;
        const int max_scroll = std::max(0, total - rows);
        const int rel = std::clamp(fo_sel_ - fo_scroll_, 0, rows - 1);
        const int ns = std::clamp(fo_scroll_ + dir * rows, 0, max_scroll);
        if (ns == fo_scroll_) fo_sel_ = dir > 0 ? total - 1 : 0;
        else { fo_scroll_ = ns; fo_sel_ = std::clamp(ns + rel, 0, total - 1); }
    };

    // Up / Down work in the list and in the search field alike.
    if (arrow && (key == 'A' || key == 'B')) {
        if (last_key_was_shifted()) page(key == 'A' ? -1 : 1);
        else move_to(fo_sel_ + (key == 'A' ? -1 : 1));
        return true;
    }

    const FoRow* cur = (fo_sel_ >= 0 && fo_sel_ < total) ? &fo_view_[static_cast<size_t>(fo_sel_)] : nullptr;
    if (key == kKeyCtrlP) {      // CTRL+p: play the hovered file; the overlay stays open
        if (!cur) return true;
        if (cur->is_dir) { status_line_ = "CTRL+p plays a chapter -- open the book (ENTER) and pick one"; return true; }
        if (audiobook_) {   // plays from its start at once (no questions), the overlay stays
            if (!book_open_dir(path_utf8(path_from_utf8(cur->path).parent_path()))) return true;
            for (size_t i = 0; i < chapters_.size(); ++i)
                if (path_utf8(chapters_[i].path) == cur->path) { book_prompt_paused_ = false; book_play_chapter(static_cast<int>(i)); break; }
            return true;
        }
        for (const auto& t : all_local_tracks_)
            if (path_utf8(t.path) == cur->path) {
                queue_focus_ = false;
                start_local_track(t);
                return true;
            }
        status_line_ = "file is not in the library";
        return true;
    }
    if (fo_search_) {   // typing the query
        if (key == 27 || key == '\r' || key == '\n' || key == 9) { fo_search_ = false; return true; }
        if (key == 127 || key == 8) { pop_utf8_char(fo_query_); folder_overlay_rebuild_view(); return true; }
        if (arrow) return true;
        if (is_text_key(key) && fo_query_.size() < 100) { fo_query_ += static_cast<char>(key); folder_overlay_rebuild_view(); }
        return true;
    }

    if (key == 27 || (audiobook_ && key == 'S')) { folder_overlay_close(); return true; }
    if (key == '/' || key == 9) { fo_search_ = true; return true; }

    if (key == '\r' || key == '\n') {
        if (!cur) return true;
        if (cur->is_dir) {
            if (fo_expanded_.count(cur->path)) fo_expanded_.erase(cur->path); else fo_expanded_.insert(cur->path);
            folder_overlay_rebuild_view();
        } else if (audiobook_) {   // ENTER on a chapter: its book opens and the chapter plays (with the bookmark questions)
            const std::string file = cur->path;
            folder_overlay_close();
            if (!book_play_file(file)) status_line_ = "that file no longer exists";
        } else {
            status_line_ = "[" + hotkey_text("HKeyFilterForFolder", "f") + "] shows this file's folder in the list";
        }
        return true;
    }
    if (arrow && key == 'C') {   // Right: open
        if (cur && cur->is_dir && !cur->expanded) { fo_expanded_.insert(cur->path); folder_overlay_rebuild_view(); }
        return true;
    }
    if (arrow && key == 'D') {   // Left: close, or go up to the parent folder
        if (!cur) return true;
        if (cur->is_dir && cur->expanded && fo_expanded_.count(cur->path)) {
            fo_expanded_.erase(cur->path);
            folder_overlay_rebuild_view();
        } else {
            for (int i = fo_sel_ - 1; i >= 0; --i)
                if (fo_view_[static_cast<size_t>(i)].is_dir && fo_view_[static_cast<size_t>(i)].depth < cur->depth) { fo_sel_ = i; break; }
        }
        return true;
    }

    // Everything else goes through the hotkeys: `f` (and the overlay key itself) are ours, the playback keys fall
    // through to the normal dispatch, anything that would act on the hidden list is swallowed.
    int lookup = (key == '\r') ? '\n' : key;
    std::string action = resolve_hotkey_action(lookup);
    if (action.empty() && lookup >= 'a' && lookup <= 'z') action = resolve_hotkey_action(lookup - 32);
    if (action.empty() && lookup >= 'A' && lookup <= 'Z') action = resolve_hotkey_action(lookup + 32);
    if (audiobook_ && !fo_search_ && book_overlay_switch(key)) return true;   // SHIFT+k / m / l: the other audio book overlays
    if (action == "HKeyFolderOverlay") { folder_overlay_close(); return true; }
    if (action == "HKeyListOverlay") { folder_overlay_close(); list_overlay_open(); return true; }     // switch without closing first
    if (action == "HKeyQueueOverlay") { folder_overlay_close(); queue_overlay_open(); return true; }
    if (action == "HKeyFilterForFolder") { folder_overlay_filter(); return true; }
    static const char* const kPass[] = {
        "HKeyTogglePlayPause", "HKeyPlayNextSong", "HKeyPlayPreviousSong", "HKeyShuffleNext", "HKeyCyclePlayMode",
        "HKeySeekForward", "HKeySeekBackward", "HKeyIncreaseVolume", "HKeyDecreaseVolume", "HKeyToggleMute",
        "HKeyToggleNormalize", "HKeyQuit"};
    for (const char* a : kPass) if (action == a) return false;
    return true;
}

void App::folder_overlay_geometry(int W, int& panel_w, int& rows) const {
    panel_w = std::min(W, std::max(40, std::min(W - 6, 160)));
    const int legend = audiobook_ ? static_cast<int>(folder_overlay_book_legend(panel_w).size()) : static_cast<int>(folder_overlay_legend(panel_w).size());
    rows = std::max(1, term_rows_ - 2 + overlay_legend_shift(legend) - kListOverlayChromeRows - legend);
}

// The legend of the library overlay of the audio book mode: a tree table in three groups (the legends of the meta editor look like this).
std::vector<LegendRow> App::folder_overlay_book_legend(int panel_w) const {
    const std::vector<LegendGroup> groups = {
        {"Open", {"[ENTER] Open the book / play the chapter\001Open", "[\u2190\u2192] Close/open a book\001Fold", "[" + hotkey_text("HKeyFilterForFolder", "f") + "] Book to the chapter pane\001To pane"}},
        {"Play", {"[CTRL+p] Play from the start (the overlay stays)\001Play"}},
        {"Find", {"[/] Search", "[SHIFT+\u2191\u2193] Page", "[ESC] Close"}}};
    return layout_legend_tree(groups, std::max(0, panel_w), 3);
}

std::vector<std::string> App::folder_overlay_legend(int panel_w) const {
    const int max_lines = std::clamp(term_rows_ - kListOverlayChromeRows - 5, 1, 3);
    std::vector<std::string> items = audiobook_ ? std::vector<std::string>{
        "[←→] open/close book",
        "[ENTER] open the book / play the chapter",
        "[" + hotkey_text("HKeyFilterForFolder", "f") + "] book to the chapter pane",
        "[CTRL+p] play from the start",
        "[/] search",
        "[SHIFT+↑↓] page",
        "[ESC] close",
    } : std::vector<std::string>{
        "[ENTER/←→] open/close folder",
        "[" + hotkey_text("HKeyFilterForFolder", "f") + "] folder to list",
        "[CTRL+p] play",
        "[/] search",
        "[SHIFT+↑↓] page",
        "[ESC] close",
    };
    return pack_legend(items, std::max(0, panel_w), max_lines);
}

std::vector<std::string> App::build_folder_overlay_panel(int panel_w, int list_rows) const {
    const std::string border_ansi = ansi_for(settings_.border_color, false);
    const std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    // --- search bar (same look as the main one) ---
    {
        const int search_w = panel_w - 5;
        out.push_back(box_top(audiobook_ ? "SEARCH BOOKS" : "SEARCH LOCAL FOLDERS", search_w, border_ansi) + border_ansi + "╭───╮\x1b[0m");
        const int inner = std::max(0, search_w - 4);
        const std::string pre = audiobook_ ? "/" : "/f:";
        std::string q = fo_query_;
        const int room = std::max(0, inner - display_width(pre) - (fo_search_ ? 1 : 0));
        if (display_width(q) > room) q = truncate_str(q, room);
        const int fill = std::max(0, inner - display_width(pre) - display_width(q) - (fo_search_ ? 1 : 0));
        std::string line = bar + " " + search_idle_sgr(settings_) + pre + search_run(settings_, q, !q.empty())
                         + (fo_search_ ? search_idle_sgr(settings_) + "█" : std::string())
                         + std::string(static_cast<size_t>(fill), ' ') + "\x1b[0m " + bar;
        out.push_back(line + border_ansi + settings_.box_vertical + " " + std::string(1, play_mode_letter()) + " " + settings_.box_vertical + "\x1b[0m");
        out.push_back(box_bottom(search_w, "", border_ansi_bottom) + border_ansi_bottom + "╰───╯\x1b[0m");
    }

    // --- the tree ---
    const int inner = panel_w - 4;
    const int total = static_cast<int>(fo_view_.size());
    out.push_back(box_top(audiobook_ ? "AUDIO BOOKS" : "LOCAL AUDIO FILES FOLDER", panel_w, border_ansi));
    for (int row = 0; row < list_rows; ++row) {
        const int idx = fo_scroll_ + row;
        std::string content;
        bool playing = false;
        if (idx < total) {
            const FoRow& r = fo_view_[static_cast<size_t>(idx)];
            const std::string indent(static_cast<size_t>(r.depth) * 2, ' ');
            const std::string mark = r.is_dir ? (r.expanded ? "▾ " : "▸ ") : (audiobook_ && fo_marked_.count(r.path) ? "◆ " : "  ");   // (◆: a bookmark)
            const std::string head = indent + mark;
            if (r.is_dir) {
                const std::string cnt = std::to_string(r.count) + (audiobook_ ? (r.count == 1 ? " chapter" : " chapters") : (r.count == 1 ? " track" : " tracks"));
                const int name_w = std::max(5, inner - display_width(head) - 2 - static_cast<int>(cnt.size()));
                content = head + pad_right(truncate_str(apply_font_map(r.label, settings_.font_map), name_w), name_w) + "  " + cnt;
            } else {
                content = head + apply_font_map(r.label, settings_.font_map);
                playing = has_track_ && path_utf8(current_path_) == r.path;
            }
        }
        const std::string padded = pad_right(truncate_str(content, inner), inner);
        if (idx == fo_sel_ && idx < total)
            out.push_back(bar + " " + cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + padded + "\x1b[0m " + bar);
        else if (playing)
            out.push_back(bar + " " + ansi_for(settings_.list_playing_color) + bg_ansi_for(settings_.list_playing_bg_color) + padded + "\x1b[0m " + bar);
        else
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color) + padded + "\x1b[0m " + bar);
    }
    {
        const int rows = std::max(1, list_rows);
        const int pages = std::max(1, (total + rows - 1) / rows);
        const int page = std::clamp((fo_scroll_ + rows / 2) / rows + 1, 1, pages);
        const int remaining = total - (fo_scroll_ + rows);
        std::vector<std::string> parts;
        if (remaining > 0) parts.push_back("( " + std::to_string(remaining) + " more )");
        parts.push_back("page " + std::to_string(page) + "/" + std::to_string(pages));
        std::string footer;
        for (;;) {
            footer.clear();
            for (size_t i = 0; i < parts.size(); ++i) footer += (i ? "  " : "") + parts[i];
            if (parts.empty() || display_width(footer) + 5 <= panel_w) break;
            parts.pop_back();
        }
        if (parts.empty()) footer.clear();
        out.push_back(box_bottom(panel_w, footer, border_ansi_bottom));
    }
    {
        if (audiobook_) {
            const auto lrows = folder_overlay_book_legend(panel_w);
            for (size_t i = 0; i < lrows.size(); ++i) out.push_back(paint_legend(settings_, lrows, i, border_ansi, panel_w, false));
        } else {
            const auto lines = folder_overlay_legend(panel_w);
            for (const auto& l : lines) out.push_back(overlay_legend_line(settings_, l, panel_w, lines.size() > 1));
        }
    }
    return out;
}

} // namespace muisc
