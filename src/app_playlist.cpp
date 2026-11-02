// app_playlist.cpp
// Part of class App (src/app.h): The playlist editor, the playlist export / import and the playlist screen.
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


// ---------------------------------------------------------------------
// Playlist editor overlay (Mode::Playlist, HKeyPlaylist)
// ---------------------------------------------------------------------

// The library search of the playlist and meta editors: a normal search, or -- with "/f:" (or "f:") in front of the query --
// every track whose folder matches: each word of the rest must appear in "<parent folder> <folder name>", as in the main
// UI's "/f:" list. No sort here; the callers add their own.
std::vector<LocalTrack> App::filter_local_or_folders(const std::string& raw) const {
    std::string q = raw;
    size_t i = 0;
    while (i < q.size() && q[i] == ' ') ++i;
    if (q.size() >= i + 3 && q[i] == '/') ++i;
    const bool folders = q.size() >= i + 2 && (q[i] == 'f' || q[i] == 'F') && q[i + 1] == ':';
    if (!folders) return filter_and_rank_local(raw);
    std::vector<std::string> words;
    std::string cur;
    for (size_t k = i + 2; k <= q.size(); ++k) {
        if (k == q.size() || q[k] == ' ') { if (!cur.empty()) words.push_back(cur); cur.clear(); }
        else cur += q[k];
    }
    std::vector<LocalTrack> out;
    for (const auto& t : all_local_tracks_) {
        const fs::path dir = t.path.parent_path();
        const std::string hay = path_utf8(dir.parent_path().filename()) + " " + path_utf8(dir.filename());
        bool ok = true;
        for (const auto& w : words) if (!contains_ci(hay, w)) { ok = false; break; }
        if (ok) out.push_back(t);
    }
    return out;
}

const char* App::playlist_sort_name(int mode) {
    switch (mode) {
        case 1: return "Filenames A-Z";
        case 2: return "Title A-Z";
        case 3: return "Artist A-Z";
        case 4: return "Album A-Z";
        case 5: return "Recently added";
        default: return "folder order";
    }
}

// The LIBRARY list of the playlist editor: the search result (or, with "/f:" in front of the query, every track whose
// folder matches -- each word of the rest must appear in "<parent folder> <folder name>", as in the main UI's "/f:" list),
// then the sort of Shift+T. Unlike the main list the sort also applies to a search result, so a found set can be put in
// order; sort 0 keeps the relevance ranking / folder order.
void App::playlist_refresh_lib_view() {
    playlist_edit_lib_view_ = filter_local_or_folders(playlist_edit_lib_query_);
    if (playlist_edit_sort_ == 5) {   // recently added: newest first (a stable sort: equal times keep their order)
        std::vector<std::pair<long long, size_t>> keyed;
        keyed.reserve(playlist_edit_lib_view_.size());
        for (size_t i = 0; i < playlist_edit_lib_view_.size(); ++i) keyed.emplace_back(added_time_of(playlist_edit_lib_view_[i].path), i);
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<LocalTrack> sorted;
        sorted.reserve(keyed.size());
        for (const auto& k : keyed) sorted.push_back(std::move(playlist_edit_lib_view_[k.second]));
        playlist_edit_lib_view_ = std::move(sorted);
    } else if (playlist_edit_sort_ != 0) {
        struct Key { bool empty; std::string s; };
        std::vector<std::pair<Key, size_t>> keyed;
        keyed.reserve(playlist_edit_lib_view_.size());
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            for (size_t i = 0; i < playlist_edit_lib_view_.size(); ++i) {
                const LocalTrack& t = playlist_edit_lib_view_[i];
                auto it = row_meta_cache_.find(path_utf8(t.path));
                std::string k;
                if (playlist_edit_sort_ == 1) k = t.title;
                else if (playlist_edit_sort_ == 2) k = (it != row_meta_cache_.end() && !it->second.title.empty()) ? it->second.title : t.title;
                else if (playlist_edit_sort_ == 3) k = (it != row_meta_cache_.end() && !it->second.artist.empty()) ? it->second.artist : t.folder_artist;
                else if (it != row_meta_cache_.end()) k = it->second.album;
                keyed.push_back({{k.empty(), ascii_lower_str(k)}, i});   // no album / artist: after the others
            }
        }
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
            if (a.first.empty != b.first.empty) return !a.first.empty;
            return a.first.s < b.first.s;
        });
        std::vector<LocalTrack> sorted;
        sorted.reserve(keyed.size());
        for (const auto& k : keyed) sorted.push_back(std::move(playlist_edit_lib_view_[k.second]));
        playlist_edit_lib_view_ = std::move(sorted);
    }
    playlist_edit_lib_selected_ = std::clamp(playlist_edit_lib_selected_, 0,
        std::max(0, static_cast<int>(playlist_edit_lib_view_.size()) - 1));
}

void App::playlist_refresh_manage_view() {
    playlist_manage_view_ = filter_playlists(playlist_manage_query_);
    playlist_manage_selected_ = std::clamp(playlist_manage_selected_, 0,
        std::max(0, static_cast<int>(playlist_manage_view_.size()) - 1));
}

// HKeyPlaylist entry point -- always starts a fresh, blank playlist on
// tab 0 (name field focused, ready to type). The only way to bring an
// *existing* playlist back into the editor is explicit: tab 1, Enter on
// it (see playlist_load_into_editor()) -- that way reopening this
// overlay never silently discards an unsaved in-progress playlist by
// accident.
void App::playlist_open_editor() {
    mode_ = Mode::Playlist;
    playlist_export_open_ = false;
    text_disengage();
    playlist_tab_ = 0;
    playlist_edit_focus_ = 0;
    playlist_edit_name_.clear();
    playlist_edit_tracks_.clear();
    playlist_edit_lib_query_.clear();
    playlist_edit_lib_selected_ = 0;
    playlist_edit_track_selected_ = 0;
    playlist_status_.clear();
    playlist_edit_dirty_ = false;
    playlist_confirm_exit_ = false;
    playlist_confirm_delete_ = false;
    playlist_manage_query_.clear();
    playlist_manage_focus_ = 1; // land in the list: Enter/DEL work immediately; Tab goes to the search box
    playlist_refresh_lib_view();
    playlist_refresh_manage_view();
}

void App::playlist_load_into_editor(const std::string& name) {
    auto pl = load_playlist(name);
    if (!pl) { playlist_status_ = "could not load \"" + name + "\""; return; }
    playlist_edit_name_ = pl->name;
    playlist_edit_tracks_ = pl->tracks;
    resolve_playlist_tracks_async();
    playlist_edit_track_selected_ = 0;
    playlist_tab_ = 0;
    playlist_edit_focus_ = 1;
    text_disengage();
    playlist_edit_dirty_ = false; // freshly loaded from disk -- matches what's saved, nothing to lose yet
    playlist_status_ = "editing \"" + pl->name + "\" (" + std::to_string(pl->tracks.size()) + " tracks)";
}


// ---------------------------------------------------------------------
// Playlist export (M3U8 / M3U)
// ---------------------------------------------------------------------

void App::playlist_export_begin() {
    const int total = static_cast<int>(playlist_manage_view_.size());
    if (playlist_manage_selected_ < 0 || playlist_manage_selected_ >= total) { playlist_status_ = "no playlist selected"; return; }
    playlist_export_name_ = playlist_manage_view_[playlist_manage_selected_].name;
    playlist_export_dir_ = settings_.playlist_export_path.empty() ? path_utf8(playlists_dir()) : settings_.playlist_export_path;
    playlist_export_m3u_ = false;
    playlist_export_field_ = 0;
    playlist_export_status_.clear();
    edit_owner_.clear();   // the folder field takes the caret fresh (at the end)
    playlist_export_open_ = true;
}

