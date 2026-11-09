// READ LYRICS overlay (stored lyrics versions, translations, search). Split out of app.cpp.
// Member functions of App (declared in app.h); the helpers shared with app.cpp are in app_internal.h.
#include "app.h"
#include "readlyrics_state.h"
#include "text_edit.h"
#include "overlay_common.h"
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
#include <regex>
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

// Part of app_readlyrics.cpp, moved to its own file so that the two compile in parallel (the same App class, nothing else changed).
void App::readlyrics_add_open() {
    // The current track: the one the reader shows (when it is a real track), else the one that plays.
    std::error_code ec;
    rd_->add_cur.clear();
    rd_->add_cur_title.clear();
    if (!rd_->opened_hit.path.empty()) {
        // A lyrics file was opened from the search: the song it belongs to (the track named like it, preferably in the folder
        // above its lyrics folder) is the current one. Nothing else stands in for it: the playing song is only the current one
        // when no lyrics file was opened.
        const fs::path home = rd_->opened_hit.path.parent_path().parent_path();
        const LocalTrack* found = nullptr;
        for (const LocalTrack& t : all_local_tracks_) {
            if (ascii_lower_str(path_utf8(t.path.stem())) != rd_->opened_hit.lname) continue;
            if (!found) found = &t;
            if (t.path.parent_path() == home) { found = &t; break; }
        }
        if (found) { rd_->add_cur = found->path; rd_->add_cur_title = readlyrics_add_track_text(*found); }
    }
    else if (!rd_->track.empty() && fs::exists(rd_->track, ec)) { rd_->add_cur = rd_->track; rd_->add_cur_title = rd_->title; }
    else if (has_track_ && !current_path_.empty()) {
        rd_->add_cur = current_path_;
        rd_->add_cur_title = (metadata_.artist == "-" || metadata_.artist.empty()) ? metadata_.name : metadata_.artist + " - " + metadata_.name;
    }
    // Every lyrics file below the "lyrics" folders of the library (subfolders too; the translations and exports are not lyrics).
    std::set<std::string> dirs;
    auto add_dir = [&](const fs::path& folder) { if (!folder.empty()) dirs.insert(path_utf8(folder / "lyrics")); };
    for (const auto& t : all_local_tracks_) add_dir(t.path.parent_path());
    for (const auto& p : settings_.local_music_paths) add_dir(path_from_utf8(p));
    if (!rd_->add_cur.empty()) add_dir(rd_->add_cur.parent_path());
    rd_->add_files.clear();
    rd_->add_dirs = 0;
    for (const auto& d : dirs) {
        const fs::path dir = path_from_utf8(d);
        if (!fs::is_directory(dir, ec)) continue;
        ++rd_->add_dirs;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2)) {
                const std::string n = ascii_lower_str(path_utf8(it->path().filename()));
                if (n == "translations" || n == "export") it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(e2)) continue;
            const std::string ext = ascii_lower_str(path_utf8(it->path().extension()));
            if (ext != ".lrc" && ext != ".txt") continue;
            static const std::regex archived_re(R"(\.v\d{8}-\d{6}\.)");   // the app's own older copies are not lyrics files to pick
            if (std::regex_search(path_utf8(it->path().filename()), archived_re)) continue;
            LyricsFileHit h;
            h.path = it->path();
            h.name = path_utf8(it->path().stem());
            h.lname = ascii_lower_str(h.name);
            std::error_code e3;
            const fs::path rel = fs::relative(it->path().parent_path(), dir.parent_path(), e3);
            h.folder = e3 ? path_utf8(dir.parent_path().filename()) : path_utf8(rel);
            rd_->add_files.push_back(std::move(h));
            if (rd_->add_files.size() >= 200000) break;
        }
    }
    std::sort(rd_->add_files.begin(), rd_->add_files.end(), [](const LyricsFileHit& a, const LyricsFileHit& b) {
        return a.lname != b.lname ? a.lname < b.lname : a.folder < b.folder;
    });
    rd_->add_fq.clear();
    rd_->add_tq.clear();
    rd_->add_fsel = rd_->add_tsel = 0;
    rd_->add_pane = 0;
    rd_->add_msg.clear();
    rd_->add_warn = false;
    rd_->add_confirm = false;
    rd_->add_tprev_ok = rd_->add_fprev_ok = false;   // (a fresh search; the texts below are made again for the library as it is now)
    rd_->add_thay.clear();
    rd_->add_lock_file = LyricsFileHit();
    rd_->add_lock_auto = false;
    rd_->add_lock_cur = true;
    rd_->add_lock_track.clear();
    if (!rd_->opened_hit.path.empty()) rd_->add_lock_file = rd_->opened_hit;   // the lyrics that are open are the ones to add
    readlyrics_add_rebuild();
    readlyrics_add_lock_refresh();
    rd_->add_open = true;
    force_redraw_ = true;
}

// From the re-fetch form and the karaoke screen: READ LYRICS for the playing track, with the overlay open at once.
void App::readlyrics_add_open_from_playing() {
    if (!has_track_ || current_path_.empty()) { status_line_ = "add lyrics manually: play a track first, or open READ LYRICS on one"; return; }
    rl_from_reader_ = rl_from_karaoke_ = false;
    readlyrics_open();
    if (mode_ != Mode::ReadLyrics) return;
    readlyrics_add_open();
}

