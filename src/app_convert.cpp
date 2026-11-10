// app_convert.cpp
// Part of class App (src/app.h): The file conversion overlay.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "app_state_cv.h"
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
// File conversion overlay (HKeyConvertOverlay, SHIFT+c) -- see app.h for the design.
// ---------------------------------------------------------------------

namespace {
const char* const kCvTypeNames[] = {"ALL FILES", "LOSSLESS", "FLAC", "WAV", "M4A", "OGG", "OPUS", "AAC", "MP3"};
constexpr int kCvTypeCount = 9;
const int kCvMinKbps[] = {0, 320, 500, 1000};
constexpr int kCvMinCount = 4;

std::string cv_size_str(long long b) {
    char buf[32];
    if (b >= 1000000000LL) std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(b) / 1e9);
    else if (b >= 1000000LL) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / 1e6);
    else std::snprintf(buf, sizeof buf, "%lld KB", b / 1000);
    return buf;
}

// The extension alone decides the types; M4A (and so LOSSLESS) needs the probe, because an .m4a is lossless only as ALAC.
bool cv_type_ok(int type, const std::string& ext, const AudioInfo* inf) {
    switch (type) {
        case 0: return true;
        case 1: return ext == ".flac" || ext == ".wav" || (inf && inf->probed && inf->lossless);
        case 2: return ext == ".flac";
        case 3: return ext == ".wav";
        case 4: return ext == ".m4a";
        case 5: return ext == ".ogg";
        case 6: return ext == ".opus";
        case 7: return ext == ".aac";
        case 8: return ext == ".mp3";
        default: return true;
    }
}
// Files the probe thread has to look at for a type filter (the ones the type could still let through).
bool cv_probe_candidate(int type, const std::string& ext) {
    return cv_type_ok(type, ext, nullptr) || (type == 1 && ext == ".m4a");
}

std::string cv_khz(int sr) {
    char buf[24];
    if (sr % 1000 == 0) std::snprintf(buf, sizeof buf, "%dk", sr / 1000);
    else std::snprintf(buf, sizeof buf, "%.1fk", sr / 1000.0);
    return buf;
}
} // namespace

bool App::convert_overlay_active() const {
    return convert_overlay_open_ && mode_ == Mode::Browse;
}

std::string App::conversion_export_dir() const {
    if (!settings_.conversion_export_path.empty()) return settings_.conversion_export_path;
    return path_utf8(cache_.download_dir() / "conversion-export");
}

AudioInfo App::CvState::cv_info_of(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mu);
    auto it = info.find(path);
    return it == info.end() ? AudioInfo{} : it->second;
}

// Every library file except the ones already in the export folder (converting a conversion makes no sense).
void App::convert_overlay_load_files() {
    cv_->files.clear();
    const fs::path out = path_from_utf8(conversion_export_dir()).lexically_normal();
    for (const auto& t : all_local_tracks_) {
        if (fo_is_under(t.path.parent_path().lexically_normal(), out)) continue;
        CvFile f;
        f.path = path_utf8(t.path);
        f.name = truncate_str(path_utf8(t.path.parent_path().filename()), 24) + "/" + path_utf8(t.path.filename());   // "Album/song.flac"
        f.ext = ascii_lower_str(path_utf8(t.path.extension()));
        std::error_code ec;
        const auto sz = fs::file_size(t.path, ec);
        f.size = ec ? 0 : static_cast<long long>(sz);
        cv_->files.push_back(std::move(f));
    }
    std::sort(cv_->files.begin(), cv_->files.end(), [](const CvFile& a, const CvFile& b) {
        return ascii_lower_str(a.path) < ascii_lower_str(b.path);
    });
    // marks of files that are gone
    std::set<std::string> have;
    for (const auto& f : cv_->files) have.insert(f.path);
    for (auto it = cv_->marked.begin(); it != cv_->marked.end();) it = have.count(*it) ? std::next(it) : cv_->marked.erase(it);
}