void App::playlist_export_run() {
    std::string dir = playlist_export_dir_;
    while (!dir.empty() && (dir.front() == ' ' || dir.front() == '\t')) dir.erase(dir.begin());
    while (!dir.empty() && (dir.back() == ' ' || dir.back() == '\t')) dir.pop_back();
    if (!dir.empty() && dir[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) dir = std::string(home) + dir.substr(1);
    }
    if (dir.empty()) { playlist_export_status_ = "enter a folder first"; playlist_export_field_ = 0; return; }
    auto pl = load_playlist(playlist_export_name_);
    if (!pl) { playlist_export_status_ = "could not load \"" + playlist_export_name_ + "\""; return; }

    const fs::path folder = path_from_utf8(dir);
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (!fs::is_directory(folder, ec)) { playlist_export_status_ = "cannot create the folder " + dir; return; }
    const std::string ext = playlist_export_m3u_ ? ".m3u" : ".m3u8";
    const fs::path file = folder / path_from_utf8(PlaylistManager::sanitize_name(pl->name) + ext);

    // Extended M3U: one #EXTINF line (length unknown = -1, "Artist - Title")
    // and the absolute path per track. Both formats are written as UTF-8 --
    // M3U8 is defined that way, and current players read a plain .m3u as
    // UTF-8 too. Missing files are left out (and counted).
    std::ostringstream out;
    out << "#EXTM3U\n";
    out << "#PLAYLIST:" << pl->name << "\n";
    int written = 0, missing = 0;
    for (const auto& t : pl->tracks) {
        if (!fs::exists(t.path, ec)) { ++missing; continue; }
        std::string title = t.title, artist = t.artist;
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            auto it = row_meta_cache_.find(path_utf8(t.path));
            if (it != row_meta_cache_.end()) {
                if (!it->second.title.empty()) title = it->second.title;
                if (!it->second.artist.empty()) artist = it->second.artist;
            }
        }
        out << "#EXTINF:-1," << (artist.empty() ? title : artist + " - " + title) << "\n";
        out << path_utf8(fs::absolute(t.path, ec)) << "\n";
        ++written;
    }
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) { playlist_export_status_ = "cannot write " + path_utf8(file); return; }
    f << out.str();
    f.close();
    playlist_export_open_ = false;
    playlist_status_ = "exported " + std::to_string(written) + " track" + (written == 1 ? "" : "s") + " to " + path_utf8(file)
                     + (missing > 0 ? " (" + std::to_string(missing) + " missing, left out)" : "");
    log_event(playlist_status_);
}

std::vector<std::string> App::build_playlist_export_panel(int panel_w) const {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string bar = border + settings_.box_vertical + "\x1b[0m";
    const std::string R = "\x1b[0m";
    const int inner = panel_w - 4;
    std::vector<std::string> lines;
    auto row = [&](const std::string& painted, int cols) {
        lines.push_back(bar + " " + painted + std::string(static_cast<size_t>(std::max(0, inner - cols)), ' ') + " " + bar);
    };
    lines.push_back(box_top("EXPORT PLAYLIST \"" + truncate_str(playlist_export_name_, std::max(4, panel_w - 24)) + "\"", panel_w, border));
    row("", 0);
    {   // the folder field, with caret while it has the focus
        const std::string label = "Folder : ";
        const int lw = display_width(label);
        const bool f = playlist_export_field_ == 0;
        std::string painted; int cols;
        if (f) {
            const size_t c = std::min(edit_caret_, playlist_export_dir_.size());
            const size_t a = std::min(edit_anchor_, playlist_export_dir_.size());
            EditPaint p = paint_edit_field(playlist_export_dir_, edit_owner_ == "playlist-export-dir" ? c : playlist_export_dir_.size(),
                                           edit_owner_ == "playlist-export-dir" ? a : playlist_export_dir_.size(),
                                           std::max(4, inner - lw), "", true);
            painted = p.s; cols = p.cols;
        } else {
            painted = truncate_str(playlist_export_dir_, std::max(4, inner - lw));
            cols = display_width(painted);
        }
        row(header_sgr(settings_) + label + R + painted, lw + cols);
    }
    {   // the format row
        const std::string label = "Format : ";
        const bool f = playlist_export_field_ == 1;
        const std::string hi = f ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) : "\x1b[1m";
        const std::string m8 = playlist_export_m3u_ ? " M3U8 " : hi + "[M3U8]" + R;
        const std::string m3 = playlist_export_m3u_ ? hi + "[M3U]" + R : " M3U ";
        row(header_sgr(settings_) + label + R + m8 + "  " + m3 + legend_sgr(settings_) + "   (M3U8 = UTF-8, the default)" + R,
            display_width(label) + 6 + 2 + 5 + display_width("   (M3U8 = UTF-8, the default)"));
    }
    {
        const std::string file = PlaylistManager::sanitize_name(playlist_export_name_) + (playlist_export_m3u_ ? ".m3u" : ".m3u8");
        const std::string t = truncate_str("File   : " + file, inner);
        row(t, display_width(t));
    }
    row("", 0);
    {
        const std::string st = truncate_str(playlist_export_status_, inner);
        row("\x1b[38;5;246m" + st + R, display_width(st));
    }
    {
        const std::string hint = truncate_str("[TAB/\u2191\u2193] Field | [\u2190\u2192] Cursor / format | [ENTER] Export | [ESC] Cancel", inner);
        row(legend_colored(settings_, hint), display_width(hint));
    }
    lines.push_back(box_bottom(panel_w, "", border));
    return lines;
}

// ---------------------------------------------------------------------
// Playlist import (M3U8 / M3U)
// ---------------------------------------------------------------------

namespace {

std::string import_trim(std::string d) {
    while (!d.empty() && (d.front() == ' ' || d.front() == '\t')) d.erase(d.begin());
    while (!d.empty() && (d.back() == ' ' || d.back() == '\t')) d.pop_back();
    if (!d.empty() && d[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) d = std::string(home) + d.substr(1);
    }
    return d;
}

std::string import_lower_ext(const fs::path& p) {
    std::string e = path_utf8(p.extension());
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// "%20" etc. of a file:// URL
std::string import_url_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            o.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else o.push_back(s[i]);
    }
    return o;
}

// Reads one extended-M3U / plain M3U(8) file: the optional "#PLAYLIST:" name and
// the track paths. Relative paths are resolved against the file's own folder;
// stream URLs are skipped (a playlist here is local files only), "file://" URLs
// are unwrapped. Returns false if the file cannot be read.
bool import_parse_m3u(const fs::path& file, std::string* name_out, std::vector<fs::path>* tracks_out, int* skipped_urls) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return false;
    std::string line;
    bool first = true;
    std::error_code ec;
    while (std::getline(in, line)) {
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3); // UTF-8 BOM
        first = false;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (name_out && line.compare(0, 10, "#PLAYLIST:") == 0) {
                std::string n = line.substr(10);
                while (!n.empty() && n.front() == ' ') n.erase(n.begin());
                if (!n.empty()) *name_out = n;
            }
            continue;
        }
        if (line.compare(0, 7, "http://") == 0 || line.compare(0, 8, "https://") == 0) { if (skipped_urls) ++*skipped_urls; continue; }
        if (line.compare(0, 7, "file://") == 0) line = import_url_decode(line.substr(7));
        fs::path p = path_from_utf8(line);
#if !defined(_WIN32)
        if (line.find('\\') != std::string::npos && !fs::exists(p, ec)) { // made on Windows
            std::string alt = line;
            std::replace(alt.begin(), alt.end(), '\\', '/');
            p = path_from_utf8(alt);
        }
#endif
        if (p.is_relative()) p = file.parent_path() / p;
        if (tracks_out) tracks_out->push_back(p.lexically_normal());
    }
    return true;
}

} // namespace

void App::playlist_import_begin() {
    playlist_import_dir_ = settings_.playlist_export_path.empty() ? path_utf8(playlists_dir()) : settings_.playlist_export_path;
    playlist_import_field_ = 0;
    playlist_import_status_.clear();
    edit_owner_.clear();   // the folder field takes the caret fresh (at the end)
    playlist_import_open_ = true;
    playlist_import_scan();
}