void App::readlyrics_add_rebuild() {
    auto words_of = [](const std::string& q) {
        std::vector<std::string> w;
        std::istringstream iss(ascii_lower_str(q));
        std::string x;
        while (iss >> x) w.push_back(x);
        return w;
    };
    // files
    rd_->add_typed = LyricsFileHit();
    {
        std::string q = rd_->add_fq;
        while (!q.empty() && (q.back() == ' ' || q.back() == '"' || q.back() == '\'')) q.pop_back();
        while (!q.empty() && (q.front() == ' ' || q.front() == '"' || q.front() == '\'')) q.erase(0, 1);
        if (!q.empty() && (q.find('/') != std::string::npos || q.find('\\') != std::string::npos)) {
            std::error_code ec;
            const fs::path p = path_from_utf8(q);
            const std::string ext = ascii_lower_str(path_utf8(p.extension()));
            if ((ext == ".lrc" || ext == ".txt") && fs::is_regular_file(p, ec)) {
                rd_->add_typed.path = p;
                rd_->add_typed.name = path_utf8(p.stem());
                rd_->add_typed.lname = ascii_lower_str(rd_->add_typed.name);
                rd_->add_typed.folder = "typed path";
            }
        }
    }
    // files: a query that continues the one before is looked for among the matches of the one before
    const auto fw = words_of(rd_->add_fq);
    std::vector<int> fcand;
    const bool f_narrow = rd_->add_fprev_ok && rd_->add_fq.compare(0, rd_->add_fprev_q.size(), rd_->add_fprev_q) == 0;
    if (f_narrow) { for (int v : rd_->add_fview) if (v >= 0) fcand.push_back(v); }
    else { fcand.resize(rd_->add_files.size()); for (size_t i = 0; i < fcand.size(); ++i) fcand[i] = static_cast<int>(i); }
    rd_->add_fview.clear();
    if (!rd_->add_typed.path.empty()) rd_->add_fview.push_back(-1);
    for (int i : fcand) {
        bool ok = true;
        for (const auto& w : fw) if (rd_->add_files[static_cast<size_t>(i)].lname.find(w) == std::string::npos) { ok = false; break; }
        if (ok) rd_->add_fview.push_back(i);
    }
    rd_->add_fprev_q = rd_->add_fq;
    rd_->add_fprev_ok = true;
    rd_->add_fsel = std::clamp(rd_->add_fsel, 0, std::max(0, static_cast<int>(rd_->add_fview.size()) - 1));
    // tracks: the current track is always the first entry
    const auto tw = words_of(rd_->add_tq);
    if (rd_->add_nolyr && rd_->add_has_lyr.size() != all_local_tracks_.size()) {   // which tracks have saved lyrics (one look at each lyrics folder entry)
        rd_->add_has_lyr.assign(all_local_tracks_.size(), 0);
        for (size_t i = 0; i < all_local_tracks_.size(); ++i) rd_->add_has_lyr[i] = lyrics_copy_in_use(all_local_tracks_[i].path).empty() ? 0 : 1;
    }
    if (rd_->add_thay.size() != all_local_tracks_.size() || rd_->add_thay_names != rd_->add_names) {   // the searched texts, made once
        rd_->add_thay.resize(all_local_tracks_.size());
        for (size_t i = 0; i < all_local_tracks_.size(); ++i) {
            const LocalTrack& t = all_local_tracks_[i];
            rd_->add_thay[i] = ascii_lower_str(rd_->add_names ? path_utf8(t.path.filename()) : t.folder_artist + " " + t.title);
        }
        rd_->add_thay_names = rd_->add_names;
        rd_->add_tprev_ok = false;
    }
    const int flags = (rd_->add_names ? 2 : 0) | (rd_->add_nolyr ? 1 : 0);
    std::vector<int> tcand;
    const bool t_narrow = rd_->add_tprev_ok && flags == rd_->add_tprev_flags && rd_->add_tq.compare(0, rd_->add_tprev_q.size(), rd_->add_tprev_q) == 0;
    if (t_narrow) { for (int v : rd_->add_tview) if (v >= 0) tcand.push_back(v); }
    else { tcand.resize(all_local_tracks_.size()); for (size_t i = 0; i < tcand.size(); ++i) tcand[i] = static_cast<int>(i); }
    rd_->add_tview.assign(1, -1);
    for (int i : tcand) {
        if (rd_->add_nolyr && rd_->add_has_lyr[static_cast<size_t>(i)]) continue;
        const std::string& hay = rd_->add_thay[static_cast<size_t>(i)];
        bool ok = true;
        for (const auto& w : tw) if (hay.find(w) == std::string::npos) { ok = false; break; }
        if (ok) rd_->add_tview.push_back(i);
    }
    rd_->add_tprev_q = rd_->add_tq;
    rd_->add_tprev_flags = flags;
    rd_->add_tprev_ok = true;
    rd_->add_tsel = std::clamp(rd_->add_tsel, 0, static_cast<int>(rd_->add_tview.size()) - 1);
}