// (Re)starts the probe thread for the files the current type filter can still let through and that have no result yet.
void App::CvState::convert_overlay_probe_restart() {
    probe_stop = true;
    if (probe_th.joinable()) probe_th.join();
    probe_stop = false;
    std::vector<std::string> todo;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& f : files)
            if (cv_probe_candidate(type, f.ext) && !info.count(f.path)) todo.push_back(f.path);
    }
    probe_total = static_cast<int>(todo.size());
    probe_done = 0;
    if (todo.empty()) return;
    probe_th = std::thread([this, todo = std::move(todo)]() {
        for (const auto& p : todo) {
            if (probe_stop.load()) break;
            AudioInfo a = probe_audio_info(path_from_utf8(p));
            {
                std::lock_guard<std::mutex> lk(mu);
                info[p] = a;   // a failed probe is stored too (probed=false), so it is not asked again
            }
            ++probe_done;
        }
    });
}

void App::convert_overlay_rebuild_view() {
    std::string keep;
    if (cv_->sel >= 0 && cv_->sel < static_cast<int>(cv_->view.size())) keep = cv_->files[static_cast<size_t>(cv_->view[static_cast<size_t>(cv_->sel)])].path;
    cv_->view.clear();

    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : ascii_lower_str(cv_->query)) {
            if (c == ' ') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
    }
    const bool need_probe = cv_->hires || cv_->minkbps > 0;
    std::map<std::string, AudioInfo> info;
    {
        std::lock_guard<std::mutex> lk(cv_->mu);
        info = cv_->info;
    }
    for (size_t i = 0; i < cv_->files.size(); ++i) {
        const CvFile& f = cv_->files[i];
        auto it = info.find(f.path);
        const AudioInfo* inf = it == info.end() ? nullptr : &it->second;
        if (cv_->scope && !cv_->scope_paths.count(f.path)) continue;
        if (!cv_type_ok(cv_->type, f.ext, inf)) continue;
        if (need_probe) {
            if (!inf || !inf->probed) continue;
            if (cv_->hires && !inf->hires) continue;
            if (inf->bitrate_kbps < kCvMinKbps[cv_->minkbps]) continue;
        }
        if (!words.empty()) {
            const std::string hay = ascii_lower_str(f.path);
            bool all = true;
            for (const auto& w : words) if (hay.find(w) == std::string::npos) { all = false; break; }
            if (!all) continue;
        }
        cv_->view.push_back(static_cast<int>(i));
    }
    cv_->sel = 0;
    if (!keep.empty())
        for (size_t i = 0; i < cv_->view.size(); ++i)
            if (cv_->files[static_cast<size_t>(cv_->view[i])].path == keep) { cv_->sel = static_cast<int>(i); break; }
    const int n = cv_->convert_overlay_total();
    cv_->scroll = std::clamp(cv_->scroll, 0, std::max(0, n - std::max(1, cv_->rows)));
}

// The folder / playlist list of the "/f:" / "/p:" modes; rebuilt when the query or the mode changes.
void App::convert_overlay_rebuild_picks() {
    if (cv_->pick == 1) {          // "/f:" -- folders that hold files
        cv_->folders.clear();
        const fs::path out = path_from_utf8(conversion_export_dir()).lexically_normal();
        for (auto& f : filter_folders(cv_->query))
            if (!fo_is_under(path_from_utf8(f.path).lexically_normal(), out)) cv_->folders.push_back(std::move(f));
        cv_->sel = 0;
    } else if (cv_->pick == 2) {   // "/p:" -- saved playlists
        cv_->playlists = filter_playlists(cv_->query);
        cv_->sel = 0;
    }
    cv_->sel = 0;
    cv_->scroll = 0;
}

int App::CvState::convert_overlay_total() const {
    return pick == 1 ? static_cast<int>(folders.size())
         : pick == 2 ? static_cast<int>(playlists.size())
                         : static_cast<int>(view.size());
}

// ENTER on a folder / playlist row of the "/f:" / "/p:" list: from now on the file list only holds the files of that
// folder (the files directly in it, like `f` in the main list) or playlist. ESC shows everything again.
void App::convert_overlay_pick() {
    std::set<std::string> paths;
    std::string label;
    if (cv_->pick == 1) {
        if (cv_->sel < 0 || cv_->sel >= static_cast<int>(cv_->folders.size())) return;
        const FolderSummary& f = cv_->folders[static_cast<size_t>(cv_->sel)];
        for (const auto& file : cv_->files)
            if (path_utf8(path_from_utf8(file.path).parent_path()) == f.path) paths.insert(file.path);
        label = "folder " + f.name;
    } else if (cv_->pick == 2) {
        if (cv_->sel < 0 || cv_->sel >= static_cast<int>(cv_->playlists.size())) return;
        const std::string name = cv_->playlists[static_cast<size_t>(cv_->sel)].name;
        if (auto pl = load_playlist(name))
            for (const auto& t : pl->tracks) if (!t.missing) paths.insert(path_utf8(t.path));
        label = "playlist " + name;
    } else {
        return;
    }
    cv_->scope = true;
    cv_->scope_paths = std::move(paths);
    cv_->scope_label = label;
    cv_->pick = 0;
    cv_->query.clear();
    cv_->search = false;
    cv_->sel = cv_->scroll = 0;
    convert_overlay_rebuild_view();
}