void App::playlist_import_scan() {
    playlist_import_files_.clear();
    playlist_import_counts_.clear();
    playlist_import_marked_.clear();
    playlist_import_selected_ = 0;
    const std::string dir = import_trim(playlist_import_dir_);
    if (dir.empty()) { playlist_import_status_ = "enter a folder"; return; }
    std::error_code ec;
    const fs::path folder = path_from_utf8(dir);
    if (!fs::is_directory(folder, ec)) { playlist_import_status_ = "not a folder"; return; }
    std::vector<fs::path> found;
    for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string ext = import_lower_ext(it->path());
        if (ext == ".m3u" || ext == ".m3u8") found.push_back(it->path());
    }
    std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
        std::string x = path_utf8(a.filename()), y = path_utf8(b.filename());
        for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : y) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return x < y;
    });
    for (const auto& f : found) {
        std::vector<fs::path> tr;
        import_parse_m3u(f, nullptr, &tr, nullptr);
        playlist_import_files_.push_back(path_utf8(f));
        playlist_import_counts_.push_back(static_cast<int>(tr.size()));
    }
    playlist_import_marked_.assign(playlist_import_files_.size(), 0);
    playlist_import_status_ = playlist_import_files_.empty() ? "no .m3u / .m3u8 files in this folder" : "";
}

void App::playlist_import_run() {
    if (playlist_import_files_.empty()) { playlist_import_status_ = "nothing to import in this folder"; return; }
    // the marked files; with none marked, the hovered one
    std::vector<int> pick;
    for (size_t i = 0; i < playlist_import_marked_.size(); ++i) if (playlist_import_marked_[i]) pick.push_back(static_cast<int>(i));
    if (pick.empty()) pick.push_back(std::clamp(playlist_import_selected_, 0, static_cast<int>(playlist_import_files_.size()) - 1));

    const fs::path dir = playlists_dir();
    int imported = 0, tracks_total = 0, missing_total = 0, urls = 0;
    std::string last_name, error;
    std::error_code ec;
    for (int idx : pick) {
        const fs::path file = path_from_utf8(playlist_import_files_[idx]);
        std::string name;
        std::vector<fs::path> paths;
        if (!import_parse_m3u(file, &name, &paths, &urls)) { error = "cannot read " + path_utf8(file.filename()); continue; }
        if (paths.empty()) { error = path_utf8(file.filename()) + " has no tracks"; continue; }
        if (name.empty()) name = path_utf8(file.stem());
        // The editor's name field takes 25 characters; keep imported names inside that, and never
        // overwrite a playlist that already exists -- " (2)", " (3)" ... instead.
        const std::string base = utf8_take(name, 25);
        std::string unique = base;
        for (int n = 2; PlaylistManager::load(dir, unique).has_value() && n < 1000; ++n) {
            const std::string suffix = " (" + std::to_string(n) + ")";
            unique = utf8_take(base, 25 - static_cast<int>(suffix.size())) + suffix;
        }
        Playlist pl;
        pl.name = unique;
        for (const auto& p : paths) {
            PlaylistTrack t;
            t.path = p;
            t.title = path_utf8(p.stem());
            t.artist = path_utf8(p.parent_path().filename());
            t.missing = !fs::exists(p, ec);
            if (t.missing) ++missing_total;
            pl.tracks.push_back(std::move(t));
        }
        std::string err;
        if (!PlaylistManager::save(dir, pl, &err)) { error = "save failed: " + err; continue; }
        ++imported;
        tracks_total += static_cast<int>(pl.tracks.size());
        last_name = unique;
    }
    if (imported == 0) { playlist_import_status_ = error.empty() ? "nothing imported" : error; return; }

    playlist_import_open_ = false;
    playlist_refresh_manage_view();
    if (list_source_ == ListSource::Playlist) playlist_view_ = filter_playlists(last_playlist_query_);
    playlist_status_ = "imported " + std::to_string(imported) + " playlist" + (imported == 1 ? "" : "s")
                     + (imported == 1 ? " \"" + last_name + "\"" : std::string())
                     + " (" + std::to_string(tracks_total) + " track" + (tracks_total == 1 ? "" : "s") + ")"
                     + (missing_total > 0 ? ", " + std::to_string(missing_total) + " file(s) not found (kept, skipped when queued)" : std::string())
                     + (urls > 0 ? ", " + std::to_string(urls) + " stream URL(s) left out" : std::string())
                     + (error.empty() ? std::string() : "; " + error);
    log_event(playlist_status_);
}

std::vector<std::string> App::build_playlist_import_panel(int panel_w) const {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string bar = border + settings_.box_vertical + "\x1b[0m";
    const std::string R = "\x1b[0m";
    const int inner = panel_w - 4;
    std::vector<std::string> lines;
    auto row = [&](const std::string& painted, int cols) {
        lines.push_back(bar + " " + painted + std::string(static_cast<size_t>(std::max(0, inner - cols)), ' ') + " " + bar);
    };
    lines.push_back(box_top("IMPORT PLAYLISTS", panel_w, border));
    row("", 0);
    {   // the folder field, with caret while it has the focus
        const std::string label = "Folder : ";
        const int lw = display_width(label);
        std::string painted; int cols;
        if (playlist_import_field_ == 0) {
            const bool mine = edit_owner_ == "playlist-import-dir";
            const size_t c = std::min(edit_caret_, playlist_import_dir_.size());
            const size_t a = std::min(edit_anchor_, playlist_import_dir_.size());
            EditPaint p = paint_edit_field(playlist_import_dir_, mine ? c : playlist_import_dir_.size(),
                                           mine ? a : playlist_import_dir_.size(), std::max(4, inner - lw), "", true);
            painted = p.s; cols = p.cols;
        } else {
            painted = truncate_str(playlist_import_dir_, std::max(4, inner - lw));
            cols = display_width(painted);
        }
        row(header_sgr(settings_) + label + R + painted, lw + cols);
    }
    row("", 0);
    {
        const std::string t = "Playlists found (" + std::to_string(playlist_import_files_.size()) + ")"
                            + (playlist_import_files_.empty() ? std::string() : "   [x] = marked");
        row(header_sgr(settings_) + truncate_str(t, inner) + R, display_width(truncate_str(t, inner)));
    }
    constexpr int kRows = 8;
    const int total = static_cast<int>(playlist_import_files_.size());
    const int sel = std::clamp(playlist_import_selected_, 0, std::max(0, total - 1));
    const int scroll = std::clamp(sel - kRows / 2, 0, std::max(0, total - kRows));
    for (int r = 0; r < kRows; ++r) {
        const int i = scroll + r;
        if (i >= total) { row("", 0); continue; }
        const bool marked = i < static_cast<int>(playlist_import_marked_.size()) && playlist_import_marked_[i];
        const std::string cnt = std::to_string(playlist_import_counts_[i]) + (playlist_import_counts_[i] == 1 ? " track" : " tracks");
        const std::string fname = path_utf8(path_from_utf8(playlist_import_files_[i]).filename());
        const int name_w = std::max(5, inner - 4 - 2 - static_cast<int>(cnt.size()));
        std::string text = std::string(marked ? "[x] " : "[ ] ") + pad_right(truncate_str(fname, name_w), name_w) + "  " + cnt;
        text = pad_right(truncate_str(text, inner), inner);
        const bool hov = i == sel;
        const std::string ansi = hov ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color)
                                     : ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
        lines.push_back(bar + " " + ansi + text + R + " " + bar);
    }
    row("", 0);
    {
        const std::string st = truncate_str(playlist_import_status_, inner);
        row("\x1b[38;5;246m" + st + R, display_width(st));
    }
    {
        const std::string hint = truncate_str(playlist_import_field_ == 0
            ? "[TAB] List | [↑↓] Select | [ENTER] Import | [ESC] Cancel"
            : "[TAB] Folder | [↑↓] Select | [SPACE] Mark | [a] All | [ENTER] Import | [ESC] Cancel", inner);
        row(legend_colored(settings_, hint), display_width(hint));
    }
    lines.push_back(box_bottom(panel_w, "", border));
    return lines;
}