fs::path App::readlyrics_add_target() const {   // the locked track
    return rd_->add_lock_cur ? rd_->add_cur : rd_->add_lock_track;
}

void App::readlyrics_add_lock_refresh() {
    const fs::path t = readlyrics_add_target();
    rd_->add_lock_info = t.empty() ? std::string() : lyrics_in_use_summary(t);
    rd_->add_manual = !t.empty() && lyrics_added_manually(t);
    // A lyrics file that was given to the track by hand shows as locked in (until another one is locked by hand).
    if (rd_->add_manual && (rd_->add_lock_file.path.empty() || rd_->add_lock_auto)) {
        LyricsFileHit h;
        h.path = lyrics_copy_in_use(t);
        rd_->add_orig_name = lyrics_manual_origin_name(t);
        h.name = (rd_->add_show_orig && !rd_->add_orig_name.empty()) ? rd_->add_orig_name : path_utf8(h.path.filename());
        h.lname = ascii_lower_str(h.name);
        h.folder = "lyrics";
        rd_->add_lock_file = h;
        rd_->add_lock_auto = true;
    } else if (rd_->add_lock_auto) {
        rd_->add_orig_name.clear();
        rd_->add_lock_file = LyricsFileHit();
        rd_->add_lock_auto = false;
    }
}

void App::readlyrics_add_clear() {   // forgets the locked lyrics file and the locked track (the current track is locked in again)
    rd_->add_lock_file = LyricsFileHit();
    rd_->add_lock_auto = false;
    rd_->add_lock_cur = true;
    rd_->add_lock_track.clear();
    rd_->add_lock_title.clear();
    rd_->add_warn = false;
    rd_->add_msg = "choice cleared -- the current track is locked in again, lock a lyrics file";
    readlyrics_add_lock_refresh();
}

void App::readlyrics_add_unassign() {
    const fs::path target = readlyrics_add_target();
    rd_->add_warn = true;
    rd_->add_confirm = false;
    if (target.empty()) { rd_->add_msg = "there is no track chosen"; return; }
    std::string err;
    if (!unassign_lyrics_file(target, &err)) { rd_->add_msg = err; return; }
    log_event("lyrics: unassigned the manually added lyrics of " + path_utf8(target.filename()));
    if (has_track_ && target == current_path_) {
        std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
        launch_lyrics_fetch(metadata_.name, artist, current_path_);   // loads whatever is in use now (no network)
    }
    if (target == rd_->track) {
        rd_->versions = list_lyrics_versions(rd_->track);
        rd_->sel = 0;
        for (size_t i = 0; i < rd_->versions.size(); ++i) if (rd_->versions[i].active) { rd_->sel = static_cast<int>(i); break; }
        rd_->first = 0;
        readlyrics_load_version();
    }
    rd_->add_has_lyr.clear();   // (the track has no lyrics copy of its own now, perhaps)
    readlyrics_add_rebuild();
    readlyrics_add_lock_refresh();
    rd_->add_warn = false;
    rd_->add_msg = "unassigned -- the lyrics file itself is untouched";
}