void App::convert_overlay_open() {
    if (folder_overlay_open_) folder_overlay_close();
    if (list_overlay_open_) list_overlay_close();   // the overlays are mutually exclusive
    if (qst_.overlay_open) queue_overlay_close();
    convert_overlay_open_ = true;
    cv_->search = false;
    cv_->query.clear();
    cv_->pick = 0;
    cv_->scope = false;
    cv_->scope_paths.clear();
    cv_->sel = cv_->scroll = 0;
    convert_overlay_load_files();
    cv_->convert_overlay_probe_restart();
    cv_->probe_seen = -1;
    convert_overlay_rebuild_view();
}

void App::convert_overlay_close() {
    convert_overlay_open_ = false;
    cv_->search = false;
    cv_->probe_stop = true;
    if (cv_->probe_th.joinable()) cv_->probe_th.join();
    cv_->probe_stop = false;
}

// Once per frame: probe results re-filter the list; a running conversion reports on the status line (only when the
// numbers change, so other messages are not drowned).
void App::convert_overlay_poll() {
    if (convert_overlay_open_) {
        const int d = cv_->probe_done.load();
        if (d != cv_->probe_seen) {
            cv_->probe_seen = d;
            convert_overlay_rebuild_view();
        }
    }
    const ConvertProgress p = converter_.snapshot();
    if (p.running) {
        const int n = p.done + p.skipped + p.failed;
        if (n != cv_->last_done) {
            cv_->last_done = n;
            status_line_ = "converting " + std::to_string(std::min(n + 1, p.total)) + "/" + std::to_string(p.total) +
                           ": " + p.current + "  (" + hotkey_text("HKeyConvertOverlay", "C") + ": details)";
        }
    } else if (p.finished && cv_->last_done != -2) {
        cv_->last_done = -2;
        status_line_ = std::string(p.cancelled ? "conversion cancelled: " : "conversion done: ") + std::to_string(p.done) + " converted, " +
                       std::to_string(p.skipped) + " skipped, " + std::to_string(p.failed) + " failed -> " + p.out_dir;
        ConsoleLog::instance().log_basic(status_line_);
        for (const auto& e : p.errors) ConsoleLog::instance().log_basic("conversion: " + e);
    }
}

// ENTER: converts the marked files, or the hovered one when nothing is marked.
void App::convert_overlay_start() {
    if (converter_.running()) { status_line_ = "a conversion is running -- [c] cancels it"; return; }
    std::vector<const CvFile*> picked;
    if (!cv_->marked.empty()) {
        for (const auto& f : cv_->files) if (cv_->marked.count(f.path)) picked.push_back(&f);
    } else if (cv_->sel >= 0 && cv_->sel < static_cast<int>(cv_->view.size())) {
        picked.push_back(&cv_->files[static_cast<size_t>(cv_->view[static_cast<size_t>(cv_->sel)])]);
    }
    if (picked.empty()) { status_line_ = "nothing to convert -- mark files with SPACE"; return; }

    const fs::path out = path_from_utf8(conversion_export_dir());
    std::vector<fs::path> roots;
    for (const auto& r : library_roots()) if (!r.empty()) roots.push_back(fo_normal_root(r));
    const std::string ext = convert_preset(cv_->preset).ext;

    std::vector<ConvertItem> items;
    for (const CvFile* f : picked) {
        ConvertItem it;
        it.src = path_from_utf8(f->path);
        // keep the folder structure below the library folder the file lives in
        const fs::path dir = it.src.parent_path().lexically_normal();
        const fs::path* best = nullptr;
        size_t best_len = 0;
        for (const auto& r : roots) {
            if (!fo_is_under(dir, r)) continue;
            size_t n = 0;
            for (auto i = r.begin(); i != r.end(); ++i) ++n;
            if (!best || n > best_len) { best = &r; best_len = n; }
        }
        fs::path rel = best ? dir.lexically_relative(*best) : fs::path(dir.filename());
        if (rel == ".") rel.clear();
        it.dst = out / rel / path_from_utf8(path_utf8(it.src.stem()) + ext);
        const AudioInfo a = cv_->cv_info_of(f->path);
        if (a.probed) { it.duration = a.duration; it.sample_rate = a.sample_rate; it.channels = a.channels; }
        items.push_back(std::move(it));
    }
    const int n = static_cast<int>(items.size());
    cv_->last_done = -1;
    if (converter_.start(std::move(items), cv_->preset, path_utf8(out))) {
        cv_->marked.clear();
        status_line_ = "converting " + std::to_string(n) + (n == 1 ? " file" : " files") + " to " + convert_preset(cv_->preset).label;
        log_event(status_line_ + " -> " + path_utf8(out));
    }
}