void App::playlist_add_hovering_to_edit() {
    if (playlist_edit_lib_selected_ < 0
        || playlist_edit_lib_selected_ >= static_cast<int>(playlist_edit_lib_view_.size())) return;
    const auto& t = playlist_edit_lib_view_[playlist_edit_lib_selected_];
    for (auto& existing : playlist_edit_tracks_) {
        if (existing.path == t.path) { playlist_status_ = "already in the playlist"; return; }
    }
    PlaylistTrack pt;
    pt.title = t.title;
    pt.artist = t.folder_artist;
    pt.path = t.path;
    pt.missing = false;
    playlist_edit_tracks_.push_back(std::move(pt));
    playlist_edit_dirty_ = true;
    playlist_status_.clear();
}

void App::playlist_remove_hovering_track() {
    if (playlist_edit_track_selected_ < 0
        || playlist_edit_track_selected_ >= static_cast<int>(playlist_edit_tracks_.size())) return;
    const std::string removed_title = playlist_edit_tracks_[playlist_edit_track_selected_].title;
    playlist_edit_tracks_.erase(playlist_edit_tracks_.begin() + playlist_edit_track_selected_);
    playlist_status_ = "removed \"" + removed_title + "\"";
    playlist_edit_track_selected_ = std::clamp(playlist_edit_track_selected_, 0,
        std::max(0, static_cast<int>(playlist_edit_tracks_.size()) - 1));
    playlist_edit_dirty_ = true;
}

// Keys 4/5 in the track list (tab 0, playlist_edit_focus_==2) -- same
// swap-with-neighbor approach as queue_move_hovering(), just against
// playlist_edit_tracks_ instead of queue_. Reordering counts as a
// change like add/remove, so it sets the dirty flag too.
void App::playlist_move_hovering_track(int dir) {
    if (playlist_edit_tracks_.empty()) return;
    int target = playlist_edit_track_selected_ + dir;
    if (target < 0 || target >= static_cast<int>(playlist_edit_tracks_.size())) return; // already at an edge
    std::swap(playlist_edit_tracks_[playlist_edit_track_selected_], playlist_edit_tracks_[target]);
    playlist_edit_track_selected_ = target;
    playlist_edit_dirty_ = true;
}

// Tab 1's DEL, fired only after playlist_confirm_delete_ has been
// confirmed with Y -- see handle_playlist_key().
void App::playlist_delete_selected() {
    if (playlist_manage_selected_ < 0
        || playlist_manage_selected_ >= static_cast<int>(playlist_manage_view_.size())) return;
    std::string name = playlist_manage_view_[playlist_manage_selected_].name;
    std::string error;
    if (!PlaylistManager::remove(playlists_dir(), name, &error)) {
        playlist_status_ = "delete failed: " + error;
        return;
    }
    playlist_status_ = "deleted \"" + name + "\"";
    playlist_refresh_manage_view();
    // Same reasoning as playlist_save_current(): keep the main UI's "/p:"
    // browse view in sync if it's currently showing playlists.
    if (list_source_ == ListSource::Playlist) playlist_view_ = filter_playlists(last_playlist_query_);
}

// HOME -- saves and STAYS in the playlist menu (ESC leaves it), so the
// confirmation shows in the menu's own status row. Only the "Save before
// exiting?" prompt passes leave=true: there the save is part of leaving.
// Deliberately not a plain letter (an earlier version used "S", which meant
// typing an "s" into the name field or the library search saved and kicked
// you out mid-keystroke); HOME can never appear inside typed text.
void App::playlist_save_current(bool leave) {
    std::string name = playlist_edit_name_;
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.empty()) {
        playlist_status_ = "enter a name first";
        playlist_edit_focus_ = 0;
        return;
    }
    Playlist pl;
    pl.name = name;
    pl.tracks = playlist_edit_tracks_;
    std::string error;
    if (!PlaylistManager::save(playlists_dir(), pl, &error)) {
        playlist_status_ = "save failed: " + error;
        return;
    }
    status_line_ = "saved playlist \"" + name + "\" (" + std::to_string(pl.tracks.size()) + " tracks)";
    playlist_status_ = status_line_; // the menu stays open now, so say it here too
    // If the main UI is currently browsing "/p:" results, refresh them
    // so a newly-saved (or renamed) playlist shows up immediately.
    if (list_source_ == ListSource::Playlist) playlist_view_ = filter_playlists(last_playlist_query_);
    playlist_edit_dirty_ = false;
    if (leave) mode_ = Mode::Browse;
}