void App::readlyrics_add_key(int key) {
    const bool arrow = last_key_was_arrow();
    struct Refresh { App* a; ~Refresh() { if (a->rd_->add_open) a->readlyrics_add_lock_refresh(); } } refresh{this};
    if (key == kKeyDelete) {   // DEL: unassign (twice, the second one confirms)
        if (!lyrics_added_manually(readlyrics_add_target())) { rd_->add_warn = true; rd_->add_confirm = false; rd_->add_msg = "the lyrics of the locked track were not added manually -- nothing to unassign"; return; }
        if (!rd_->add_confirm) { rd_->add_confirm = true; rd_->add_warn = true; rd_->add_msg = "unassign the manually added lyrics of this track? [DEL] again confirms, anything else cancels"; return; }
        readlyrics_add_unassign();
        return;
    }
    rd_->add_confirm = false;
    if (key == 27) { rd_->add_open = false; force_redraw_ = true; return; }
    if (!arrow && key == '\t') { rd_->add_pane = (rd_->add_pane + 1) % 4; rd_->add_msg.clear(); return; }   // file list, track list, the two buttons
    if (key == '\r' || key == '\n') {   // ENTER locks the highlighted lyrics file / track in; on the button it adds
        rd_->add_warn = false;
        if (rd_->add_pane == 2) { readlyrics_add_do(); return; }
        if (rd_->add_pane == 3) { readlyrics_add_clear(); return; }
        if (rd_->add_pane == 0) {
            if (rd_->add_fview.empty()) return;
            const int fi = rd_->add_fview[static_cast<size_t>(rd_->add_fsel)];
            rd_->add_lock_file = fi < 0 ? rd_->add_typed : rd_->add_files[static_cast<size_t>(fi)];
            rd_->add_lock_auto = false;
            rd_->add_msg = "locked lyrics file: " + rd_->add_lock_file.name;
        } else {
            const int ti = rd_->add_tview[static_cast<size_t>(rd_->add_tsel)];
            if (ti < 0) { rd_->add_lock_cur = true; rd_->add_msg = "locked track: the current track"; }
            else {
                rd_->add_lock_cur = false;
                rd_->add_lock_track = all_local_tracks_[static_cast<size_t>(ti)].path;
                rd_->add_lock_title = readlyrics_add_track_text(all_local_tracks_[static_cast<size_t>(ti)]);
                rd_->add_msg = "locked track: " + rd_->add_lock_title;
            }
        }
        return;
    }
    // CTRL+c clears the choice (from anywhere, also in the search fields)
    if (key == kKeyCtrlC) { readlyrics_add_clear(); return; }
    // CTRL+t: the track list shows file names / "folder - title"; CTRL+f: only the tracks without saved lyrics (both work in the search fields)
    if (key == kKeyCtrlF) {
        rd_->add_nolyr = !rd_->add_nolyr;
        rd_->add_has_lyr.clear();
        rd_->add_warn = false;
        rd_->add_tsel = 0;
        rd_->add_msg = rd_->add_nolyr ? "the track list shows only the tracks without saved lyrics" : "the track list shows all tracks";
        readlyrics_add_rebuild();
        return;
    }
    if (key == kKeyCtrlO) {   // the locked-in lyrics file: the name it was picked with / the name of the copy in the lyrics folder
        rd_->add_show_orig = !rd_->add_show_orig;
        readlyrics_add_lock_refresh();
        rd_->add_warn = false;
        rd_->add_msg = rd_->add_show_orig ? "locked-in lyrics file: shown by the name of the file it came from" : "locked-in lyrics file: shown by the name of the copy in the lyrics folder";
        return;
    }
    if (key == kKeyCtrlT) {
        rd_->add_names = !rd_->add_names;
        rd_->add_warn = false;
        rd_->add_msg = rd_->add_names ? "the track list shows file names" : "the track list shows artist - title";
        readlyrics_add_rebuild();
        return;
    }
    if (arrow && key == 'D' && rd_->add_pane == 1) { rd_->add_pane = 0; return; }
    if (arrow && key == 'C' && rd_->add_pane == 0) { rd_->add_pane = 1; return; }
    if (rd_->add_pane >= 2) return;   // (nothing else to do on a button)
    int& sel = rd_->add_pane == 0 ? rd_->add_fsel : rd_->add_tsel;
    std::string& q = rd_->add_pane == 0 ? rd_->add_fq : rd_->add_tq;
    const int n = rd_->add_pane == 0 ? static_cast<int>(rd_->add_fview.size()) : static_cast<int>(rd_->add_tview.size());
    const int page = readlyrics_add_list_rows();   // SHIFT+up / down and PgUp / PgDn (reported alike) turn a page
    if (arrow && key == 'A') { sel = std::max(0, sel - (last_key_was_shifted() ? page : 1)); return; }
    if (arrow && key == 'B') { sel = std::min(std::max(0, n - 1), sel + (last_key_was_shifted() ? page : 1)); return; }
    if (arrow) return;
    if (key == kKeyHome) { sel = 0; return; }
    if (key == kKeyEnd) { sel = std::max(0, n - 1); return; }
    if (key == 127 || key == 8) { pop_utf8_char(q); sel = 0; rd_->add_msg.clear(); readlyrics_add_rebuild(); return; }
    if (is_text_key(key) && q.size() < 300) {
        q += static_cast<char>(key);
        sel = 0;
        rd_->add_msg.clear();
        readlyrics_add_rebuild();
    }
}

void App::readlyrics_add_do() {
    rd_->add_warn = true;
    if (rd_->add_lock_file.path.empty()) { rd_->add_msg = "lock a lyrics file first: highlight it in the left list and press [ENTER]"; return; }
    const LyricsFileHit file = rd_->add_lock_file;
    const fs::path target = readlyrics_add_target();
    const std::string target_title = rd_->add_lock_cur ? rd_->add_cur_title : rd_->add_lock_title;
    if (target.empty()) { rd_->add_msg = "there is no current track -- lock a track from the right list"; return; }
    std::string err;
    if (!assign_lyrics_file(target, file.path, &err)) { rd_->add_msg = err; return; }
    log_event("lyrics: assigned " + path_utf8(file.path.filename()) + " to " + target_title);
    rd_->add_open = false;
    force_redraw_ = true;
    if (has_track_ && target == current_path_) {   // the playing track: its lyrics load now
        std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
        launch_lyrics_fetch(metadata_.name, artist, current_path_);   // loads whatever is in use now (no network)
    }
    if (target == rd_->track) {   // the reader shows this track: the new copy is the one to see
        rd_->versions = list_lyrics_versions(rd_->track);
        rd_->sel = 0;
        for (size_t i = 0; i < rd_->versions.size(); ++i) if (rd_->versions[i].active) { rd_->sel = static_cast<int>(i); break; }
        rd_->first = 0;
        rd_->pane = 0;
        readlyrics_load_version();
    }
    rd_->status = "assigned \"" + path_utf8(file.path.filename()) + "\" to " + target_title + (target == rd_->track ? "" : " -- open READ LYRICS on that track to read it");
    rd_->status_warn = false;
}