bool App::convert_overlay_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int total = cv_->convert_overlay_total();
    const int rows = std::max(1, cv_->rows);
    auto move_to = [&](int idx) { cv_->sel = total > 0 ? std::clamp(idx, 0, total - 1) : 0; };
    auto page = [&](int dir) {
        if (total <= 0) return;
        const int max_scroll = std::max(0, total - rows);
        const int rel = std::clamp(cv_->sel - cv_->scroll, 0, rows - 1);
        const int ns = std::clamp(cv_->scroll + dir * rows, 0, max_scroll);
        if (ns == cv_->scroll) cv_->sel = dir > 0 ? total - 1 : 0;
        else { cv_->scroll = ns; cv_->sel = std::clamp(ns + rel, 0, total - 1); }
    };
    const bool files = cv_->pick == 0;
    const CvFile* cur = (files && cv_->sel >= 0 && cv_->sel < total) ? &cv_->files[static_cast<size_t>(cv_->view[static_cast<size_t>(cv_->sel)])] : nullptr;
    // Back to the plain file search: no folder / playlist list, no query, no scope.
    auto full_search = [&]() {
        cv_->pick = 0;
        cv_->scope = false;
        cv_->scope_paths.clear();
        cv_->query.clear();
        cv_->sel = cv_->scroll = 0;
        convert_overlay_rebuild_view();
    };

    if (arrow && (key == 'A' || key == 'B')) {
        if (last_key_was_shifted()) page(key == 'A' ? -1 : 1);
        else move_to(cv_->sel + (key == 'A' ? -1 : 1));
        return true;
    }
    if (key == kKeyCtrlP) {      // CTRL+p: play the hovered file; the overlay stays open
        if (!cur) return true;
        for (const auto& t : all_local_tracks_)
            if (path_utf8(t.path) == cur->path) { qst_.focus = false; start_local_track(t); return true; }
        return true;
    }
    if (cv_->search) {   // typing the query
        if (key == 27 || key == 9) { cv_->search = false; return true; }
        if (key == '\r' || key == '\n') {
            if (cv_->pick != 0) convert_overlay_pick(); else cv_->search = false;
            return true;
        }
        if (arrow) return true;
        if (key == '/') { full_search(); return true; }   // "/" again: back to the full file search
        if (key == 127 || key == 8) {
            if (cv_->query.empty() && cv_->pick != 0) { cv_->pick = 0; convert_overlay_rebuild_view(); }   // erases the "f:" / "p:"
            else {
                pop_utf8_char(cv_->query);
                if (cv_->pick == 0) convert_overlay_rebuild_view(); else convert_overlay_rebuild_picks();
            }
            return true;
        }
        if (is_text_key(key) && cv_->query.size() < 100) {
            cv_->query += static_cast<char>(key);
            if (cv_->pick == 0 && cv_->query.size() == 2 && cv_->query[1] == ':' && (cv_->query[0] == 'f' || cv_->query[0] == 'F' || cv_->query[0] == 'p' || cv_->query[0] == 'P')) {
                cv_->pick = (cv_->query[0] == 'f' || cv_->query[0] == 'F') ? 1 : 2;   // the bar now reads /f: or /p:
                cv_->query.clear();
                convert_overlay_rebuild_picks();
            } else if (cv_->pick == 0) convert_overlay_rebuild_view();
            else convert_overlay_rebuild_picks();
        }
        return true;
    }
    if (key == 27) {   // ESC: leave the folder / playlist list, then the scope, then the overlay
        if (cv_->pick != 0) { cv_->pick = 0; cv_->query.clear(); cv_->sel = cv_->scroll = 0; convert_overlay_rebuild_view(); }
        else if (cv_->scope) { cv_->scope = false; cv_->scope_paths.clear(); cv_->sel = cv_->scroll = 0; convert_overlay_rebuild_view(); }
        else convert_overlay_close();
        return true;
    }
    if (!arrow && (key == '/' || key == 9)) { cv_->search = true; return true; }

    if (arrow && (key == 'C' || key == 'D')) {   // LEFT / RIGHT: the target
        const int n = convert_preset_count();
        cv_->preset = (cv_->preset + (key == 'C' ? 1 : n - 1)) % n;
        return true;
    }
    if (!arrow) {
        if (key == ' ' && files) {   // mark / unmark, then on to the next row
            if (cur) {
                if (!cv_->marked.erase(cur->path)) cv_->marked.insert(cur->path);
                move_to(cv_->sel + 1);
            }
            return true;
        }
        if (key == 'a' && files) {   // everything shown: mark all, or unmark all when all are marked already
            bool all = total > 0;
            for (int idx : cv_->view) if (!cv_->marked.count(cv_->files[static_cast<size_t>(idx)].path)) { all = false; break; }
            for (int idx : cv_->view) {
                const std::string& p = cv_->files[static_cast<size_t>(idx)].path;
                if (all) cv_->marked.erase(p); else cv_->marked.insert(p);
            }
            return true;
        }
        if (key == 't' || key == 'T') {
            cv_->type = (cv_->type + (key == 't' ? 1 : kCvTypeCount - 1)) % kCvTypeCount;
            cv_->convert_overlay_probe_restart();
            cv_->probe_seen = -1;
            convert_overlay_rebuild_view();
            return true;
        }
        if (key == 'r' || key == 'R') {
            cv_->minkbps = (cv_->minkbps + (key == 'r' ? 1 : kCvMinCount - 1)) % kCvMinCount;
            convert_overlay_rebuild_view();
            return true;
        }
        if (key == 'h' || key == 'H') { cv_->hires = !cv_->hires; convert_overlay_rebuild_view(); return true; }
        if (key == 'c') {
            if (converter_.running()) { converter_.cancel(); status_line_ = "cancelling the conversion ..."; }
            return true;
        }
    }
    if (key == '\r' || key == '\n') {
        if (cv_->pick != 0) convert_overlay_pick(); else convert_overlay_start();
        return true;
    }

    // The rest goes through the hotkeys: the overlay key itself closes, the playback keys fall through to the normal
    // dispatch, anything that would act on the hidden list is swallowed.
    int lookup = (key == '\r') ? '\n' : key;
    std::string action = resolve_hotkey_action(lookup);
    if (action.empty() && lookup >= 'a' && lookup <= 'z') action = resolve_hotkey_action(lookup - 32);
    if (action.empty() && lookup >= 'A' && lookup <= 'Z') action = resolve_hotkey_action(lookup + 32);
    if (action == "HKeyConvertOverlay") { convert_overlay_close(); return true; }
    static const char* const kPass[] = {
        "HKeyTogglePlayPause", "HKeyPlayNextSong", "HKeyPlayPreviousSong", "HKeyShuffleNext", "HKeyCyclePlayMode", "HKeyCyclePlayModeBack",
        "HKeySeekForward", "HKeySeekBackward", "HKeyIncreaseVolume", "HKeyDecreaseVolume", "HKeyToggleMute",
        "HKeyToggleNormalize", "HKeyQuit"};
    for (const char* a : kPass) if (action == a) return false;
    return true;
}