void App::handle_playlist_key(int key) {
    // "Save before exiting?" prompt -- shown instead of the hint line
    // when ESC is pressed on tab 0 with unsaved changes (see below).
    // Swallows every key except the three it cares about so nothing
    // gets edited underneath it by accident.
    if (playlist_confirm_exit_) {
        if (key == 'y' || key == 'Y') { playlist_confirm_exit_ = false; playlist_save_current(true); return; }
        if (key == 'n' || key == 'N') { playlist_confirm_exit_ = false; mode_ = Mode::Browse; return; }
        if (key == 27) { playlist_confirm_exit_ = false; } // cancel the prompt, keep editing
        return;
    }

    // "Really delete this playlist?" prompt -- shown instead of the hint
    // line when DEL is pressed on tab 1 (see below). Same swallow-every-
    // other-key shape as playlist_confirm_exit_ above, so nothing on tab
    // 1 can be triggered by accident while it's up.
    if (playlist_confirm_delete_) {
        if (key == 'y' || key == 'Y') { playlist_confirm_delete_ = false; playlist_delete_selected(); return; }
        if (key == 'n' || key == 'N') { playlist_confirm_delete_ = false; return; }
        if (key == 27) { playlist_confirm_delete_ = false; } // cancel the prompt
        return;
    }

    // Import overlay ('i' on the Saved Playlists tab): owns every key while open.
    if (playlist_import_open_) {
        const bool arrow = last_key_was_arrow();
        const int total = static_cast<int>(playlist_import_files_.size());
        if (key == 27) { playlist_import_open_ = false; return; }
        if (key == '\r' || key == '\n' || key == kKeyCtrlS) { playlist_import_run(); return; }
        if (key == 9) { playlist_import_field_ ^= 1; return; }
        if (arrow && key == 'A') { playlist_import_selected_ = std::max(0, playlist_import_selected_ - (last_key_was_shifted() ? 10 : 1)); return; }
        if (arrow && key == 'B') { if (total > 0) playlist_import_selected_ = std::min(total - 1, playlist_import_selected_ + (last_key_was_shifted() ? 10 : 1)); return; }
        if (playlist_import_field_ == 1) { // the list: SPACE marks, 'a' marks / unmarks all
            if (key == ' ' && total > 0) {
                const int i = std::clamp(playlist_import_selected_, 0, total - 1);
                playlist_import_marked_[i] = !playlist_import_marked_[i];
            } else if (key == 'a' || key == 'A') {
                const bool any_unmarked = std::find(playlist_import_marked_.begin(), playlist_import_marked_.end(), 0) != playlist_import_marked_.end();
                std::fill(playlist_import_marked_.begin(), playlist_import_marked_.end(), any_unmarked ? 1 : 0);
            }
            return;
        }
        edit_focus("playlist-import-dir", playlist_import_dir_);
        if (edit_text_key(playlist_import_dir_, edit_caret_, edit_anchor_, key, 240, &playlist_import_status_)) playlist_import_scan();
        return;
    }

    // Export overlay ('e' on the Saved Playlists tab): owns every key while open.
    if (playlist_export_open_) {
        const bool arrow = last_key_was_arrow();
        if (key == 27) { playlist_export_open_ = false; return; }
        if (key == '\r' || key == '\n') { playlist_export_run(); return; }
        if (key == kKeyCtrlS) { playlist_export_run(); return; }
        if (key == 9 || (arrow && (key == 'A' || key == 'B'))) { playlist_export_field_ ^= 1; return; }
        if (playlist_export_field_ == 1) {   // format row: Left/Right or SPACE flip M3U8 <-> M3U
            if ((arrow && (key == 'C' || key == 'D')) || key == ' ') playlist_export_m3u_ = !playlist_export_m3u_;
            return;
        }
        edit_focus("playlist-export-dir", playlist_export_dir_);
        edit_text_key(playlist_export_dir_, edit_caret_, edit_anchor_, key, 240, &playlist_export_status_);
        return;
    }

    // Which text box (if any) has the keyboard right now -- the same owner
    // names edit_focus() uses. Empty on the two plain lists.
    const char* field = playlist_tab_ == 1 ? (playlist_manage_focus_ == 0 ? "playlist-manage-search" : "")
                      : playlist_edit_focus_ == 0 ? "playlist-name"
                      : playlist_edit_focus_ == 1 ? "playlist-lib-search" : "";

    if (key == 27) { // ESC
        // First ESC after typing: leave the field (Left/Right switch tabs
        // again, the text stays). Only an ESC outside a field leaves the menu.
        if (*field && text_engaged(field)) { text_disengage(); return; }
        if (playlist_tab_ == 0 && playlist_edit_dirty_) { playlist_confirm_exit_ = true; return; }
        mode_ = Mode::Browse;
        return;
    }
    if (key == kKeyCtrlS) {
        if (playlist_tab_ == 0) playlist_save_current();
        else playlist_status_ = "switch to Create/Edit (\u2190\u2192) to save a playlist";
        return;
    }
    // Arrows collapse to 'A'..'D' app-wide; last_key_was_arrow() is what
    // tells a real arrow from a typed capital.
    const bool arrow = last_key_was_arrow();
    if (!arrow && key_is("HKeyTranslit", key)) { toggle_latin_letters(); return; }   // SHIFT+" : Latin letters on / off (create / edit and manage tabs)
    // Tab switch: plain Left/Right, as in the meta editor and Settings --
    // EXCEPT while a text box is in use. A box only counts as in use once
    // something was typed / edited in it (text_engage()); until then the
    // arrows switch tabs, afterwards they move the caret, and ESC hands
    // them back to the tabs. Alt+Left/Right keeps working from everywhere.
    const bool lr = arrow && (key == 'C' || key == 'D');
    if (key == kKeyAltLeft || key == kKeyAltRight || (lr && !(*field && text_engaged(field)))) {
        playlist_tab_ = (playlist_tab_ + 1) % 2;
        text_disengage();
        if (playlist_tab_ == 1) playlist_refresh_manage_view();
        return;
    }

    if (playlist_tab_ == 1) {
        // --- Tab 1: browse/manage saved playlists ---
        int total = static_cast<int>(playlist_manage_view_.size());
        if (key == 9) { // Tab -- search box <-> list
            playlist_manage_focus_ = (playlist_manage_focus_ == 0) ? 1 : 0;
            text_disengage();
            return;
        }
        if (playlist_manage_focus_ == 0) { // search box -- filter, caret, marking, clipboard
            // Up/Down keep walking the filtered list while the box is
            // focused, same as the main UI's "/" search does.
            if (arrow && (key == 'A' || key == 'B')) {
                if (key == 'A') {
                    playlist_manage_selected_ = std::max(0, playlist_manage_selected_ - (last_key_was_shifted() ? 10 : 1));
                    return;
                }
                if (total > 0) playlist_manage_selected_ = std::min(total - 1, playlist_manage_selected_ + (last_key_was_shifted() ? 10 : 1));
                return;
            }
            if (key == '\r' || key == '\n') { playlist_manage_focus_ = 1; text_disengage(); return; } // Enter: leave the box, take the list
            edit_focus("playlist-manage-search", playlist_manage_query_);
            text_engage("playlist-manage-search");
            if (edit_text_key(playlist_manage_query_, edit_caret_, edit_anchor_, key, 80, &playlist_status_))
                playlist_refresh_manage_view();
            return;
        }
        if (arrow && key == 'A') { // up
            playlist_manage_selected_ = std::max(0, playlist_manage_selected_ - (last_key_was_shifted() ? 10 : 1));
            return;
        }
        if (arrow && key == 'B') { // down
            if (total > 0) playlist_manage_selected_ = std::min(total - 1, playlist_manage_selected_ + (last_key_was_shifted() ? 10 : 1));
            return;
        }
        if (key == '\r' || key == '\n') {
            if (total > 0 && playlist_manage_selected_ < total) {
                playlist_load_into_editor(playlist_manage_view_[playlist_manage_selected_].name);
            }
            return;
        }
        if (key == kKeyDelete && total > 0 && playlist_manage_selected_ < total) {
            playlist_confirm_delete_ = true;
            return;
        }
        if (key == 'e' || key == 'E') { playlist_export_begin(); return; }   // export as M3U8 / M3U
        if (key == 'i' || key == 'I') { playlist_import_begin(); return; }   // import M3U8 / M3U files
        return;
    }

    // --- Tab 0: create/edit ---
    if (key == 9) { // Tab -- cycle focus: name field -> library picker -> track list -> ...
        playlist_edit_focus_ = (playlist_edit_focus_ + 1) % 3;
        text_disengage();
        return;
    }

    if (playlist_edit_focus_ == 0) { // name field
        if (key == '\r' || key == '\n') { playlist_edit_focus_ = 1; text_disengage(); return; } // confirm name, jump to picking tracks
        if (arrow && (key == 'A' || key == 'B')) return; // single line, nothing to move to
        edit_focus("playlist-name", playlist_edit_name_);
        text_engage("playlist-name");
        // Caret, marking, clipboard and typing in one call -- Up/Down
        // arrows included (edit_text_key drops them, this box has no
        // vertical anything to navigate), where the old code had to
        // blacklist 'A'/'B' so they could not be typed either.
        if (edit_text_key(playlist_edit_name_, edit_caret_, edit_anchor_, key, 25, &playlist_status_))
            playlist_edit_dirty_ = true;
        return;
    }

    if (playlist_edit_focus_ == 1) { // library picker -- typing filters live, same as the main search box
        int total = static_cast<int>(playlist_edit_lib_view_.size());
        if (arrow && key == 'A') {
            playlist_edit_lib_selected_ = std::max(0, playlist_edit_lib_selected_ - (last_key_was_shifted() ? 10 : 1));
            return;
        }
        if (arrow && key == 'B') {
            if (total > 0) playlist_edit_lib_selected_ = std::min(total - 1, playlist_edit_lib_selected_ + (last_key_was_shifted() ? 10 : 1));
            return;
        }
        if (key == '\r' || key == '\n') { playlist_add_hovering_to_edit(); return; }
        // Shift+T (HKeyCycleSortMode) sorts the list -- while the box has not been typed into yet (a capital T typed
        // as text is a T, so start a search with a small letter, or leave the box with ESC first).
        if (!text_engaged("playlist-lib-search") && resolve_hotkey_action(key) == "HKeyCycleSortMode") {
            playlist_edit_sort_ = (playlist_edit_sort_ + 1) % 6;
            if (playlist_edit_sort_ == 5) { std::lock_guard<std::mutex> lk(added_time_mutex_); added_time_cache_.clear(); }
            playlist_refresh_lib_view();
            playlist_status_ = std::string("sort: ") + playlist_sort_name(playlist_edit_sort_);
            return;
        }
        edit_focus("playlist-lib-search", playlist_edit_lib_query_);
        text_engage("playlist-lib-search");
        if (edit_text_key(playlist_edit_lib_query_, edit_caret_, edit_anchor_, key, 120, &playlist_status_))
            playlist_refresh_lib_view();
        return;
    }

    // playlist_edit_focus_ == 2: the in-progress playlist's track list
    {
        int total = static_cast<int>(playlist_edit_tracks_.size());
        if (arrow && key == 'A') {
            playlist_edit_track_selected_ = std::max(0, playlist_edit_track_selected_ - (last_key_was_shifted() ? 10 : 1));
            return;
        }
        if (arrow && key == 'B') {
            if (total > 0) playlist_edit_track_selected_ = std::min(total - 1, playlist_edit_track_selected_ + (last_key_was_shifted() ? 10 : 1));
            return;
        }
        // DEL, Backspace, or 'd' (lowercase only -- a typed capital C/D
        // is intercepted above as the Left/Right arrow, so it only reaches
        // here as a genuine arrow, which does nothing).
        if (key == kKeyDelete || key == 127 || key == 8 || key == 'd') { playlist_remove_hovering_track(); return; }
        // 4/5 move the hovering track up/down -- same keys as the main
        // queue's HKeyQueueMoveUp/Down, kept as literal codes (like the
        // rest of this function) rather than routed through
        // resolve_hotkey_action() since this whole handler already
        // works in raw arrow-collapsed key codes, not configurable
        // hotkeys.
        if (key == '4') { playlist_move_hovering_track(-1); return; }
        if (key == '5') { playlist_move_hovering_track(1); return; }
        return;
    }
}