// The panel is a full-window screen: W columns wide and term_rows_ - 1 rows high (what a full-screen frame may use). Inside, every
// part has its own bordered pane: the two search fields, the two result lists (with a count line each) and the two locked-in fields.
std::vector<std::string> App::build_readlyrics_add_panel(int W) const {
    const int PW = std::max(70, W);
    const int pw_l = (PW - 3) / 2, pw_r = PW - 3 - pw_l;   // pane widths, a space between them, flush against the outer border
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const std::string hdr = header_sgr(settings_);
    const std::string leg = legend_sgr(settings_);
    std::vector<std::string> out;
    out.push_back(box_top("ADD LYRICS MANUALLY", PW, border));
    // one line of a pane: `content` is already painted and `used` columns wide; it is padded to the pane's inner width
    auto pline = [&](const std::string& content, int used, int w) {
        return bar + " " + content + std::string(static_cast<size_t>(std::max(0, w - 4 - used)), ' ') + " " + bar;
    };
    auto both = [&](const std::string& l, const std::string& r) { out.push_back(bar + l + " " + r + bar); };
    auto cut = [&](const std::string& t, int w, const std::string& sgr = std::string()) {   // text cut to the pane, painted
        const std::string c = truncate_str(t, w - 4);
        return std::make_pair(sgr.empty() ? c : sgr + c + R, display_width(c));
    };
    auto title = [&](int which, const std::string& t) { return t + (rd_->add_pane == which ? focus_indicator(settings_, border) : std::string()); };
    // --- the two search fields
    auto search_line = [&](int which, const std::string& q, int w) {
        const bool focus = rd_->add_pane == which;
        const int room = std::max(1, w - 4 - 1);
        std::string shown = q;
        while (display_width(shown) > room && !shown.empty()) {   // a long query keeps its END visible
            size_t i = 1;
            while (i < shown.size() && (static_cast<unsigned char>(shown[i]) & 0xC0) == 0x80) ++i;
            shown.erase(0, i);
        }
        return pline(shown + (focus ? "\x1b[7m \x1b[0m" : " "), display_width(shown) + 1, w);
    };
    both(box_top(title(0, "SEARCH LYRICS FILES"), pw_l, border), box_top(title(1, "SEARCH TRACKS"), pw_r, border));
    both(search_line(0, rd_->add_fq, pw_l), search_line(1, rd_->add_tq, pw_r));
    both(box_bottom(pw_l, "", border), box_bottom(pw_r, "", border));
    // --- the two result lists: whatever height is left
    const int list_rows = readlyrics_add_list_rows();
    const int nf = static_cast<int>(rd_->add_fview.size());
    const int nt = static_cast<int>(rd_->add_tview.size());
    auto cell = [&](int which, int w, int r, int sel, int n, const std::function<std::string(int)>& text, const std::function<bool(int)>& locked, const std::string& empty_text) {
        const bool focus = rd_->add_pane == which;
        const int cw = w - 4;
        int first = 0;
        if (sel >= list_rows) first = sel - list_rows + 1;
        const int i = first + r;
        std::string painted;
        if (i < n) {
            const bool lk = locked(i);
            const std::string line = pad_right(truncate_str((lk ? "\xC2\xBB " : "  ") + text(i), cw), cw);   // » marks the locked one
            if (i == sel && focus) painted = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + line + R;
            else if (lk) painted = hdr + line + R;
            else painted = line;
            return pline(painted, cw, w);
        }
        if (r == 0 && n == 0) return pline(pad_right(truncate_str(empty_text, cw), cw), cw, w);
        return pline(std::string(), 0, w);
    };
    both(box_top(title(0, "LYRICS FILES"), pw_l, border), box_top(title(1, std::string("TRACKS (") + (rd_->add_names ? "file names" : "artist - title") + (rd_->add_nolyr ? ", no lyrics" : "") + ")"), pw_r, border));
    const std::string fempty = rd_->add_files.empty() ? "no lyrics files found -- type a path" : "no match -- or type a full path";
    for (int r = 0; r < list_rows; ++r) {
        const std::string l = cell(0, pw_l, r, rd_->add_fsel, nf, [&](int i) {
            const int fi = rd_->add_fview[static_cast<size_t>(i)];
            const LyricsFileHit& h = fi < 0 ? rd_->add_typed : rd_->add_files[static_cast<size_t>(fi)];
            return h.name + "  (" + h.folder + ")";
        }, [&](int i) {
            const int fi = rd_->add_fview[static_cast<size_t>(i)];
            const LyricsFileHit& h = fi < 0 ? rd_->add_typed : rd_->add_files[static_cast<size_t>(fi)];
            return !rd_->add_lock_file.path.empty() && h.path == rd_->add_lock_file.path;
        }, fempty);
        const std::string rr = cell(1, pw_r, r, rd_->add_tsel, nt, [&](int i) { return readlyrics_add_track_label(rd_->add_tview[static_cast<size_t>(i)]); }, [&](int i) {
            const int ti = rd_->add_tview[static_cast<size_t>(i)];
            return rd_->add_lock_cur ? ti < 0 : (ti >= 0 && all_local_tracks_[static_cast<size_t>(ti)].path == rd_->add_lock_track);
        }, "");
        both(l, rr);
    }
    {   // the count lines, under each list
        const auto l = cut(std::to_string(nf) + " of " + std::to_string(rd_->add_files.size()) + " files in " + std::to_string(rd_->add_dirs) +
                           (rd_->add_dirs == 1 ? " lyrics folder" : " lyrics folders"), pw_l, leg);
        const auto r = cut(std::to_string(nt - 1) + " of " + std::to_string(all_local_tracks_.size()) + " tracks  (+ the current track)", pw_r, leg);
        both(pline(l.first, l.second, pw_l), pline(r.first, r.second, pw_r));
    }
    both(box_bottom(pw_l, "", border), box_bottom(pw_r, "", border));
    // --- the locked-in fields
    both(box_top(std::string("LOCKED IN: LYRICS FILE") + (rd_->add_lock_auto && !rd_->add_orig_name.empty() ? (rd_->add_show_orig ? " (original)" : " (copy)") : ""), pw_l, border), box_top("LOCKED IN: TRACK", pw_r, border));
    {
        const bool has_f = !rd_->add_lock_file.path.empty();
        const auto l1 = cut(has_f ? rd_->add_lock_file.name : "(none yet -- highlight a file, press [ENTER])", pw_l, has_f ? hdr : std::string());
        const std::string tt = rd_->add_lock_cur ? (rd_->add_cur.empty() ? std::string(rd_->opened_hit.path.empty() ? "(no current track -- lock one from the list)" : "(no track is named like the opened lyrics -- lock one from the list)") : "Current track:  " + rd_->add_cur_title) : rd_->add_lock_title;
        const auto r1 = cut(tt, pw_r, hdr);
        both(pline(l1.first, l1.second, pw_l), pline(r1.first, r1.second, pw_r));
        // the path of the file, then the note for lyrics that were added by hand; on a narrow pane the note wraps over both rows
        const std::string note = "Added manually; hit DEL to unassign";
        std::string row2 = has_f ? (rd_->add_lock_auto ? "In the lyrics folder: " : std::string()) + path_utf8(rd_->add_lock_file.path) : std::string(), row3;
        std::string sgr2 = leg, sgr3 = hdr;
        if (rd_->add_manual) {
            if (display_width(note) <= pw_l - 4) row3 = note;
            else {
                const std::vector<std::string> parts = wrap_lines(note, pw_l - 4, 2);
                row2 = parts.empty() ? note : parts[0];
                row3 = parts.size() > 1 ? parts[1] : std::string();
                sgr2 = hdr;
            }
        }
        const auto l2 = cut(row2, pw_l, sgr2);
        const auto l3 = cut(row3, pw_l, sgr3);
        std::string info = rd_->add_lock_info.empty() ? std::string("no lyrics saved for this track yet") : "has lyrics: " + rd_->add_lock_info;
        const auto r2 = cut(info, pw_r, leg);
        both(pline(l2.first, l2.second, pw_l), pline(r2.first, r2.second, pw_r));
        both(pline(l3.first, l3.second, pw_l), pline(std::string(), 0, pw_r));
    }
    both(box_bottom(pw_l, "", border), box_bottom(pw_r, "", border));
    // --- the buttons, and what the last key did
    {
        const int inner = PW - 4, gap = 4;
        std::string b1 = "[ Add chosen lyrics to chosen/current song ]", b2 = "[CTRL+c] Clear current choice";
        if (display_width(b1) + gap + display_width(b2) > inner) { b1 = "[ Add chosen lyrics ]"; b2 = "[CTRL+c] Clear choice"; }   // narrow window
        const int w1 = display_width(b1), w2 = display_width(b2);
        const int lead = std::max(0, (inner - w1 - gap - w2) / 2);
        const std::string p1 = rd_->add_pane == 2 ? "\x1b[7m" + b1 + R : hdr + b1 + R;
        const std::string p2 = rd_->add_pane == 3 ? "\x1b[7m" + b2 + R : hdr + b2 + R;
        out.push_back(bar + " " + std::string(static_cast<size_t>(lead), ' ') + p1 + std::string(static_cast<size_t>(gap), ' ') + p2 +
                      std::string(static_cast<size_t>(std::max(0, inner - lead - w1 - gap - w2)), ' ') + " " + bar);
    }
    out.push_back(bar + " " + (rd_->add_warn && !rd_->add_msg.empty() ? std::string("\x1b[31m") : leg) + pad_right(truncate_str(rd_->add_msg, PW - 4), PW - 4) + R + " " + bar);
    out.push_back(box_bottom(PW, "", border_bottom));
    return out;
}