void App::convert_overlay_geometry(int W, int& panel_w, int& rows) const {
    panel_w = std::min(W, std::max(40, std::min(W - 6, 160)));
    const int legend = static_cast<int>(convert_overlay_legend(panel_w).size());
    rows = std::max(1, term_rows_ - 2 + overlay_legend_shift(legend) - kListOverlayChromeRows - legend - 2);   // 2 rows of the box hold the filter and status lines
}

std::vector<LegendRow> App::convert_overlay_legend(int panel_w) const {
    const int max_lines = std::clamp(term_rows_ - kListOverlayChromeRows - 5, 1, 3);
    std::vector<LegendGroup> groups = {
        {"Convert", {"[SPACE] mark", "[a] mark all", "[ENTER] convert", "[c] cancel"}},
        {"Options", {"[\u2190\u2192] target", "[t] type", "[r] min kbps", "[h] hi-res"}},
        {"Other", {"[/ , /p: , /f:] full or filtered path search\001search", "[CTRL+p] play", "[SHIFT+\u2191\u2193] page", "[ESC] close"}},
    };
    return layout_legend_tree(groups, std::max(0, panel_w), max_lines);
}

std::vector<std::string> App::build_convert_overlay_panel(int panel_w, int list_rows) const {
    const std::string border_ansi = ansi_for(settings_.border_color, false);
    const std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    const std::string R = "\x1b[0m";
    std::vector<std::string> out;

    // --- search bar (same look as the main one) ---
    {
        const int search_w = panel_w - 5;
        out.push_back(box_top("SEARCH FILES TO CONVERT", search_w, border_ansi) + border_ansi + "╭───╮\x1b[0m");
        const int inner = std::max(0, search_w - 4);
        const std::string pre = cv_->pick == 1 ? "/f:" : cv_->pick == 2 ? "/p:" : "/";
        std::string q = cv_->query;
        const int room = std::max(0, inner - display_width(pre) - (cv_->search ? 1 : 0));
        if (display_width(q) > room) q = truncate_str(q, room);
        const int fill = std::max(0, inner - display_width(pre) - display_width(q) - (cv_->search ? 1 : 0));
        std::string line = bar + " " + search_idle_sgr(settings_) + pre + search_run(settings_, q, !q.empty())
                         + (cv_->search ? search_idle_sgr(settings_) + "█" : std::string())
                         + std::string(static_cast<size_t>(fill), ' ') + R + " " + bar;
        out.push_back(line + border_ansi + settings_.box_vertical + " " + std::string(1, play_mode_letter()) + " " + settings_.box_vertical + R);
        out.push_back(box_bottom(search_w, "", border_ansi_bottom) + border_ansi_bottom + "╰───╯\x1b[0m");
    }

    const int inner = panel_w - 4;
    const int total = cv_->convert_overlay_total();
    const ConvertProgress prog = converter_.snapshot();
    {
        std::string title = cv_->pick == 1 ? "FOLDERS (ENTER: show its files)" : cv_->pick == 2 ? "PLAYLISTS (ENTER: show its files)" : "FILES TO CONVERT";
        if (cv_->pick == 0 && cv_->scope) title += ": " + truncate_str(cv_->scope_label, 40);
        out.push_back(box_top(title, panel_w, border_ansi));
    }
    auto boxed = [&](const std::string& text, const std::string& sgr) {
        out.push_back(bar + " " + sgr + pad_right(truncate_str(text, inner), inner) + R + " " + bar);
    };

    // row 0: the pre-filter and the target
    boxed("TYPE [t] " + std::string(kCvTypeNames[cv_->type]) + "   MIN KBPS [r] " +
              (kCvMinKbps[cv_->minkbps] == 0 ? std::string("any") : std::to_string(kCvMinKbps[cv_->minkbps])) +
              "   HI-RES [h] " + (cv_->hires ? "on" : "off") + "   TARGET [←→] " + convert_preset(cv_->preset).label,
          header_sgr(settings_));
    // row 1: progress / result
    {
        std::string text;
        if (prog.running) {
            const int n = prog.done + prog.skipped + prog.failed;
            const int w = 20;
            const int fill = static_cast<int>(prog.fraction * w + 0.5);
            text = "CONVERTING " + std::to_string(std::min(n + 1, prog.total)) + "/" + std::to_string(prog.total) + "  [" +
                   std::string(static_cast<size_t>(fill), '#') + std::string(static_cast<size_t>(w - fill), '-') + "] " +
                   std::to_string(static_cast<int>(prog.fraction * 100)) + "%  " + prog.current + "   [c] cancel";
        } else if (prog.finished) {
            text = std::string(prog.cancelled ? "CANCELLED: " : "DONE: ") + std::to_string(prog.done) + " converted, " +
                   std::to_string(prog.skipped) + " skipped (already there), " + std::to_string(prog.failed) + " failed  ->  " + prog.out_dir;
            if (!prog.errors.empty()) text += "   first error: " + prog.errors.front();
        } else if (cv_->pick != 0) {
            text = std::string("Type to filter the ") + (cv_->pick == 1 ? "folders" : "playlists") + ", ENTER shows the files of the one under the cursor, / returns to the full search";
        } else if (cv_->scope) {
            text = "Only the files of " + cv_->scope_label + ": " + std::to_string(cv_->scope_paths.size()) + " in the library, " +
                   std::to_string(cv_->view.size()) + " match the pre-filter   [ESC] shows everything again";
        } else {
            text = "Export folder: " + conversion_export_dir() + "   (Settings -> PATHS -> CONVERSION EXPORT PATH)";
        }
        boxed(text, legend_sgr(settings_));
    }

    // the files
    for (int row = 0; row < list_rows; ++row) {
        const int idx = cv_->scroll + row;
        std::string content;
        if (idx < total && cv_->pick != 0) {
            std::string label, count;
            if (cv_->pick == 1) {
                const FolderSummary& f = cv_->folders[static_cast<size_t>(idx)];
                label = (f.parent.empty() ? std::string() : f.parent + "/") + f.name;
                count = std::to_string(f.track_count) + (f.track_count == 1 ? " file" : " files");
            } else {
                const PlaylistSummary& pl = cv_->playlists[static_cast<size_t>(idx)];
                label = pl.name;
                count = std::to_string(pl.track_count) + (pl.track_count == 1 ? " track" : " tracks");
            }
            const int name_w = std::max(5, inner - 2 - static_cast<int>(count.size()));
            content = pad_right(truncate_str(apply_font_map(label, settings_.font_map), name_w), name_w) + "  " + count;
        } else if (idx < total) {
            const CvFile& f = cv_->files[static_cast<size_t>(cv_->view[static_cast<size_t>(idx)])];
            const AudioInfo a = cv_->cv_info_of(f.path);
            std::string fmt = f.ext.size() > 1 ? f.ext.substr(1) : f.ext;
            for (auto& c : fmt) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            std::string spec = "…", kbps = "…", est;
            if (a.probed) {
                spec = (a.bits > 0 ? std::to_string(a.bits) + "/" : std::string()) + cv_khz(a.sample_rate);
                kbps = std::to_string(a.bitrate_kbps) + " kbps";
                if (a.duration > 0) est = "-> ~" + cv_size_str(convert_estimate_bytes(cv_->preset, a.duration));
            }
            const std::string right = pad_right(fmt, 5) + " " + pad_right(spec, 9) + " " + pad_left(kbps, 9) + " " +
                                      pad_left(cv_size_str(f.size), 9) + "  " + pad_right(est, 12);
            const std::string head = std::string(cv_->marked.count(f.path) ? "[x] " : "[ ] ");
            const int name_w = std::max(5, inner - display_width(head) - display_width(right) - 1);
            content = head + pad_right(truncate_str(apply_font_map(f.name, settings_.font_map), name_w), name_w) + " " + right;
        }
        const std::string padded = pad_right(truncate_str(content, inner), inner);
        const bool playing = idx < total && cv_->pick == 0 && has_track_ && path_utf8(current_.path) == cv_->files[static_cast<size_t>(cv_->view[static_cast<size_t>(idx)])].path;
        if (idx == cv_->sel && idx < total)
            out.push_back(bar + " " + cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + padded + R + " " + bar);
        else if (playing)
            out.push_back(bar + " " + ansi_for(settings_.list_playing_color) + bg_ansi_for(settings_.list_playing_bg_color) + padded + R + " " + bar);
        else
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color) + padded + R + " " + bar);
    }
    {
        const int rows = std::max(1, list_rows);
        const int pages = std::max(1, (total + rows - 1) / rows);
        const int page = std::clamp((cv_->scroll + rows / 2) / rows + 1, 1, pages);
        const int remaining = total - (cv_->scroll + rows);
        std::vector<std::string> parts;
        if (!cv_->marked.empty()) parts.push_back(std::to_string(cv_->marked.size()) + " marked");
        if (cv_->probe_total > 0 && cv_->probe_done.load() < cv_->probe_total)
            parts.push_back("reading " + std::to_string(cv_->probe_done.load()) + "/" + std::to_string(cv_->probe_total));
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
        const auto lrows = convert_overlay_legend(panel_w);
        for (size_t i = 0; i < lrows.size(); ++i) out.push_back(paint_legend(settings_, lrows, i, border_ansi, panel_w, false));
    }
    return out;
}

} // namespace muisc