// SEARCH LIBRARY: the library filter's own boxed pane (title row, input row,
// bottom row), coloured with the Colors tab's SEARCH row like every other
// search bar. The input is live while the LIBRARY pane has focus -- the
// caret/selection shows then, a placeholder when it is empty and idle.
std::vector<std::string> App::build_playlist_search_panel(int total_width) const {
    const std::string border_ansi = ansi_for(settings_.border_color, false);
    const std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::vector<std::string> out;
    out.push_back(box_top(std::string("SEARCH LIBRARY") + (playlist_edit_focus_ == 1 ? focus_indicator(settings_, border_ansi) : std::string()), total_width, border_ansi));

    const std::string prefix = "Search: ";
    const int field_w = std::max(1, total_width - 4 - static_cast<int>(prefix.size()));
    std::string field;
    int field_cols = 0;
    if (playlist_edit_focus_ == 1) { // keys go to the box: caret + block cursor
        const bool typed = !playlist_edit_lib_query_.empty();
        EditPaint p = paint_edit_field(playlist_edit_lib_query_, edit_caret_, edit_anchor_, field_w, typed ? search_sgr(settings_) : search_idle_sgr(settings_), true);
        field = search_run(settings_, p.s, typed);
        field_cols = p.cols;
    } else {
        const bool typed = !playlist_edit_lib_query_.empty();
        const std::string plain = truncate_str(typed ? playlist_edit_lib_query_ : std::string("(type to filter)"), field_w);
        field = search_run(settings_, plain, typed);
        field_cols = static_cast<int>(display_width(plain));
    }
    out.push_back(box_line_field(prefix, field, field_cols, total_width, border_ansi, search_idle_sgr(settings_)));
    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_playlist_library_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    // The query has its own SEARCH LIBRARY pane above (build_playlist_search_panel());
    // typing still goes to it while this pane has focus, so the focus triangle
    // sits here, where Up/Down and ENTER act.
    out.push_back(box_top(std::string("LIBRARY") + (playlist_edit_sort_ ? std::string(" (") + playlist_sort_name(playlist_edit_sort_) + ")" : std::string())
                          + (playlist_edit_focus_ == 1 ? focus_indicator(settings_, border_ansi) : std::string()),
                          total_width, border_ansi));

    // Four columns -- Filename (narrow) | Title | Artist | Album -- separated by
    // " | ", with a header row of their own right under the title row.
    // Filename is the file's stem, Title/Artist come from the resolved
    // tags (row_meta_cache_) with the same folder-name fallback the main
    // list uses for Artist; an untagged Title shows "-". No index column:
    // the header promises exactly these three and each gets a third.
    const std::string col_sep = " | ";
    const int sep_w = static_cast<int>(col_sep.size());
    // Filename (half of its former third) | Title | Artist | Album: the rest is shared by the last three
    const int col1_w = std::max(3, (std::max(3, inner - 2 * sep_w) / 3) / 2);
    const int avail = std::max(3, inner - 3 * sep_w - col1_w);
    const int col2_w = avail / 3;
    const int col3_w = avail / 3;
    const int col4_w = avail - col2_w - col3_w;
    {
        const std::string head_sep(col_sep.size(), ' ');   // (the names only, without the "|" of the rows below)
        std::string header = pad_right(truncate_str("Filename", col1_w), col1_w) + head_sep
                           + pad_right(truncate_str("Title", col2_w), col2_w) + head_sep
                           + pad_right(truncate_str("Artist", col3_w), col3_w) + head_sep
                           + pad_right(truncate_str("Album", col4_w), col4_w);
        out.push_back(bar + " " + header_sgr(settings_)
                      + pad_right(truncate_str(header, inner), inner) + "\x1b[0m " + bar);
    }

    int total = static_cast<int>(playlist_edit_lib_view_.size());
    int scroll = std::clamp(playlist_edit_lib_selected_ - height / 2, 0, std::max(0, total - height));
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const auto& t = playlist_edit_lib_view_[idx];
            std::string tag_title, tag_artist, tag_album;
            {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto it = row_meta_cache_.find(path_utf8(t.path));
                if (it != row_meta_cache_.end()) {
                    tag_title = it->second.title;
                    tag_artist = it->second.artist;
                    tag_album = it->second.album;
                }
            }
            if (tag_title.empty()) tag_title = "-";
            if (tag_album.empty()) tag_album = "-";
            if (tag_artist.empty()) tag_artist = t.folder_artist.empty() ? std::string("-") : t.folder_artist;
            std::string c_file = apply_font_map(t.title, settings_.font_map);
            std::string c_title = apply_font_map(tag_title, settings_.font_map);
            std::string c_artist = apply_font_map(tag_artist, settings_.font_map);
            std::string c_album = apply_font_map(tag_album, settings_.font_map);
            // Every column padded to its own fixed width *before*
            // concatenating (rather than truncating the assembled whole
            // afterward) -- matches build_list_panel()'s row construction,
            // so one title's display_width() landing a column short can't
            // shift every border to its right. The hovering row's
            // filename scrolls (marquee) when it doesn't fit.
            bool row_focused_sel = (playlist_edit_focus_ == 1) && (idx == playlist_edit_lib_selected_);
            std::string file_shown = row_focused_sel
                ? marquee_or_truncate(c_file, col1_w, idx, marquee_pl_lib_row_idx_, marquee_pl_lib_since_)
                : pad_right(truncate_str(c_file, col1_w), col1_w);
            content = file_shown + col_sep
                    + (row_focused_sel ? marquee_or_truncate(c_title, col2_w, idx, marquee_pl_lib_row_idx_, marquee_pl_lib_since_) : pad_right(truncate_str(c_title, col2_w), col2_w)) + col_sep
                    + (row_focused_sel ? marquee_or_truncate(c_artist, col3_w, idx, marquee_pl_lib_row_idx_, marquee_pl_lib_since_) : pad_right(truncate_str(c_artist, col3_w), col3_w)) + col_sep
                    + (row_focused_sel ? marquee_or_truncate(c_album, col4_w, idx, marquee_pl_lib_row_idx_, marquee_pl_lib_since_) : pad_right(truncate_str(c_album, col4_w), col4_w));
        }
        bool sel = (playlist_edit_focus_ == 1) && (idx == playlist_edit_lib_selected_) && idx < total;
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_playlist_tracks_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    // When focused, also show WHICH row the cursor is on ("3/12"), so the
    // selection is readable even on a terminal/colour scheme where the
    // highlighted row is hard to see.
    std::string sel_pos;
    if (playlist_edit_focus_ == 2 && !playlist_edit_tracks_.empty()) {
        sel_pos = "  " + std::to_string(playlist_edit_track_selected_ + 1) + "/"
                + std::to_string(playlist_edit_tracks_.size());
    }
    std::string label = "TRACKS (" + std::to_string(playlist_edit_tracks_.size()) + ")" + sel_pos
                       + (playlist_edit_focus_ == 2 ? focus_indicator(settings_, border_ansi) : ""); // filled triangle: focus indicator,
                                                                        // same purpose as LIBRARY's "\u2588" text
                                                                        // cursor but this panel has no text field
                                                                        // of its own to blink a cursor in
    out.push_back(box_top(label, total_width, border_ansi));

    int total = static_cast<int>(playlist_edit_tracks_.size());
    if (total == 0) {
        int mid = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid) {
                std::string text = apply_font_map("ENTER ON A LIBRARY TRACK TO ADD IT", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            // Highlighted even with nothing to select, same cursor-color
            // treatment a real row gets below -- otherwise an empty,
            // focused panel is visually identical to an unfocused one.
            if (playlist_edit_focus_ == 2) {
                std::string cursor_ansi = cursor_sgr(settings_.queue_cursor_color, settings_.queue_cursor_bg_color);
                out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
            } else {
                std::string queue_ansi = ansi_for(settings_.queue_color, false);
                out.push_back(bar + " " + queue_ansi + padded + "\x1b[0m " + bar);
            }
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    // Same three equal columns as the LIBRARY pane above (Filename | Title |
    // Artist, identical widths so they line up under its header) -- but no
    // header row of its own, and no index column.
    const std::string col_sep = " | ";
    const int sep_w = static_cast<int>(col_sep.size());
    const int col1_w = std::max(3, (std::max(3, inner - 2 * sep_w) / 3) / 2);
    const int avail = std::max(3, inner - 3 * sep_w - col1_w);
    const int col2_w = avail / 3;
    const int col3_w = avail / 3;
    const int col4_w = avail - col2_w - col3_w;

    int scroll = std::clamp(playlist_edit_track_selected_ - height / 2, 0, std::max(0, total - height));
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const auto& t = playlist_edit_tracks_[idx];
            std::string tag_title, tag_artist, tag_album;
            {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto it = row_meta_cache_.find(path_utf8(t.path));
                if (it != row_meta_cache_.end()) {
                    tag_title = it->second.title;
                    tag_artist = it->second.artist;
                    tag_album = it->second.album;
                }
            }
            if (tag_title.empty()) tag_title = "-";
            if (tag_album.empty()) tag_album = "-";
            if (tag_artist.empty()) {
                std::string folder = path_utf8(t.path.parent_path().filename());
                tag_artist = folder.empty() ? std::string("-") : folder;
            }
            std::string file_txt = t.missing ? (t.title + " [missing]") : t.title;
            std::string c_file = apply_font_map(file_txt, settings_.font_map);
            std::string c_title = apply_font_map(tag_title, settings_.font_map);
            std::string c_artist = apply_font_map(tag_artist, settings_.font_map);
            std::string c_album = apply_font_map(tag_album, settings_.font_map);
            bool row_focused_sel = (playlist_edit_focus_ == 2) && (idx == playlist_edit_track_selected_);
            std::string file_shown = row_focused_sel
                ? marquee_or_truncate(c_file, col1_w, idx, marquee_pl_track_row_idx_, marquee_pl_track_since_)
                : pad_right(truncate_str(c_file, col1_w), col1_w);
            content = file_shown + col_sep
                    + (row_focused_sel ? marquee_or_truncate(c_title, col2_w, idx, marquee_pl_track_row_idx_, marquee_pl_track_since_) : pad_right(truncate_str(c_title, col2_w), col2_w)) + col_sep
                    + (row_focused_sel ? marquee_or_truncate(c_artist, col3_w, idx, marquee_pl_track_row_idx_, marquee_pl_track_since_) : pad_right(truncate_str(c_artist, col3_w), col3_w)) + col_sep
                    + (row_focused_sel ? marquee_or_truncate(c_album, col4_w, idx, marquee_pl_track_row_idx_, marquee_pl_track_since_) : pad_right(truncate_str(c_album, col4_w), col4_w));
        }
        bool sel = (playlist_edit_focus_ == 2) && (idx == playlist_edit_track_selected_) && idx < total;
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.queue_cursor_color, settings_.queue_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string queue_ansi = ansi_for(settings_.queue_color, false) + bg_ansi_for(settings_.queue_inactive_bg_color);
            out.push_back(bar + " " + queue_ansi + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

// SEARCH PLAYLISTS: the saved-playlists filter in a pane of its own, coloured
// like every search bar (query highlighted only as far as it reaches).
std::vector<std::string> App::build_playlist_manage_search_panel(int total_width) const {
    const std::string border_ansi = ansi_for(settings_.border_color, false);
    const std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::vector<std::string> out;
    out.push_back(box_top(std::string("SEARCH PLAYLISTS") + (playlist_manage_focus_ == 0 ? focus_indicator(settings_, border_ansi) : std::string()),
                          total_width, border_ansi));
    const std::string prefix = "Search: ";
    std::string field;
    int field_cols = 0;
    const bool typed = !playlist_manage_query_.empty();
    if (playlist_manage_focus_ == 0) { // focus is in the box: caret + block cursor
        EditPaint p = paint_edit_field(playlist_manage_query_, edit_caret_, edit_anchor_,
                                       std::max(1, total_width - 4 - static_cast<int>(prefix.size())),
                                       typed ? search_sgr(settings_) : search_idle_sgr(settings_), true);
        field = search_run(settings_, p.s, typed);
        field_cols = p.cols;
    } else { // focus elsewhere: plain text, with the placeholder hint
        const std::string plain = typed ? playlist_manage_query_ : std::string("(type to filter)");
        field = search_run(settings_, plain, typed);
        field_cols = static_cast<int>(display_width(plain));
    }
    out.push_back(box_line_field(prefix, field, field_cols, total_width, border_ansi, search_idle_sgr(settings_)));
    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_playlist_manage_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;
    // The ◀ marks whichever pane has the keys: this list when the search pane
    // (build_playlist_manage_search_panel(), drawn right above) is not the one.
    out.push_back(box_top(std::string("SAVED PLAYLISTS") + (playlist_manage_focus_ == 1 ? focus_indicator(settings_, border_ansi) : std::string()),
                          total_width, border_ansi));

    const int list_height = std::max(1, height); // the caller already left room for the search pane

    int total = static_cast<int>(playlist_manage_view_.size());
    if (total == 0) {
        int mid = list_height / 2;
        for (int row = 0; row < list_height; ++row) {
            std::string content;
            if (row == mid) {
                std::string text = apply_font_map(playlist_manage_query_.empty() ? "NO SAVED PLAYLISTS YET" : "NO MATCHING PLAYLISTS", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string list_ansi = ansi_for(settings_.list_color, false);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    int scroll = std::clamp(playlist_manage_selected_ - list_height / 2, 0, std::max(0, total - list_height));
    const int idx_w = 3;
    for (int row = 0; row < list_height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const auto& p = playlist_manage_view_[idx];
            const int count_w = 10;
            int name_w = std::max(5, inner - idx_w - 2 - 2 - count_w);
            std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
            std::string t_name = apply_font_map(p.name, settings_.font_map);
            std::string t_count = std::to_string(p.track_count) + (p.track_count == 1 ? " track" : " tracks");
            content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                    + pad_right(truncate_str(t_name, name_w), name_w) + settings_.list_separator + " "
                    + pad_right(t_count, count_w);
        }
        bool sel = (idx == playlist_manage_selected_) && idx < total;
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + list_height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

// Assembles the full-screen playlist editor overlay. Structured like the
// Browse view's own stack of boxed panels (a header box, then side-by-
// side boxed sub-panels, then a plain hint/status line) rather than
// Settings' absolute-positioned single mega-box -- same box-drawing
// vocabulary (box_top/box_line/box_bottom), simpler composition.
void App::build_playlist_screen(std::ostringstream& frame, int W, int target_height) const {
    if (W < 60) W = 60;
    std::string border = ansi_for(settings_.border_color, false);
    std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string HI = "\x1b[7m", R = "\x1b[0m";

    int fixed_rows = 3; // the two strip lines + bottom border
    {   // title on the border line + the settings-style tab strip (no numbers); the pane below stays separate.
        // On the CREATE / EDIT tab the name field sits on the strip's second line, left of the tab indicators
        // (which are pushed to the right so the 25 characters of a name always fit).
        const int name_max = 25;
        const std::string prefix = " Name: ";
        const std::string dirty_mark = playlist_edit_dirty_ ? " *" : "";
        const int mark_w = static_cast<int>(dirty_mark.size());
        const int field_w = name_max + 1;                       // 25 characters + the caret block
        const int inner_w = static_cast<int>(prefix.size()) + field_w + 2 + 2 + 1;   // room for " *" and the " â" focus mark always: the strip never jumps
        std::string line2;
        int line2_cols = 0;
        if (playlist_tab_ == 0) {
            std::string field;
            int field_cols = 0;
            if (playlist_edit_focus_ == 0) {
                EditPaint p = paint_edit_field(playlist_edit_name_, edit_caret_, edit_anchor_, field_w, "", true);
                field = p.s;
                field_cols = p.cols;
            } else {
                field = truncate_str(playlist_edit_name_.empty() ? std::string("(untitled)") : playlist_edit_name_, field_w);
                field_cols = static_cast<int>(display_width(field));
            }
            std::string focus_mark;
            int focus_w = 0;
            if (playlist_edit_focus_ == 0) { focus_mark = " " + focus_sgr(settings_) + "\xe2\x97\x80" + "\x1b[0m"; focus_w = 2; }
            line2 = prefix + field + dirty_mark + "\x1b[0m" + focus_mark;
            line2_cols = static_cast<int>(prefix.size()) + field_cols + mark_w + focus_w;
        }
        const auto strip = menu_tab_strip(W, "PLAYLISTS", {"CREATE / EDIT", "SAVED PLAYLISTS"}, playlist_tab_, border, border,
                                          settings_.box_upper_left, settings_.box_upper_right, settings_.box_vertical,
                                          inner_w, line2, line2_cols,
                                          ansi_for(settings_.tab_current_color, false), ansi_for(settings_.tab_other_color, false));
        frame << strip.first << "\n" << strip.second << "\n";
    }
    frame << box_bottom(W, "", border_bottom) << "\n";

    // Sized to the room the terminal has (via target_height) with an 8-row
    // floor on short terminals. It has no fixed cap, so a maximised window is filled to the bottom.
    // Sized against BOTH bounds that matter (same reasoning as
    // build_meta_screen()): target_height is player_view_height(), but
    // clamp_output_rows() keeps term_rows_ - 1 lines and a full-width
    // prompt here can wrap, so never lay out taller than the screen.
    int budget = std::min(target_height, term_rows_ - 1);
    // -5: the panel box's own top+bottom border rows (added by
    // build_playlist_*_panel(), which draw a box around the panel_h rows
    // they're handed) plus the TWO legend lines + status line below it. The
    // old "-2" forgot the border rows, which made the frame 1-2 lines too
    // tall and chopped the green status line -- "removed ...", "deleted ...",
    // "added ..." -- off every single frame. Keeping that sum exact is also
    // what stops the second legend row (the text-field keys) from pushing
    // the frame past term_rows_ - 1 and scrolling the terminal.
    int panel_h = std::max(8, budget - fixed_rows - 5); // no upper cap: fills a maximised window (-5: 2 border rows + the 2-row legend + the status row)
    if (playlist_tab_ == 0) {
        // Stacked, full width: playlist box (above), LIBRARY, then TRACKS.
        // The two panes together take exactly the lines the old side-by-
        // side pair did (panel_h rows + 2 borders), so the whole screen
        // keeps its height. LIBRARY spends 3 lines on chrome (top border,
        // Filename|Title|Artist header, bottom border), TRACKS 2, which
        // leaves panel_h - 3 data rows to split roughly 1:3.
        // The SEARCH LIBRARY pane above them costs 3 more lines (top border,
        // input row, bottom border), which come out of the TRACKS pane.
        // LIBRARY has two rows more and TRACKS two fewer than the plain 1:3 split, and the third legend row ("Sort")
        // costs TRACKS one more, so the frame keeps its height.
        int data_rows = std::max(5, panel_h - 6);
        int lib_rows = std::max(2, data_rows / 4);
        int track_rows = data_rows - lib_rows;
        lib_rows += 2;
        track_rows = std::max(2, track_rows - 3);
        for (auto& l : build_playlist_search_panel(W)) frame << l << "\n";
        for (auto& l : build_playlist_library_panel(W, lib_rows)) frame << l << "\n";
        for (auto& l : build_playlist_tracks_panel(W, track_rows)) frame << l << "\n";
    } else {
        // The SEARCH PLAYLISTS pane takes 3 lines, so the list
        // gives up two rows net and the screen keeps its height.
        for (auto& l : build_playlist_manage_search_panel(W)) frame << l << "\n";
        for (auto& l : build_playlist_manage_panel(W, panel_h - 3)) frame << l << "\n";
    }

    // Footer: a full key legend (gray, "\x1b[90m") plus a status line
    // (green, "\x1b[38;5;246m") below it -- exactly the colors and layout
    // Settings' own footer uses (see build_settings_screen()'s
    // "[TAB] Switch | ... " line and its status_line_ line just below).
    if (playlist_confirm_exit_) {
        std::string prompt = "Save changes to \"" + (playlist_edit_name_.empty() ? std::string("(untitled)") : playlist_edit_name_)
                            + "\" before exiting?   [Y]es   [N]o   [ESC] cancel";
        frame << "\x1b[43;30m " << prompt << " \x1b[0m\n";
        frame << "\n\n";   // the legend's two rows + the status row stay reserved (the prompt is the first of the three), so a prompt never changes the layout above it
    } else if (playlist_confirm_delete_) {
        std::string name = (playlist_manage_selected_ >= 0
                          && playlist_manage_selected_ < static_cast<int>(playlist_manage_view_.size()))
                          ? playlist_manage_view_[playlist_manage_selected_].name : std::string();
        std::string prompt = "Delete playlist \"" + name + "\"? This can't be undone.   [Y]es   [N]o   [ESC] cancel";
        frame << "\x1b[41;97m " << prompt << " \x1b[0m\n";
        frame << "\n\n";
    } else {
        // Left/Right and ESC say what they do RIGHT NOW (see
        // handle_playlist_key()): in a text box that was typed into they move
        // the caret / leave the box, everywhere else they switch the tab /
        // close the editor.
        const char* field = playlist_tab_ == 1 ? (playlist_manage_focus_ == 0 ? "playlist-manage-search" : "")
                          : playlist_edit_focus_ == 0 ? "playlist-name"
                          : playlist_edit_focus_ == 1 ? "playlist-lib-search" : "";
        const bool in_text = *field && text_engaged(field);
        // The legend is a small tree table, like the meta editor's: two rows (Move / list keys)
        // on both tabs, so the footer always has the same height and fits 120 x 30 (shorter labels on narrower terminals).
        const std::string lr = in_text ? "[\u2190\u2192] Cursor" : "[\u2190\u2192] Switch Tab\001Tab";
        const std::string esc = in_text ? "[ESC] Leave field" : "[ESC] Clear / Exit\001Exit";
        std::vector<LegendGroup> groups;
        if (playlist_tab_ == 1) {
            groups.push_back({"Move", {lr, "[TAB] Focus", "[\u2191\u2193] Navi.", "[SHIFT+\u2191\u2193] Page", "[ENTER] Load", esc}});
            groups.push_back({"Playlist", {"[DEL] Delete", "[e/i] Export/Import"}});
        } else {
            groups.push_back({"Move", {lr, "[TAB] Focus", "[\u2191\u2193] Navi.", "[SHIFT+\u2191\u2193] Page", "[ENTER] Add/Load", esc}});
            groups.push_back({"Tracks", {"[DEL] Remove", "[4/5] Move \u2191\u2193", "[CTRL+s] Save"}});
            groups.push_back({"Sort", {"[SHIFT+t] Sorting searched titles", "[/f:] Search folders"}});
        }
        {
            const auto lrows = layout_legend_tree(groups, W - 1);
            for (size_t i = 0; i < lrows.size(); ++i) frame << " " << paint_legend(settings_, lrows, i, border, 0, true) << "\n";
        }
        if (!playlist_status_.empty()) frame << "\x1b[38;5;246m" << playlist_status_ << "\x1b[0m\n";
        else frame << "\n";
    }

}

} // namespace muisc