// The legend is ordered and laid out like the one of READ LYRICS itself: one row per group (Move / Choose / Tracks / Session), long
// labels first, short ones when a row does not fit the width.
std::vector<std::string> App::build_readlyrics_add_legend(int W) const {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::vector<LegendGroup> groups = {
        {"Move", {"[TAB] Focus", "[\u2190\u2192\u2191\u2193] Pick\001Pick", "[PgUp/PgDn] Page up/down\001Page", "[HOME/END] Top/Bottom"}},
        {"Choose", {"[ENTER] Lock in / press button\001Lock in", "[CTRL+c] Clear current choice\001Clear", "[DEL] Unassign manual lyrics\001Unassign"}},
        {"Tracks", {"[CTRL+t] File names / artist - title\001File names", "[CTRL+f] Only tracks without lyrics\001No lyrics", "[CTRL+o] Original / copy name\001Orig/copy"}},
        {"Session", {"[ESC] Close"}},
    };
    std::vector<std::string> out;
    const auto lrows = layout_legend_tree(groups, std::max(20, W - 1));
    for (size_t i = 0; i < lrows.size(); ++i) out.push_back(" " + paint_legend(settings_, lrows, i, border, 0, true));
    return out;
}

std::vector<std::string> App::build_readlyrics_options_panel() const {
    const int PW = 66, inner = PW - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    std::vector<std::string> out;
    out.push_back(box_top("TRANSLATE", PW, border));
    auto row = [&](const std::string& label, const std::string& value, bool sel) {
        const std::string lab = pad_right(label, 15);
        const std::string val = pad_right(truncate_str("< " + value + " >", inner - 15), inner - 15);
        out.push_back(bar + " " + header_sgr(settings_) + lab + R + (sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) : std::string()) +
                      val + R + " " + bar);
    };
    auto plain = [&](const std::string& s, bool legend, const char* sgr = nullptr) {
        out.push_back(bar + " " + (sgr ? std::string(sgr) : legend ? legend_sgr(settings_) : std::string()) + pad_right(truncate_str(s, inner), inner) + R + " " + bar);
    };
    std::string from_name = settings_.translate_from == "auto" ? "Auto-detect" : language_name(settings_.translate_from);
    row("SERVICE", translate_service_name(settings_.translate_service), rd_->opts_row == 0);
    row("TRANSLATE INTO", language_name(settings_.translate_to), rd_->opts_row == 1);
    row("LYRICS ARE IN", from_name, rd_->opts_row == 2);
    plain("", false);
    const std::string& svc = settings_.translate_service;
    plain(svc == "google" ? "Google Translate's key-less web endpoint (unofficial)."
                          : "MyMemory: free, about 5000 characters a day per address.", true);
    plain(settings_.translate_from == "auto" ? "If detection is wrong, set LYRICS ARE IN by hand." : "The language of the lyrics is set by hand.", true);
    if (!rd_->opts_warn.empty()) plain(rd_->opts_warn, false, "\x1b[31m");
    else if (rd_->busy) plain("Running: " + language_name(rd_->job_to) + " with " + translate_service_name(rd_->job_svc) + " (goes on in the background)", true);
    else plain("", false);
    out.push_back(box_bottom(PW, "[↑↓] Row  [←→] Change  [ENTER] Translate  [ESC] Close", border_bottom));
    return out;
}

// ---------------------------------------------------------------------
// Export ([e]): the lyrics as a text file in the "export" folder inside the lyrics folder: the original only, or with the saved
// translation and / or the Latin spelling under each line.
// ---------------------------------------------------------------------
int App::readlyrics_export_choices(std::vector<int>& flags, std::vector<std::string>& names) const {
    const bool has_tr = !rd_->tlist.empty() && rd_->tsel >= 0 && rd_->tsel < static_cast<int>(rd_->tlist.size()) && rd_->trans.size() == rd_->lines.size();
    const bool has_tl = lyrics_export_translit_differs(rd_->lines);
    const std::string tr_name = has_tr ? language_name(rd_->tlist[static_cast<size_t>(rd_->tsel)].to) : std::string();
    flags.clear();
    names.clear();
    flags.push_back(0); names.push_back("Original only");
    if (has_tr) { flags.push_back(1); names.push_back("Original + translation (" + tr_name + ")"); }
    if (has_tl) { flags.push_back(2); names.push_back("Original + transliteration (Latin letters)"); }
    if (has_tr && has_tl) { flags.push_back(3); names.push_back("Original + transliteration + translation (" + tr_name + ")"); }
    return static_cast<int>(flags.size());
}

fs::path App::readlyrics_export_dir() const {
    fs::path folder;
    if (rd_->sel >= 0 && rd_->sel < static_cast<int>(rd_->versions.size()) && !rd_->versions[static_cast<size_t>(rd_->sel)].file.empty()) {
        const fs::path f = rd_->versions[static_cast<size_t>(rd_->sel)].file;   // a fetched / older copy: its own lyrics folder
        folder = f.parent_path();
        if (folder.filename() != "lyrics") folder = lyrics_folder_of(rd_->track);
    }
    if (folder.empty()) folder = lyrics_folder_of(rd_->track);
    if (folder.empty() && !rd_->track.empty()) folder = rd_->track.parent_path() / "lyrics";
    return folder / "export";
}

fs::path App::readlyrics_export_file(int flags) const {
    std::string stem = path_utf8(rd_->track.stem());
    if (stem.empty() && rd_->sel >= 0 && rd_->sel < static_cast<int>(rd_->versions.size())) stem = path_utf8(rd_->versions[static_cast<size_t>(rd_->sel)].file.stem());
    if (stem.empty()) stem = rd_->title;
    std::string tag;
    const std::string to = (flags & 1) && rd_->tsel >= 0 && rd_->tsel < static_cast<int>(rd_->tlist.size()) ? rd_->tlist[static_cast<size_t>(rd_->tsel)].to : std::string();
    if ((flags & 1) && (flags & 2)) tag = " (transliteration, translation " + to + ")";
    else if (flags & 1) tag = " (translation " + to + ")";
    else if (flags & 2) tag = " (transliteration)";
    std::string name = PlaylistManager::sanitize_name(stem + tag);
    if (name.empty()) name = "lyrics";
    return readlyrics_export_dir() / path_from_utf8(name + ".txt");
}

bool App::readlyrics_reader_working() const {
    return settings_.latin_letters && !kanji_broken_ && !kanji_script_.empty();
}

void App::readlyrics_export_do() {
    std::vector<int> fl; std::vector<std::string> nm;
    readlyrics_export_choices(fl, nm);
    const int flags = std::find(fl.begin(), fl.end(), rd_->exp_choice) != fl.end() ? rd_->exp_choice : 0;
    if ((flags & 2) && readlyrics_reader_working() && !rd_->exp_wait) {   // the Latin spelling of kanji is not there yet: say so once
        const size_t left = translit_kanji_waiting(rd_->lines);
        if (left > 0) {
            rd_->exp_wait = true;
            rd_->exp_msg = "Still reading the kanji (" + std::to_string(left) + " lines left): wait a moment, or ENTER again to export as it is";
            rd_->exp_warn = true;
            return;
        }
    }
    rd_->exp_wait = false;
    const std::string text = lyrics_export_text(rd_->lines, (flags & 1) ? &rd_->trans : nullptr, (flags & 2) != 0);
    const fs::path file = readlyrics_export_file(flags);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (text.empty() || ec || !write_text_atomic(file, text)) {
        rd_->status = ec ? "could not create the export folder: " + path_utf8(file.parent_path()) : text.empty() ? "nothing to export" : "could not write " + path_utf8(file);
        rd_->status_warn = true;
    } else {
        rd_->status = "exported to " + path_utf8(file);
        rd_->status_warn = false;
        log_event("lyrics exported: " + path_utf8(file));
    }
    rd_->exp_msg = rd_->status_warn ? rd_->status : "Exported: " + path_utf8(file.filename());   // the overlay stays open and says what happened
    rd_->exp_warn = rd_->status_warn;
}

// The export overlay: LEFT / RIGHT choose what the file holds, ENTER writes it, ESC closes.
void App::readlyrics_export_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (key == 27 || key_is("HKeyReadLyrics", key) || (!arrow && (key == 'q' || key == 'Q' || key == 'e' || key == 'E'))) { rd_->exp_open = false; return; }
    if (key == '\r' || key == '\n') { readlyrics_export_do(); return; }
    std::vector<int> fl; std::vector<std::string> nm;
    const int n = readlyrics_export_choices(fl, nm);
    int idx = 0;
    for (int i = 0; i < n; ++i) if (fl[static_cast<size_t>(i)] == rd_->exp_choice) idx = i;
    if (arrow && (key == 'C' || key == 'B')) idx = (idx + 1) % n;
    else if (arrow && (key == 'D' || key == 'A')) idx = (idx + n - 1) % n;
    rd_->exp_choice = fl[static_cast<size_t>(idx)];
    rd_->exp_msg.clear();
    rd_->exp_wait = false;
}

std::vector<std::string> App::build_readlyrics_export_panel() const {
    const int PW = 74, inner = PW - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    std::vector<std::string> out;
    out.push_back(box_top("EXPORT LYRICS", PW, border));
    auto plain = [&](const std::string& s, bool legend) {
        out.push_back(bar + " " + (legend ? legend_sgr(settings_) : std::string()) + pad_right(truncate_str(s, inner), inner) + R + " " + bar);
    };
    std::vector<int> fl; std::vector<std::string> nm;
    const int n = readlyrics_export_choices(fl, nm);
    int idx = 0;
    for (int i = 0; i < n; ++i) if (fl[static_cast<size_t>(i)] == rd_->exp_choice) idx = i;
    plain("What the file holds:", true);
    for (int i = 0; i < n; ++i) {
        const std::string txt = pad_right(truncate_str(std::string(i == idx ? "  > " : "    ") + nm[static_cast<size_t>(i)], inner), inner);
        out.push_back(bar + " " + (i == idx ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) : std::string()) + txt + R + " " + bar);
    }
    plain("", false);
    const fs::path file = readlyrics_export_file(fl[static_cast<size_t>(idx)]);
    plain("Folder:", true);
    plain("  " + path_utf8(file.parent_path()), false);
    plain("File:", true);
    plain("  " + path_utf8(file.filename()), false);
    const size_t unread = (fl[static_cast<size_t>(idx)] & 2) && readlyrics_reader_working() ? translit_kanji_waiting(rd_->lines) : 0;
    if (!rd_->exp_msg.empty()) { plain(rd_->exp_msg, !rd_->exp_warn); }
    else if (unread > 0) plain("Reading the kanji with pykakasi ... " + std::to_string(unread) + " lines left -- wait a moment", true);
    else if (rd_->tlist.empty()) plain("[t] fetches a translation; then it can be exported too.", true);
    else if (n < 3 && !lyrics_export_translit_differs(rd_->lines)) plain("These lyrics have no kana / Hangul / Chinese characters to spell in Latin letters.", true);
    else plain("", false);
    out.push_back(box_bottom(PW, "[\u2190\u2192] Choose  [ENTER] Export  [ESC] Close", border_bottom));
    return out;
}


} // namespace muisc
