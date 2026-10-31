// READ LYRICS overlay (stored lyrics versions, translations, search). Split out of app.cpp.
// Member functions of App (declared in app.h); the helpers shared with app.cpp are in app_internal.h.
#include "app.h"
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

void App::readlyrics_open() {
    fs::path track;
    std::string title;
    if (has_track_ && !current_path_.empty()) {
        track = current_path_;
        title = (metadata_.artist == "-" || metadata_.artist.empty()) ? metadata_.name : metadata_.artist + " - " + metadata_.name;
    } else if (list_source_ == ListSource::Local && selected_ >= 0 && selected_ < static_cast<int>(local_view_.size())) {
        const LocalTrack& t = local_view_[static_cast<size_t>(selected_)];
        track = t.path;
        title = t.folder_artist.empty() ? t.title : t.folder_artist + " - " + t.title;
    }
    if (track.empty()) { status_line_ = "no track to read lyrics of -- play or hover a track first"; return; }
    rd_track_ = track;
    rd_title_ = title.empty() ? path_utf8(track.stem()) : title;
    rd_versions_ = list_lyrics_versions(track);
    rd_sel_ = 0;
    for (size_t i = 0; i < rd_versions_.size(); ++i) if (rd_versions_[i].active) { rd_sel_ = static_cast<int>(i); break; }
    rd_first_ = 0;
    rd_pane_ = 0;
    rd_confirm_del_ = false;
    rd_opts_open_ = false;
    rd_exp_open_ = false;
    rd_search_open_ = false;
    rd_add_open_ = false;
    rd_opts_warn_.clear();
    rd_status_.clear();
    rd_status_warn_ = false;
    rd_follow_ = true;
    if (rd_busy_) { rd_status_ = "a translation is still running (" + language_name(rd_job_to_) + " with " + translate_service_name(rd_job_svc_) + ")"; }
    readlyrics_load_version();
    mode_ = Mode::ReadLyrics;
    force_redraw_ = true;   // full-screen takeover
}

void App::readlyrics_close() {
    if (rd_opts_dirty_) { save_settings(settings_); rd_opts_dirty_ = false; }
    rd_opts_open_ = false;
    rd_exp_open_ = false;
    rd_search_open_ = false;
    rd_add_open_ = false;
    rd_confirm_del_ = false;
    // A translation that is still running is NOT stopped: it is saved when done (readlyrics_poll() tells).
    mode_ = Mode::Browse;
    force_redraw_ = true;
}

void App::readlyrics_load_version() {
    rd_lines_.clear();
    rd_timed_.clear();
    rd_delay_ = 0.0;
    rd_trans_.clear();
    rd_tlist_.clear();
    rd_from_.clear();
    rd_show_ = rd_same_ = false;
    rd_scroll_ = 0;
    rd_tsel_ = rd_tfirst_ = 0;
    rd_confirm_del_ = false;
    if (rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) return;
    const LyricsVersion& v = rd_versions_[static_cast<size_t>(rd_sel_)];
    rd_timed_ = lyrics_read_lines(read_lyrics_version_text(rd_track_, v), &rd_delay_);
    for (const LyricLine& l : rd_timed_) rd_lines_.push_back(l.full_text);
    translit_kanji_waiting(rd_lines_);   // (asks the kanji reader for every line now: an export later does not have to wait for it)
    readlyrics_refresh_translations(settings_.translate_to);
    rd_show_ = !rd_tlist_.empty();   // a translation that is there is shown at once
}

bool App::readlyrics_track_plays() const {
    return has_track_ && !current_path_.empty() && current_path_ == rd_track_;
}

// [p]: the track of the reader starts (the library entry, or the file when it is not in the library); when it is the loaded one already
// it pauses / resumes. The reader stays open and follows the song.
void App::readlyrics_play() {
    rd_status_warn_ = false;
    if (readlyrics_track_plays()) {
        act_play_pause();
        rd_status_ = player_.is_paused() ? "paused" : "playing";
        return;
    }
    std::error_code ec;
    if (rd_track_.empty() || !fs::exists(rd_track_, ec)) {
        rd_status_ = "this lyrics file has no track to play (it came from the search) -- open the reader for a track with CTRL+L";
        rd_status_warn_ = true;
        return;
    }
    LocalTrack t;
    bool found = false;
    for (const auto& lt : all_local_tracks_) if (lt.path == rd_track_) { t = lt; found = true; break; }
    if (!found) {
        t.path = rd_track_;
        t.title = path_utf8(rd_track_.stem());
        t.folder_artist = path_utf8(rd_track_.parent_path().filename());
    }
    start_local_track(t);
    rd_follow_ = true;
    rd_status_ = "playing: " + t.title;
}

// Reads the saved translations of the chosen file again; `prefer_to` is the language to select when it is there.
void App::readlyrics_refresh_translations(const std::string& prefer_to, const std::string& prefer_svc) {
    rd_tlist_.clear();
    rd_same_ = false;
    if (rd_lines_.empty() || rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) { readlyrics_select_translation(-1); return; }
    const LyricsVersion& v = rd_versions_[static_cast<size_t>(rd_sel_)];
    rd_tlist_ = list_lyrics_translations(rd_track_, v, rd_lines_);
    int pick = rd_tlist_.empty() ? -1 : 0;
    // the preferred language; among several services the preferred one (the one just used, else the one chosen in the options)
    const std::string svc = prefer_svc.empty() ? settings_.translate_service : prefer_svc;
    for (size_t i = 0; i < rd_tlist_.size(); ++i)
        if (rd_tlist_[i].to == prefer_to && (rd_tlist_[static_cast<size_t>(pick)].to != prefer_to || rd_tlist_[i].service == svc)) pick = static_cast<int>(i);
    readlyrics_select_translation(pick);
    LyricsTranslation note;   // "these lyrics are in <language> already", saved by an earlier try
    if (load_lyrics_translation(rd_track_, v, settings_.translate_to, rd_lines_, note, settings_.translate_service) && note.same) rd_same_ = true;
}

void App::readlyrics_select_translation(int index) {
    rd_trans_.clear();
    rd_from_.clear();
    rd_to_.clear();
    rd_svc_.clear();
    if (index < 0 || index >= static_cast<int>(rd_tlist_.size()) || rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) {
        rd_tsel_ = 0;
        return;
    }
    rd_tsel_ = index;
    const TranslationInfo& info = rd_tlist_[static_cast<size_t>(index)];
    LyricsTranslation t;
    if (load_lyrics_translation(rd_track_, rd_versions_[static_cast<size_t>(rd_sel_)], info.to, rd_lines_, t, info.service) && !t.same) {
        rd_trans_ = std::move(t.lines);
        rd_from_ = t.from;
        rd_svc_ = t.service;
        rd_to_ = info.to;
    }
}

// SHIFT+t: hidden -> the chosen translation -> the next one ... -> hidden. Never fetches anything.
void App::readlyrics_toggle_translations() {
    rd_status_warn_ = false;
    if (rd_tlist_.empty()) {
        rd_status_ = rd_busy_ ? "no translation yet -- the running one is not finished" : "no translation saved for this file -- [t] fetches one";
        rd_status_warn_ = true;
        return;
    }
    const int n = static_cast<int>(rd_tlist_.size());
    if (!rd_show_) {
        rd_show_ = true;
    } else if (rd_tsel_ < n - 1) {
        readlyrics_select_translation(rd_tsel_ + 1);
    } else {
        rd_show_ = false;
        readlyrics_select_translation(0);
    }
    if (rd_show_) {
        const TranslationInfo& t = rd_tlist_[static_cast<size_t>(rd_tsel_)];
        rd_status_ = "showing the " + language_name(t.to) + " translation" + (n > 1 ? " (" + std::to_string(rd_tsel_ + 1) + "/" + std::to_string(n) + ", SHIFT+t for the next)" : std::string());
    } else {
        rd_status_ = "translations hidden";
    }
}

void App::readlyrics_delete_translation() {
    rd_confirm_del_ = false;
    if (rd_tsel_ < 0 || rd_tsel_ >= static_cast<int>(rd_tlist_.size()) || rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) return;
    const std::string to = rd_tlist_[static_cast<size_t>(rd_tsel_)].to, svc = rd_tlist_[static_cast<size_t>(rd_tsel_)].service;
    const bool ok = delete_lyrics_translation(rd_track_, rd_versions_[static_cast<size_t>(rd_sel_)], to, svc);
    const int keep = rd_tsel_;
    readlyrics_refresh_translations("");
    readlyrics_select_translation(rd_tlist_.empty() ? -1 : std::min(keep, static_cast<int>(rd_tlist_.size()) - 1));
    if (rd_tlist_.empty()) rd_show_ = false;
    rd_status_warn_ = !ok;
    rd_status_ = ok ? "deleted the " + language_name(to) + " translation (" + translate_service_name(svc) + ")" : "could not delete the " + language_name(to) + " translation";
}

// ENTER in the translate overlay: fetch a translation with the service and languages set there.
void App::readlyrics_translate() {
    rd_opts_warn_.clear();
    if (rd_busy_) {
        rd_opts_warn_ = "WARNING: the last translation is not finished yet -- please wait";
        return;
    }
    if (rd_lines_.empty() || rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) {
        rd_opts_warn_ = "there are no lyrics to translate";
        return;
    }
    TranslateRequest req;
    req.service = settings_.translate_service;
    req.from = settings_.translate_from;
    req.to = settings_.translate_to;
    rd_opts_open_ = false;
    if (rd_opts_dirty_) { save_settings(settings_); rd_opts_dirty_ = false; }
    rd_busy_ = true;
    rd_job_to_ = req.to;
    rd_job_svc_ = req.service;
    rd_job_track_ = rd_track_;
    rd_job_version_ = rd_versions_[static_cast<size_t>(rd_sel_)];
    rd_status_warn_ = false;
    rd_status_ = "translating into " + language_name(req.to) + " with " + translate_service_name(req.service) +
                 " ... this takes a while (needs an internet connection); you can go on reading";
    const std::vector<std::string> plain = rd_lines_;
    const LyricsVersion v = rd_job_version_;
    const fs::path track = rd_track_, script = translate_script_;
    spawn_bg("lyrics translation", [this, plain, v, track, script, req]() {
        LyricsTranslation t = translate_lyrics_lines(plain, req, script);
        if (t.ok) save_lyrics_translation(track, v, req.to, plain, t);
        std::lock_guard<std::mutex> lk(rd_mutex_);
        rd_result_ = std::move(t);
        rd_result_ready_ = true;
    });
}

void App::readlyrics_poll() {
    // The re-fetch overlay was used over the reader: when its fetch is done, the reader shows the lyrics now in use.
    if (rd_watch_epoch_ >= 0 && mode_ == Mode::ReadLyrics && lyrics_epoch_.load() != rd_watch_epoch_) {
        bool done = false;
        {
            std::lock_guard<std::mutex> lk(lyrics_mutex_);
            done = lyrics_ready_ && lyrics_path_ == rd_track_;
        }
        if (done) {
            rd_watch_epoch_ = -1;
            rd_versions_ = list_lyrics_versions(rd_track_);
            rd_sel_ = 0;
            for (size_t i = 0; i < rd_versions_.size(); ++i) if (rd_versions_[i].active) { rd_sel_ = static_cast<int>(i); break; }
            rd_first_ = 0;
            readlyrics_load_version();
            rd_status_ = rd_lines_.empty() ? "no lyrics found for this track" : "lyrics loaded: " + std::to_string(rd_lines_.size()) + " lines";
            rd_status_warn_ = rd_lines_.empty();
        }
    }
    LyricsTranslation t;
    {
        std::lock_guard<std::mutex> lk(rd_mutex_);   // (the flag is written by the worker thread under this lock: never read it without)
        if (!rd_result_ready_) return;
        t = std::move(rd_result_);
        rd_result_ = LyricsTranslation{};
        rd_result_ready_ = false;
    }
    rd_busy_ = false;
    const std::string what = language_name(rd_job_to_);
    // Is the file the translation was made for the one on the screen?
    const bool on_screen = mode_ == Mode::ReadLyrics && rd_sel_ >= 0 && rd_sel_ < static_cast<int>(rd_versions_.size()) &&
        lyrics_translation_file(rd_track_, rd_versions_[static_cast<size_t>(rd_sel_)], "xx") == lyrics_translation_file(rd_job_track_, rd_job_version_, "xx");
    std::string msg;
    bool warn = false;
    if (!t.ok) { msg = "translation into " + what + " failed: " + t.error; warn = true; }
    else if (t.same) msg = "these lyrics are in " + what + " already -- nothing to translate ([t] changes the languages)";
    else msg = "translated from " + language_name(t.from) + " to " + what + " with " + translate_service_name(rd_job_svc_) + " -- saved in the lyrics folder (translations)";
    if (on_screen) {
        if (t.ok) {
            readlyrics_refresh_translations(rd_job_to_, rd_job_svc_);
            if (!t.same && !rd_tlist_.empty()) rd_show_ = true;
            if (t.same) rd_same_ = true;
        }
        rd_status_ = msg;
        rd_status_warn_ = warn;
    } else {
        status_line_ = "lyrics: " + msg;      // READ LYRICS is closed (or shows another file): tell on the main screen
        if (mode_ == Mode::ReadLyrics) { rd_status_ = msg; rd_status_warn_ = warn; }
    }
}

// The translate overlay ([t]): UP / DOWN pick the row, LEFT / RIGHT change it (a letter jumps to the next language
// starting with it), ENTER fetches the translation, ESC closes it. A running translation is not touched by any of it.
void App::readlyrics_options_key(int key) {
    const bool arrow = last_key_was_arrow();
    auto close_panel = [&]() {
        rd_opts_open_ = false;
        rd_opts_warn_.clear();
        if (rd_opts_dirty_) { save_settings(settings_); rd_opts_dirty_ = false; }
    };
    if (key == 27 || key_is("HKeyReadLyrics", key)) { close_panel(); return; }
    if (key == '\r' || key == '\n') { readlyrics_translate(); return; }
    if (arrow && key == 'A') { rd_opts_row_ = std::max(0, rd_opts_row_ - 1); return; }
    if (arrow && key == 'B') { rd_opts_row_ = std::min(2, rd_opts_row_ + 1); return; }
    // the value lists: 0 services, 1 target languages, 2 source languages ("auto" first)
    std::vector<std::string> ids;
    std::vector<std::string> names;
    std::string* cur = nullptr;
    if (rd_opts_row_ == 0) {
        for (const auto& s : translate_services()) { ids.push_back(s.first); names.push_back(s.second); }
        cur = &settings_.translate_service;
    } else {
        if (rd_opts_row_ == 2) { ids.push_back("auto"); names.push_back("Auto-detect"); }
        for (const auto& l : translate_languages()) { ids.push_back(l.first); names.push_back(l.second); }
        cur = rd_opts_row_ == 1 ? &settings_.translate_to : &settings_.translate_from;
    }
    int idx = 0;
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == *cur) { idx = static_cast<int>(i); break; }
    const int n = static_cast<int>(ids.size());
    int next = idx;
    if (arrow && (key == 'C' || key == 'D')) next = (idx + (key == 'C' ? 1 : n - 1)) % n;
    else if (rd_opts_row_ > 0 && std::isalpha(key) && key < 128) {   // jump to the next language that starts with the letter
        const char want = static_cast<char>(std::tolower(key));
        for (int k = 1; k <= n; ++k) {
            const int i = (idx + k) % n;
            if (!names[static_cast<size_t>(i)].empty() && std::tolower(static_cast<unsigned char>(names[static_cast<size_t>(i)][0])) == want) { next = i; break; }
        }
    } else return;
    if (next == idx) return;
    *cur = ids[static_cast<size_t>(next)];
    rd_opts_dirty_ = true;
    rd_opts_warn_.clear();
    rd_same_ = false;
}

void App::handle_readlyrics_key(int key) {
    if (key == 0) return;
    if (rd_opts_open_) { readlyrics_options_key(key); return; }
    if (rd_exp_open_) { readlyrics_export_key(key); return; }
    if (rd_search_open_) { readlyrics_search_key(key); return; }
    if (rd_add_open_) { readlyrics_add_key(key); return; }
    const bool arrow = last_key_was_arrow();
    if (!arrow && key_is("HKeyTranslit", key)) { toggle_latin_letters(); return; }   // Latin letters on / off, also here
    if (!arrow && key_is("HKeyLyricsTiming", key)) { lyrics_edit_open_over_reader(); return; }   // the lyrics timing overlay, as on the main screen
    if (!arrow && key_is("HKeyRetryLyrics", key) && !rd_confirm_del_) { rl_open_over_reader(); return; }   // the re-fetch / version control overlay
    if (key == kKeyCtrlS && !rd_confirm_del_) { readlyrics_search_open(); return; }
    if (!rd_confirm_del_ && (key == kKeyCtrlN || (!arrow && (key == 'a' || key == 'A')))) { readlyrics_add_open(); return; }   // add a lyrics file by hand
    if (rd_confirm_del_) {   // "delete this translation?" -- only y confirms
        if (!arrow && (key == 'y' || key == 'Y')) readlyrics_delete_translation();
        else { rd_confirm_del_ = false; rd_status_ = "kept"; rd_status_warn_ = false; }
        return;
    }
    if (key == 27 || key_is("HKeyReadLyrics", key) || key == 'q' || key == 'Q') { readlyrics_close(); return; }
    if (!arrow && key == '\t') { rd_pane_ = rd_pane_ == 0 ? 1 : 0; rd_status_.clear(); return; }
    if (!arrow && (key == 'p' || key == 'P')) { readlyrics_play(); return; }
    if (!arrow && (key == 'f' || key == 'F')) {
        rd_follow_ = !rd_follow_;
        rd_status_ = rd_follow_ ? "the view follows the song" : "the view stays where it is -- [f] follows the song again";
        rd_status_warn_ = false;
        return;
    }
    if (key == kKeyShiftLeft || key == kKeyShiftRight) {   // SHIFT+left / right: 5 s back / forward, to hear a line again
        if (readlyrics_track_plays()) player_.seek_relative(key == kKeyShiftLeft ? -5.0 : 5.0);
        return;
    }
    if (!arrow && (key == 'e' || key == 'E')) {   // [e]: export these lyrics
        if (rd_lines_.empty()) { rd_status_ = "there are no lyrics to export"; rd_status_warn_ = true; return; }
        std::vector<int> fl; std::vector<std::string> nm;
        readlyrics_export_choices(fl, nm);
        if (std::find(fl.begin(), fl.end(), rd_exp_choice_) == fl.end()) rd_exp_choice_ = fl.back();   // (the most there is)
        rd_exp_open_ = true;
        rd_exp_wait_ = false;
        translit_kanji_waiting(rd_lines_);
        rd_exp_msg_.clear();
        rd_status_.clear();
        return;
    }
    if (!arrow && key == 't') { rd_opts_open_ = true; rd_opts_row_ = 0; rd_opts_warn_.clear(); return; }
    if (!arrow && key == 'T') { readlyrics_toggle_translations(); return; }
    const int n = static_cast<int>(rd_versions_.size());
    // LEFT / RIGHT choose in the pane that has the keys ('D' is Left, 'C' is Right)
    if (arrow && (key == 'D' || key == 'C')) {
        const int dir = key == 'D' ? -1 : 1;
        if (rd_pane_ == 0) {
            const int ns = std::clamp(rd_sel_ + dir, 0, std::max(0, n - 1));
            if (ns != rd_sel_) { rd_sel_ = ns; rd_status_.clear(); readlyrics_load_version(); }
        } else if (!rd_tlist_.empty()) {
            const int nt = static_cast<int>(rd_tlist_.size());
            const int ns = std::clamp(rd_tsel_ + dir, 0, nt - 1);
            if (ns != rd_tsel_ || !rd_show_) { readlyrics_select_translation(ns); rd_show_ = true; rd_status_.clear(); }
        }
        return;
    }
    if (rd_pane_ == 1 && (key == kKeyDelete || (!arrow && (key == 'd' || key == 'D')))) {
        if (rd_tlist_.empty()) { rd_status_ = "there is no translation to delete"; rd_status_warn_ = true; }
        else rd_confirm_del_ = true;
        return;
    }
    const bool scrolls = (arrow && (key == 'A' || key == 'B')) || key == ' ' || key == 'b' || key == 'B' || key == kKeyHome || key == kKeyEnd;
    if (scrolls && rd_follow_ && readlyrics_track_plays()) { rd_follow_ = false; rd_status_ = "the view stays where it is -- [f] follows the song again"; rd_status_warn_ = false; }
    if (arrow && last_key_was_shifted() && (key == 'A' || key == 'B')) {   // SHIFT+up / down: a page
        const int page = std::max(1, term_rows_ - 14);
        rd_scroll_ = key == 'A' ? std::max(0, rd_scroll_ - page) : rd_scroll_ + page;
        return;
    }
    if (arrow && key == 'A') { if (rd_scroll_ > 0) --rd_scroll_; return; }
    if (arrow && key == 'B') { ++rd_scroll_; return; }          // clamped against the real content while drawing
    if (key == ' ') { rd_scroll_ += std::max(1, term_rows_ - 14); return; }
    if (key == 'b' || key == 'B') { rd_scroll_ = std::max(0, rd_scroll_ - std::max(1, term_rows_ - 14)); return; }
    if (key == kKeyHome) { rd_scroll_ = 0; return; }
    if (key == kKeyEnd) { rd_scroll_ = 1 << 20; return; }       // clamped to the last page while drawing
}

// --- CTRL+s: search the lyrics files ------------------------------------------------------------------------------
// Lyrics live in a "lyrics" folder inside the folder of each track (and of the library folders / the download cache),
// so those are the folders that are searched: one listing per distinct folder, done when the search is opened.
void App::readlyrics_search_open() {
    std::set<std::string> dirs;
    auto add_dir = [&](const fs::path& folder) { if (!folder.empty()) dirs.insert(path_utf8(folder / "lyrics")); };
    for (const auto& t : all_local_tracks_) add_dir(t.path.parent_path());
    for (const auto& p : settings_.local_music_paths) add_dir(path_from_utf8(p));
    add_dir(rd_track_.parent_path());
    rd_search_all_.clear();
    rd_search_dirs_ = 0;
    for (const auto& d : dirs) {
        std::error_code ec;
        const fs::path dir = path_from_utf8(d);
        if (!fs::is_directory(dir, ec)) continue;
        ++rd_search_dirs_;
        const std::string folder = path_utf8(dir.parent_path().filename());
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            const std::string ext = ascii_lower_str(path_utf8(it->path().extension()));
            if (ext != ".lrc" && ext != ".txt") continue;
            LyricsFileHit h;
            h.path = it->path();
            h.name = path_utf8(it->path().stem());
            h.lname = ascii_lower_str(h.name);
            h.folder = folder;
            rd_search_all_.push_back(std::move(h));
            if (rd_search_all_.size() >= 200000) break;
        }
    }
    std::sort(rd_search_all_.begin(), rd_search_all_.end(), [](const LyricsFileHit& a, const LyricsFileHit& b) {
        return a.lname != b.lname ? a.lname < b.lname : a.folder < b.folder;
    });
    rd_search_q_.clear();
    rd_search_sel_ = rd_search_first_ = 0;
    readlyrics_search_rebuild();
    rd_search_open_ = true;
    rd_status_.clear();
}

// Every word of the query has to be somewhere in the file name (any order, case-insensitive for ASCII letters).
void App::readlyrics_search_rebuild() {
    std::vector<std::string> words;
    {
        std::istringstream iss(ascii_lower_str(rd_search_q_));
        std::string w;
        while (iss >> w) words.push_back(w);
    }
    rd_search_view_.clear();
    for (size_t i = 0; i < rd_search_all_.size(); ++i) {
        bool ok = true;
        for (const auto& w : words) if (rd_search_all_[i].lname.find(w) == std::string::npos) { ok = false; break; }
        if (ok) rd_search_view_.push_back(static_cast<int>(i));
    }
    const int n = static_cast<int>(rd_search_view_.size());
    rd_search_sel_ = std::clamp(rd_search_sel_, 0, std::max(0, n - 1));
    rd_search_first_ = std::clamp(rd_search_first_, 0, std::max(0, n - 1));
}

// Makes the reader show one lyrics file. It is handed over as the only "version" of a track that sits in the folder
// above the lyrics folder: that is what makes the translations of this file land in (and come from) the folder the file
// is in, exactly as for a real track (the track file itself is never opened).
void App::readlyrics_show_file(const LyricsFileHit& hit) {
    LyricsVersion v;
    v.kind = LyricsVersion::Kind::Fetched;
    v.file = hit.path;
    v.label = path_utf8(hit.path.filename());
    v.detail = hit.folder;
    v.active = true;
    rd_track_ = hit.path.parent_path().parent_path() / path_from_utf8(hit.name + ".audio");
    rd_title_ = hit.name;
    rd_versions_.assign(1, v);
    rd_sel_ = 0;
    rd_first_ = 0;
    rd_pane_ = 0;
    readlyrics_load_version();
    rd_status_ = "showing a lyrics file from the search -- CTRL+L closes the reader, open it again for the playing track";
    rd_status_warn_ = false;
}

void App::readlyrics_search_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int n = static_cast<int>(rd_search_view_.size());
    if (key == 27 || key == kKeyCtrlS) { rd_search_open_ = false; return; }
    if (key == '\r' || key == '\n') {
        if (n == 0) return;
        const LyricsFileHit hit = rd_search_all_[static_cast<size_t>(rd_search_view_[static_cast<size_t>(rd_search_sel_)])];
        rd_search_open_ = false;
        readlyrics_show_file(hit);
        return;
    }
    if (arrow && key == 'A') { rd_search_sel_ = std::max(0, rd_search_sel_ - 1); return; }
    if (arrow && key == 'B') { rd_search_sel_ = std::min(std::max(0, n - 1), rd_search_sel_ + 1); return; }
    if (arrow) return;
    if (key == kKeyHome) { rd_search_sel_ = 0; return; }
    if (key == kKeyEnd) { rd_search_sel_ = std::max(0, n - 1); return; }
    if (key == 127 || key == 8) { pop_utf8_char(rd_search_q_); rd_search_sel_ = rd_search_first_ = 0; readlyrics_search_rebuild(); return; }
    if (is_text_key(key) && rd_search_q_.size() < 100) {
        rd_search_q_ += static_cast<char>(key);
        rd_search_sel_ = rd_search_first_ = 0;
        readlyrics_search_rebuild();
    }
}

std::vector<std::string> App::build_readlyrics_search_panel() const {
    const int PW = 74, inner = PW - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    std::vector<std::string> out;
    out.push_back(box_top("SEARCH LYRICS FILES", PW, border));
    // the query, with a caret
    {
        const std::string pre = "Search: ";
        const int room = std::max(1, inner - display_width(pre) - 1);
        std::string shown = rd_search_q_;   // a long query keeps its END visible: cut from the front
        while (display_width(shown) > room && !shown.empty()) {
            size_t i = 1;
            while (i < shown.size() && (static_cast<unsigned char>(shown[i]) & 0xC0) == 0x80) ++i;
            shown.erase(0, i);
        }
        out.push_back(bar + " " + header_sgr(settings_) + pre + R + shown + "\x1b[7m \x1b[0m" +
                      std::string(static_cast<size_t>(std::max(0, inner - display_width(pre) - display_width(shown) - 1)), ' ') + " " + bar);
    }
    out.push_back(bar + " " + std::string(static_cast<size_t>(inner), ' ') + " " + bar);
    const int list_rows = std::clamp(term_rows_ - 12, 3, 14);
    const int n = static_cast<int>(rd_search_view_.size());
    int first = std::clamp(rd_search_first_, 0, std::max(0, n - 1));
    const int sel = std::clamp(rd_search_sel_, 0, std::max(0, n - 1));
    if (sel < first) first = sel;
    if (sel >= first + list_rows) first = sel - list_rows + 1;
    for (int r = 0; r < list_rows; ++r) {
        const int i = first + r;
        std::string painted;
        if (i < n) {
            const LyricsFileHit& h = rd_search_all_[static_cast<size_t>(rd_search_view_[static_cast<size_t>(i)])];
            const std::string tail = h.folder.empty() ? std::string() : "  (" + h.folder + ")";
            const int tail_w = std::min(display_width(tail), inner / 3);
            std::string left = truncate_str(h.name, std::max(5, inner - tail_w));
            std::string line = pad_right(left, inner - tail_w) + pad_right(truncate_str(tail, tail_w), tail_w);
            painted = i == sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + line + R : line;
        } else if (r == 0 && n == 0) {
            painted = pad_right(rd_search_all_.empty() ? "no lyrics files found in the lyrics folders of the library" : "no lyrics file matches", inner);
        } else {
            painted = std::string(static_cast<size_t>(inner), ' ');
        }
        out.push_back(bar + " " + painted + " " + bar);
    }
    out.push_back(bar + " " + std::string(static_cast<size_t>(inner), ' ') + " " + bar);
    const std::string info = std::to_string(n) + " of " + std::to_string(rd_search_all_.size()) + " files  in " + std::to_string(rd_search_dirs_) + (rd_search_dirs_ == 1 ? " folder" : " folders");
    out.push_back(bar + " " + legend_sgr(settings_) + pad_right(truncate_str(info, inner), inner) + R + " " + bar);
    out.push_back(box_bottom(PW, "[↑↓] Pick  [ENTER] Open  [ESC] Close", border_bottom));
    return out;
}

// --- [a] / CTRL+n: add a lyrics file to a track by hand ---------------------------------------------------------
std::string App::readlyrics_add_track_label(int idx) const {
    if (idx < 0) {
        if (rd_add_cur_.empty()) return "Current track:  (none -- play or open a track first)";
        return "Current track:  " + (rd_add_names_ ? path_utf8(rd_add_cur_.filename()) : rd_add_cur_title_);
    }
    const LocalTrack& t = all_local_tracks_[static_cast<size_t>(idx)];
    if (rd_add_names_) {
        const std::string folder = path_utf8(t.path.parent_path().filename());
        return path_utf8(t.path.filename()) + (folder.empty() ? std::string() : "  (" + folder + ")");
    }
    return readlyrics_add_track_text(t);
}

std::string App::readlyrics_add_track_text(const LocalTrack& t) {
    return (t.folder_artist.empty() || t.folder_artist == "-") ? t.title : t.folder_artist + " - " + t.title;
}

int App::readlyrics_add_list_rows() const {
    return std::max(3, std::max(20, term_rows_ - 1) - kAddLegendRows - 15);
}

void App::readlyrics_add_open() {
    // The current track: the one the reader shows (when it is a real track), else the one that plays.
    std::error_code ec;
    rd_add_cur_.clear();
    rd_add_cur_title_.clear();
    if (!rd_track_.empty() && fs::exists(rd_track_, ec)) { rd_add_cur_ = rd_track_; rd_add_cur_title_ = rd_title_; }
    else if (has_track_ && !current_path_.empty()) {
        rd_add_cur_ = current_path_;
        rd_add_cur_title_ = (metadata_.artist == "-" || metadata_.artist.empty()) ? metadata_.name : metadata_.artist + " - " + metadata_.name;
    }
    // Every lyrics file below the "lyrics" folders of the library (subfolders too; the translations and exports are not lyrics).
    std::set<std::string> dirs;
    auto add_dir = [&](const fs::path& folder) { if (!folder.empty()) dirs.insert(path_utf8(folder / "lyrics")); };
    for (const auto& t : all_local_tracks_) add_dir(t.path.parent_path());
    for (const auto& p : settings_.local_music_paths) add_dir(path_from_utf8(p));
    if (!rd_add_cur_.empty()) add_dir(rd_add_cur_.parent_path());
    rd_add_files_.clear();
    rd_add_dirs_ = 0;
    for (const auto& d : dirs) {
        const fs::path dir = path_from_utf8(d);
        if (!fs::is_directory(dir, ec)) continue;
        ++rd_add_dirs_;
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
            rd_add_files_.push_back(std::move(h));
            if (rd_add_files_.size() >= 200000) break;
        }
    }
    std::sort(rd_add_files_.begin(), rd_add_files_.end(), [](const LyricsFileHit& a, const LyricsFileHit& b) {
        return a.lname != b.lname ? a.lname < b.lname : a.folder < b.folder;
    });
    rd_add_fq_.clear();
    rd_add_tq_.clear();
    rd_add_fsel_ = rd_add_tsel_ = 0;
    rd_add_pane_ = 0;
    rd_add_msg_.clear();
    rd_add_warn_ = false;
    rd_add_confirm_ = false;
    rd_add_lock_file_ = LyricsFileHit();
    rd_add_lock_auto_ = false;
    rd_add_lock_cur_ = true;
    rd_add_lock_track_.clear();
    readlyrics_add_rebuild();
    readlyrics_add_lock_refresh();
    rd_add_open_ = true;
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
    rd_add_typed_ = LyricsFileHit();
    {
        std::string q = rd_add_fq_;
        while (!q.empty() && (q.back() == ' ' || q.back() == '"' || q.back() == '\'')) q.pop_back();
        while (!q.empty() && (q.front() == ' ' || q.front() == '"' || q.front() == '\'')) q.erase(0, 1);
        if (!q.empty() && (q.find('/') != std::string::npos || q.find('\\') != std::string::npos)) {
            std::error_code ec;
            const fs::path p = path_from_utf8(q);
            const std::string ext = ascii_lower_str(path_utf8(p.extension()));
            if ((ext == ".lrc" || ext == ".txt") && fs::is_regular_file(p, ec)) {
                rd_add_typed_.path = p;
                rd_add_typed_.name = path_utf8(p.stem());
                rd_add_typed_.lname = ascii_lower_str(rd_add_typed_.name);
                rd_add_typed_.folder = "typed path";
            }
        }
    }
    const auto fw = words_of(rd_add_fq_);
    rd_add_fview_.clear();
    if (!rd_add_typed_.path.empty()) rd_add_fview_.push_back(-1);
    for (size_t i = 0; i < rd_add_files_.size(); ++i) {
        bool ok = true;
        for (const auto& w : fw) if (rd_add_files_[i].lname.find(w) == std::string::npos) { ok = false; break; }
        if (ok) rd_add_fview_.push_back(static_cast<int>(i));
    }
    rd_add_fsel_ = std::clamp(rd_add_fsel_, 0, std::max(0, static_cast<int>(rd_add_fview_.size()) - 1));
    // tracks: the current track is always the first entry
    const auto tw = words_of(rd_add_tq_);
    rd_add_tview_.assign(1, -1);
    if (rd_add_nolyr_ && rd_add_has_lyr_.size() != all_local_tracks_.size()) {   // which tracks have saved lyrics (one look at each lyrics folder entry)
        rd_add_has_lyr_.assign(all_local_tracks_.size(), 0);
        for (size_t i = 0; i < all_local_tracks_.size(); ++i) rd_add_has_lyr_[i] = lyrics_copy_in_use(all_local_tracks_[i].path).empty() ? 0 : 1;
    }
    for (size_t i = 0; i < all_local_tracks_.size(); ++i) {
        if (rd_add_nolyr_ && rd_add_has_lyr_[i]) continue;
        if (!tw.empty()) {
            const std::string hay = ascii_lower_str(rd_add_names_ ? path_utf8(all_local_tracks_[i].path.filename()) : all_local_tracks_[i].folder_artist + " " + all_local_tracks_[i].title);
            bool ok = true;
            for (const auto& w : tw) if (hay.find(w) == std::string::npos) { ok = false; break; }
            if (!ok) continue;
        }
        rd_add_tview_.push_back(static_cast<int>(i));
    }
    rd_add_tsel_ = std::clamp(rd_add_tsel_, 0, static_cast<int>(rd_add_tview_.size()) - 1);
}

fs::path App::readlyrics_add_target() const {   // the locked track
    return rd_add_lock_cur_ ? rd_add_cur_ : rd_add_lock_track_;
}

void App::readlyrics_add_lock_refresh() {
    const fs::path t = readlyrics_add_target();
    rd_add_lock_info_ = t.empty() ? std::string() : lyrics_in_use_summary(t);
    rd_add_manual_ = !t.empty() && lyrics_added_manually(t);
    // A lyrics file that was given to the track by hand shows as locked in (until another one is locked by hand).
    if (rd_add_manual_ && (rd_add_lock_file_.path.empty() || rd_add_lock_auto_)) {
        LyricsFileHit h;
        h.path = lyrics_copy_in_use(t);
        h.name = path_utf8(h.path.stem());
        h.lname = ascii_lower_str(h.name);
        h.folder = "lyrics";
        rd_add_lock_file_ = h;
        rd_add_lock_auto_ = true;
    } else if (rd_add_lock_auto_) {
        rd_add_lock_file_ = LyricsFileHit();
        rd_add_lock_auto_ = false;
    }
}

void App::readlyrics_add_clear() {   // forgets the locked lyrics file and the locked track (the current track is locked in again)
    rd_add_lock_file_ = LyricsFileHit();
    rd_add_lock_auto_ = false;
    rd_add_lock_cur_ = true;
    rd_add_lock_track_.clear();
    rd_add_lock_title_.clear();
    rd_add_warn_ = false;
    rd_add_msg_ = "choice cleared -- the current track is locked in again, lock a lyrics file";
    readlyrics_add_lock_refresh();
}

void App::readlyrics_add_unassign() {
    const fs::path target = readlyrics_add_target();
    rd_add_warn_ = true;
    rd_add_confirm_ = false;
    if (target.empty()) { rd_add_msg_ = "there is no track chosen"; return; }
    std::string err;
    if (!unassign_lyrics_file(target, &err)) { rd_add_msg_ = err; return; }
    log_event("lyrics: unassigned the manually added lyrics of " + path_utf8(target.filename()));
    if (has_track_ && target == current_path_) {
        std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
        launch_lyrics_fetch(metadata_.name, artist, current_path_);   // loads whatever is in use now (no network)
    }
    if (target == rd_track_) {
        rd_versions_ = list_lyrics_versions(rd_track_);
        rd_sel_ = 0;
        for (size_t i = 0; i < rd_versions_.size(); ++i) if (rd_versions_[i].active) { rd_sel_ = static_cast<int>(i); break; }
        rd_first_ = 0;
        readlyrics_load_version();
    }
    rd_add_has_lyr_.clear();   // (the track has no lyrics copy of its own now, perhaps)
    readlyrics_add_rebuild();
    readlyrics_add_lock_refresh();
    rd_add_warn_ = false;
    rd_add_msg_ = "unassigned -- the lyrics file itself is untouched";
}

void App::readlyrics_add_key(int key) {
    const bool arrow = last_key_was_arrow();
    struct Refresh { App* a; ~Refresh() { if (a->rd_add_open_) a->readlyrics_add_lock_refresh(); } } refresh{this};
    if (key == kKeyDelete) {   // DEL: unassign (twice, the second one confirms)
        if (!lyrics_added_manually(readlyrics_add_target())) { rd_add_warn_ = true; rd_add_confirm_ = false; rd_add_msg_ = "the lyrics of the locked track were not added manually -- nothing to unassign"; return; }
        if (!rd_add_confirm_) { rd_add_confirm_ = true; rd_add_warn_ = true; rd_add_msg_ = "unassign the manually added lyrics of this track? [DEL] again confirms, anything else cancels"; return; }
        readlyrics_add_unassign();
        return;
    }
    rd_add_confirm_ = false;
    if (key == 27) { rd_add_open_ = false; force_redraw_ = true; return; }
    if (!arrow && key == '\t') { rd_add_pane_ = (rd_add_pane_ + 1) % 4; rd_add_msg_.clear(); return; }   // file list, track list, the two buttons
    if (key == '\r' || key == '\n') {   // ENTER locks the highlighted lyrics file / track in; on the button it adds
        rd_add_warn_ = false;
        if (rd_add_pane_ == 2) { readlyrics_add_do(); return; }
        if (rd_add_pane_ == 3) { readlyrics_add_clear(); return; }
        if (rd_add_pane_ == 0) {
            if (rd_add_fview_.empty()) return;
            const int fi = rd_add_fview_[static_cast<size_t>(rd_add_fsel_)];
            rd_add_lock_file_ = fi < 0 ? rd_add_typed_ : rd_add_files_[static_cast<size_t>(fi)];
            rd_add_lock_auto_ = false;
            rd_add_msg_ = "locked lyrics file: " + rd_add_lock_file_.name;
        } else {
            const int ti = rd_add_tview_[static_cast<size_t>(rd_add_tsel_)];
            if (ti < 0) { rd_add_lock_cur_ = true; rd_add_msg_ = "locked track: the current track"; }
            else {
                rd_add_lock_cur_ = false;
                rd_add_lock_track_ = all_local_tracks_[static_cast<size_t>(ti)].path;
                rd_add_lock_title_ = readlyrics_add_track_text(all_local_tracks_[static_cast<size_t>(ti)]);
                rd_add_msg_ = "locked track: " + rd_add_lock_title_;
            }
        }
        return;
    }
    // CTRL+c clears the choice (from anywhere, also in the search fields)
    if (key == kKeyCtrlC) { readlyrics_add_clear(); return; }
    // CTRL+t: the track list shows file names / "folder - title"; CTRL+f: only the tracks without saved lyrics (both work in the search fields)
    if (key == kKeyCtrlF) {
        rd_add_nolyr_ = !rd_add_nolyr_;
        rd_add_has_lyr_.clear();
        rd_add_warn_ = false;
        rd_add_tsel_ = 0;
        rd_add_msg_ = rd_add_nolyr_ ? "the track list shows only the tracks without saved lyrics" : "the track list shows all tracks";
        readlyrics_add_rebuild();
        return;
    }
    if (key == kKeyCtrlT) {
        rd_add_names_ = !rd_add_names_;
        rd_add_warn_ = false;
        rd_add_msg_ = rd_add_names_ ? "the track list shows file names" : "the track list shows artist - title";
        readlyrics_add_rebuild();
        return;
    }
    if (arrow && key == 'D' && rd_add_pane_ == 1) { rd_add_pane_ = 0; return; }
    if (arrow && key == 'C' && rd_add_pane_ == 0) { rd_add_pane_ = 1; return; }
    if (rd_add_pane_ >= 2) return;   // (nothing else to do on a button)
    int& sel = rd_add_pane_ == 0 ? rd_add_fsel_ : rd_add_tsel_;
    std::string& q = rd_add_pane_ == 0 ? rd_add_fq_ : rd_add_tq_;
    const int n = rd_add_pane_ == 0 ? static_cast<int>(rd_add_fview_.size()) : static_cast<int>(rd_add_tview_.size());
    const int page = readlyrics_add_list_rows();   // SHIFT+up / down and PgUp / PgDn (reported alike) turn a page
    if (arrow && key == 'A') { sel = std::max(0, sel - (last_key_was_shifted() ? page : 1)); return; }
    if (arrow && key == 'B') { sel = std::min(std::max(0, n - 1), sel + (last_key_was_shifted() ? page : 1)); return; }
    if (arrow) return;
    if (key == kKeyHome) { sel = 0; return; }
    if (key == kKeyEnd) { sel = std::max(0, n - 1); return; }
    if (key == 127 || key == 8) { pop_utf8_char(q); sel = 0; rd_add_msg_.clear(); readlyrics_add_rebuild(); return; }
    if (is_text_key(key) && q.size() < 300) {
        q += static_cast<char>(key);
        sel = 0;
        rd_add_msg_.clear();
        readlyrics_add_rebuild();
    }
}

void App::readlyrics_add_do() {
    rd_add_warn_ = true;
    if (rd_add_lock_file_.path.empty()) { rd_add_msg_ = "lock a lyrics file first: highlight it in the left list and press [ENTER]"; return; }
    const LyricsFileHit file = rd_add_lock_file_;
    const fs::path target = readlyrics_add_target();
    const std::string target_title = rd_add_lock_cur_ ? rd_add_cur_title_ : rd_add_lock_title_;
    if (target.empty()) { rd_add_msg_ = "there is no current track -- lock a track from the right list"; return; }
    std::string err;
    if (!assign_lyrics_file(target, file.path, &err)) { rd_add_msg_ = err; return; }
    log_event("lyrics: assigned " + path_utf8(file.path.filename()) + " to " + target_title);
    rd_add_open_ = false;
    force_redraw_ = true;
    if (has_track_ && target == current_path_) {   // the playing track: its lyrics load now
        std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
        launch_lyrics_fetch(metadata_.name, artist, current_path_);   // loads whatever is in use now (no network)
    }
    if (target == rd_track_) {   // the reader shows this track: the new copy is the one to see
        rd_versions_ = list_lyrics_versions(rd_track_);
        rd_sel_ = 0;
        for (size_t i = 0; i < rd_versions_.size(); ++i) if (rd_versions_[i].active) { rd_sel_ = static_cast<int>(i); break; }
        rd_first_ = 0;
        rd_pane_ = 0;
        readlyrics_load_version();
    }
    rd_status_ = "assigned \"" + path_utf8(file.path.filename()) + "\" to " + target_title + (target == rd_track_ ? "" : " -- open READ LYRICS on that track to read it");
    rd_status_warn_ = false;
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
    auto title = [&](int which, const std::string& t) { return t + (rd_add_pane_ == which ? focus_indicator(settings_, border) : std::string()); };
    // --- the two search fields
    auto search_line = [&](int which, const std::string& q, int w) {
        const bool focus = rd_add_pane_ == which;
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
    both(search_line(0, rd_add_fq_, pw_l), search_line(1, rd_add_tq_, pw_r));
    both(box_bottom(pw_l, "", border), box_bottom(pw_r, "", border));
    // --- the two result lists: whatever height is left
    const int list_rows = readlyrics_add_list_rows();
    const int nf = static_cast<int>(rd_add_fview_.size());
    const int nt = static_cast<int>(rd_add_tview_.size());
    auto cell = [&](int which, int w, int r, int sel, int n, const std::function<std::string(int)>& text, const std::function<bool(int)>& locked, const std::string& empty_text) {
        const bool focus = rd_add_pane_ == which;
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
    both(box_top(title(0, "LYRICS FILES"), pw_l, border), box_top(title(1, std::string("TRACKS (") + (rd_add_names_ ? "file names" : "artist - title") + (rd_add_nolyr_ ? ", no lyrics" : "") + ")"), pw_r, border));
    const std::string fempty = rd_add_files_.empty() ? "no lyrics files found -- type a path" : "no match -- or type a full path";
    for (int r = 0; r < list_rows; ++r) {
        const std::string l = cell(0, pw_l, r, rd_add_fsel_, nf, [&](int i) {
            const int fi = rd_add_fview_[static_cast<size_t>(i)];
            const LyricsFileHit& h = fi < 0 ? rd_add_typed_ : rd_add_files_[static_cast<size_t>(fi)];
            return h.name + "  (" + h.folder + ")";
        }, [&](int i) {
            const int fi = rd_add_fview_[static_cast<size_t>(i)];
            const LyricsFileHit& h = fi < 0 ? rd_add_typed_ : rd_add_files_[static_cast<size_t>(fi)];
            return !rd_add_lock_file_.path.empty() && h.path == rd_add_lock_file_.path;
        }, fempty);
        const std::string rr = cell(1, pw_r, r, rd_add_tsel_, nt, [&](int i) { return readlyrics_add_track_label(rd_add_tview_[static_cast<size_t>(i)]); }, [&](int i) {
            const int ti = rd_add_tview_[static_cast<size_t>(i)];
            return rd_add_lock_cur_ ? ti < 0 : (ti >= 0 && all_local_tracks_[static_cast<size_t>(ti)].path == rd_add_lock_track_);
        }, "");
        both(l, rr);
    }
    {   // the count lines, under each list
        const auto l = cut(std::to_string(nf) + " of " + std::to_string(rd_add_files_.size()) + " files in " + std::to_string(rd_add_dirs_) +
                           (rd_add_dirs_ == 1 ? " lyrics folder" : " lyrics folders"), pw_l, leg);
        const auto r = cut(std::to_string(nt - 1) + " of " + std::to_string(all_local_tracks_.size()) + " tracks  (+ the current track)", pw_r, leg);
        both(pline(l.first, l.second, pw_l), pline(r.first, r.second, pw_r));
    }
    both(box_bottom(pw_l, "", border), box_bottom(pw_r, "", border));
    // --- the locked-in fields
    both(box_top("LOCKED IN: LYRICS FILE", pw_l, border), box_top("LOCKED IN: TRACK", pw_r, border));
    {
        const bool has_f = !rd_add_lock_file_.path.empty();
        const auto l1 = cut(has_f ? rd_add_lock_file_.name : "(none yet -- highlight a file, press [ENTER])", pw_l, has_f ? hdr : std::string());
        const std::string tt = rd_add_lock_cur_ ? (rd_add_cur_.empty() ? std::string("(no current track -- lock one from the list)") : "Current track:  " + rd_add_cur_title_) : rd_add_lock_title_;
        const auto r1 = cut(tt, pw_r, hdr);
        both(pline(l1.first, l1.second, pw_l), pline(r1.first, r1.second, pw_r));
        // the path of the file, then the note for lyrics that were added by hand; on a narrow pane the note wraps over both rows
        const std::string note = "Added manually; hit DEL to unassign";
        std::string row2 = has_f ? path_utf8(rd_add_lock_file_.path) : std::string(), row3;
        std::string sgr2 = leg, sgr3 = hdr;
        if (rd_add_manual_) {
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
        std::string info = rd_add_lock_info_.empty() ? std::string("no lyrics saved for this track yet") : "has lyrics: " + rd_add_lock_info_;
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
        const std::string p1 = rd_add_pane_ == 2 ? "\x1b[7m" + b1 + R : hdr + b1 + R;
        const std::string p2 = rd_add_pane_ == 3 ? "\x1b[7m" + b2 + R : hdr + b2 + R;
        out.push_back(bar + " " + std::string(static_cast<size_t>(lead), ' ') + p1 + std::string(static_cast<size_t>(gap), ' ') + p2 +
                      std::string(static_cast<size_t>(std::max(0, inner - lead - w1 - gap - w2)), ' ') + " " + bar);
    }
    out.push_back(bar + " " + (rd_add_warn_ && !rd_add_msg_.empty() ? std::string("\x1b[31m") : leg) + pad_right(truncate_str(rd_add_msg_, PW - 4), PW - 4) + R + " " + bar);
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
        {"Tracks", {"[CTRL+t] File names / artist - title\001File names", "[CTRL+f] Only tracks without lyrics\001No lyrics"}},
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
    row("SERVICE", translate_service_name(settings_.translate_service), rd_opts_row_ == 0);
    row("TRANSLATE INTO", language_name(settings_.translate_to), rd_opts_row_ == 1);
    row("LYRICS ARE IN", from_name, rd_opts_row_ == 2);
    plain("", false);
    const std::string& svc = settings_.translate_service;
    plain(svc == "google" ? "Google Translate's key-less web endpoint (unofficial)."
                          : "MyMemory: free, about 5000 characters a day per address.", true);
    plain(settings_.translate_from == "auto" ? "If detection is wrong, set LYRICS ARE IN by hand." : "The language of the lyrics is set by hand.", true);
    if (!rd_opts_warn_.empty()) plain(rd_opts_warn_, false, "\x1b[31m");
    else if (rd_busy_) plain("Running: " + language_name(rd_job_to_) + " with " + translate_service_name(rd_job_svc_) + " (goes on in the background)", true);
    else plain("", false);
    out.push_back(box_bottom(PW, "[↑↓] Row  [←→] Change  [ENTER] Translate  [ESC] Close", border_bottom));
    return out;
}

// ---------------------------------------------------------------------
// Export ([e]): the lyrics as a text file in the "export" folder inside the lyrics folder: the original only, or with the saved
// translation and / or the Latin spelling under each line.
// ---------------------------------------------------------------------
int App::readlyrics_export_choices(std::vector<int>& flags, std::vector<std::string>& names) const {
    const bool has_tr = !rd_tlist_.empty() && rd_tsel_ >= 0 && rd_tsel_ < static_cast<int>(rd_tlist_.size()) && rd_trans_.size() == rd_lines_.size();
    const bool has_tl = lyrics_export_translit_differs(rd_lines_);
    const std::string tr_name = has_tr ? language_name(rd_tlist_[static_cast<size_t>(rd_tsel_)].to) : std::string();
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
    if (rd_sel_ >= 0 && rd_sel_ < static_cast<int>(rd_versions_.size()) && !rd_versions_[static_cast<size_t>(rd_sel_)].file.empty()) {
        const fs::path f = rd_versions_[static_cast<size_t>(rd_sel_)].file;   // a fetched / older copy: its own lyrics folder
        folder = f.parent_path();
        if (folder.filename() != "lyrics") folder = lyrics_folder_of(rd_track_);
    }
    if (folder.empty()) folder = lyrics_folder_of(rd_track_);
    if (folder.empty() && !rd_track_.empty()) folder = rd_track_.parent_path() / "lyrics";
    return folder / "export";
}

fs::path App::readlyrics_export_file(int flags) const {
    std::string stem = path_utf8(rd_track_.stem());
    if (stem.empty() && rd_sel_ >= 0 && rd_sel_ < static_cast<int>(rd_versions_.size())) stem = path_utf8(rd_versions_[static_cast<size_t>(rd_sel_)].file.stem());
    if (stem.empty()) stem = rd_title_;
    std::string tag;
    const std::string to = (flags & 1) && rd_tsel_ >= 0 && rd_tsel_ < static_cast<int>(rd_tlist_.size()) ? rd_tlist_[static_cast<size_t>(rd_tsel_)].to : std::string();
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
    const int flags = std::find(fl.begin(), fl.end(), rd_exp_choice_) != fl.end() ? rd_exp_choice_ : 0;
    if ((flags & 2) && readlyrics_reader_working() && !rd_exp_wait_) {   // the Latin spelling of kanji is not there yet: say so once
        const size_t left = translit_kanji_waiting(rd_lines_);
        if (left > 0) {
            rd_exp_wait_ = true;
            rd_exp_msg_ = "Still reading the kanji (" + std::to_string(left) + " lines left): wait a moment, or ENTER again to export as it is";
            rd_exp_warn_ = true;
            return;
        }
    }
    rd_exp_wait_ = false;
    const std::string text = lyrics_export_text(rd_lines_, (flags & 1) ? &rd_trans_ : nullptr, (flags & 2) != 0);
    const fs::path file = readlyrics_export_file(flags);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (text.empty() || ec || !write_text_atomic(file, text)) {
        rd_status_ = ec ? "could not create the export folder: " + path_utf8(file.parent_path()) : text.empty() ? "nothing to export" : "could not write " + path_utf8(file);
        rd_status_warn_ = true;
    } else {
        rd_status_ = "exported to " + path_utf8(file);
        rd_status_warn_ = false;
        log_event("lyrics exported: " + path_utf8(file));
    }
    rd_exp_msg_ = rd_status_warn_ ? rd_status_ : "Exported: " + path_utf8(file.filename());   // the overlay stays open and says what happened
    rd_exp_warn_ = rd_status_warn_;
}

// The export overlay: LEFT / RIGHT choose what the file holds, ENTER writes it, ESC closes.
void App::readlyrics_export_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (key == 27 || key_is("HKeyReadLyrics", key) || (!arrow && (key == 'q' || key == 'Q' || key == 'e' || key == 'E'))) { rd_exp_open_ = false; return; }
    if (key == '\r' || key == '\n') { readlyrics_export_do(); return; }
    std::vector<int> fl; std::vector<std::string> nm;
    const int n = readlyrics_export_choices(fl, nm);
    int idx = 0;
    for (int i = 0; i < n; ++i) if (fl[static_cast<size_t>(i)] == rd_exp_choice_) idx = i;
    if (arrow && (key == 'C' || key == 'B')) idx = (idx + 1) % n;
    else if (arrow && (key == 'D' || key == 'A')) idx = (idx + n - 1) % n;
    rd_exp_choice_ = fl[static_cast<size_t>(idx)];
    rd_exp_msg_.clear();
    rd_exp_wait_ = false;
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
    for (int i = 0; i < n; ++i) if (fl[static_cast<size_t>(i)] == rd_exp_choice_) idx = i;
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
    const size_t unread = (fl[static_cast<size_t>(idx)] & 2) && readlyrics_reader_working() ? translit_kanji_waiting(rd_lines_) : 0;
    if (!rd_exp_msg_.empty()) { plain(rd_exp_msg_, !rd_exp_warn_); }
    else if (unread > 0) plain("Reading the kanji with pykakasi ... " + std::to_string(unread) + " lines left -- wait a moment", true);
    else if (rd_tlist_.empty()) plain("[t] fetches a translation; then it can be exported too.", true);
    else if (n < 3 && !lyrics_export_translit_differs(rd_lines_)) plain("These lyrics have no kana / Hangul / Chinese characters to spell in Latin letters.", true);
    else plain("", false);
    out.push_back(box_bottom(PW, "[\u2190\u2192] Choose  [ENTER] Export  [ESC] Close", border_bottom));
    return out;
}

void App::build_readlyrics_screen(std::ostringstream& frame, int W, int target_height) {
    if (W < 60) W = 60;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const int inner = W - 4;
    auto boxed = [&](const std::string& painted) { return bar + " " + painted + " " + bar; };

    // One line of choices that scrolls sideways: the lyrics files / the translations. `sel` is highlighted.
    auto chooser = [&](const std::vector<std::string>& items, int& sel, int& first) {
        const int n = static_cast<int>(items.size());
        const int avail = std::max(10, inner - 4);   // the two 2-cell scroll marks
        const std::string sep_txt = " " + (settings_.list_separator.empty() ? std::string("|") : settings_.list_separator) + " ";
        const int sw = display_width(sep_txt);
        auto iw = [&](int i) { return std::min(avail, display_width(items[static_cast<size_t>(i)])); };
        sel = std::clamp(sel, 0, n - 1);
        first = std::clamp(first, 0, n - 1);
        if (sel < first) first = sel;
        auto span_to_sel = [&](int f) { int w = 0; for (int i = f; i <= sel; ++i) w += iw(i) + (i > f ? sw : 0); return w; };
        while (first < sel && span_to_sel(first) > avail) ++first;
        std::vector<int> shown;
        int used = 0;
        for (int i = first; i < n; ++i) {
            const int add = iw(i) + (i > first ? sw : 0);
            if (used + add > avail) break;
            used += add;
            shown.push_back(i);
        }
        std::string mid;
        for (size_t k = 0; k < shown.size(); ++k) {
            const int i = shown[k];
            if (k > 0) mid += border + sep_txt + R;
            const std::string txt = truncate_str(items[static_cast<size_t>(i)], avail);
            mid += (i == sel) ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + txt + R : txt;
        }
        mid += std::string(static_cast<size_t>(std::max(0, avail - used)), ' ');
        const bool more_left = first > 0;
        const bool more_right = !shown.empty() && shown.back() < n - 1;
        return border + (more_left ? "◀ " : "  ") + R + mid + border + (more_right ? " ▶" : "  ") + R;
    };

    // --- top pane 1: ONE line with the lyrics files of the track ---
    frame << box_top("READ LYRICS" + (rd_pane_ == 0 ? focus_indicator(settings_, border) : std::string()), W, border) << "\n";
    {
        std::string line;
        if (rd_versions_.empty()) {
            line = pad_right("no lyrics saved for this track yet -- they are fetched while it plays", inner);
        } else {
            std::vector<std::string> items;
            for (const auto& v : rd_versions_) {
                std::string t = (v.active ? "● " : "") + v.label;
                if (!v.detail.empty()) t += " (" + v.detail + ")";
                items.push_back(t);
            }
            line = chooser(items, rd_sel_, rd_first_);
        }
        frame << boxed(line) << "\n";
    }
    frame << box_bottom(W, n_files_label(rd_versions_.size()), border_bottom) << "\n";

    // --- top pane 2: ONE line with the translations saved for the chosen file ---
    const bool show = rd_show_ && !rd_tlist_.empty() && rd_trans_.size() == rd_lines_.size();
    frame << box_top("TRANSLATION" + (rd_pane_ == 1 ? focus_indicator(settings_, border) : std::string()), W, border) << "\n";
    {
        std::string line;
        if (rd_tlist_.empty()) {
            line = pad_right(rd_lines_.empty() ? "nothing to translate" : "no translation yet -- [t] opens the translate overlay", inner);
        } else {
            std::vector<std::string> items;
            for (size_t i = 0; i < rd_tlist_.size(); ++i) {
                const TranslationInfo& t = rd_tlist_[i];
                items.push_back(std::string(show && static_cast<int>(i) == rd_tsel_ ? "● " : "") + language_name(t.to) +
                                (t.service.empty() ? std::string() : " (" + translate_service_name(t.service) + ")"));
            }
            line = chooser(items, rd_tsel_, rd_tfirst_);
        }
        frame << boxed(line) << "\n";
    }
    {
        std::string label;
        if (rd_busy_) label = "translating into " + language_name(rd_job_to_) + " with " + translate_service_name(rd_job_svc_) + " ...";
        else if (!rd_tlist_.empty()) label = std::to_string(rd_tlist_.size()) + (rd_tlist_.size() == 1 ? " translation" : " translations") + (show ? "" : " (hidden)");
        frame << box_bottom(W, label, border_bottom) << "\n";
    }

    // --- footer first (the lyrics pane takes the rows it leaves) ---
    // The legend is a small tree table (Move / Translate / Session), three rows whatever the width; the same in both panes
    // (TAB only moves the focus indicator): the keys that act on the focused pane say so in one entry.
    std::vector<std::string> hint_lines;
    if (rd_confirm_del_ && rd_tsel_ >= 0 && rd_tsel_ < static_cast<int>(rd_tlist_.size())) {
        hint_lines.push_back(legend_sgr(settings_) + truncate_str(" Delete the " + language_name(rd_tlist_[static_cast<size_t>(rd_tsel_)].to) + " translation (" + translate_service_name(rd_tlist_[static_cast<size_t>(rd_tsel_)].service) + ")?  [y] Yes | [any other key] No", W) + "\x1b[0m");
        hint_lines.push_back(std::string());
        hint_lines.push_back(std::string());
    } else {
        std::vector<LegendGroup> groups = {
            {"Move", {"[\u2190\u2192] Choose file/Translation\001Choose", "[TAB] Focus", "[\u2191\u2193] Scroll", "[SHIFT+\u2191\u2193] Page up/down\001Page", "[HOME/END] Top/Bottom"}},
            {"Play", {"[p] Play/Pause", "[SHIFT+\u2190\u2192] Seek 5 s\001Seek", "[f] Follow the song\001Follow", "[" + hotkey_text("HKeyLyricsTiming", "ALT+L") + "] Lyrics timing\001Timing"}},
            {"Translate", {"[t] Translate", "[SHIFT+t] Show/Hide translations\001Show/Hide", "[d/DEL] Delete translation\001Delete", "[e] Export"}},
            {"Session", {"[CTRL+s] Search", "[" + hotkey_text("HKeyRetryLyrics", "l") + "] Re-fetch lyrics\001Re-fetch", "[a] Add manually\001Add", "[ESC] Close"}},
        };
        const auto lrows = layout_legend_tree(groups, W - 1);
        for (size_t i = 0; i < lrows.size(); ++i) hint_lines.push_back(" " + paint_legend(settings_, lrows, i, border, 0, true));
    }
    const int legend_rows = static_cast<int>(hint_lines.size());
    const int status_rows = 0;   // no status line here: the status message and the log overlay show rd_status_ (it used to change the size of the panes)
    const int budget = std::min(target_height, term_rows_ - 1);
    const int body = std::max(3, budget - 6 /*two top panes*/ - 2 /*lyrics pane borders*/ - legend_rows - status_rows);
    // two blank rows above and below the lyrics (none on a terminal too short for them)
    const int margin = body >= 10 ? 2 : 0;
    const int text_rows = body - 2 * margin;
    const int wrap_w = std::max(10, inner - 8);   // a little air left and right: the lines are centred

    // --- lyrics pane ---
    // The track of the reader is the loaded one: its lyrics are drawn the way the lyrics engine draws them (the active line, the words that
    // are sung, the animation of Settings), the view follows the song, and the translation of the active line is lit with it.
    struct Row { std::string text; int style; bool pre = false; };   // 0 original, 1 translation, 2 blank, 3 translation of the active line; pre: painted already (wrap_w wide)
    const bool plays = readlyrics_track_plays() && rd_timed_.size() == rd_lines_.size() && !rd_lines_.empty();
    const double elapsed = plays ? player_.poll_elapsed() : 0.0;
    bool synced = false;
    for (const LyricLine& l : rd_timed_) if (l.start_time >= 0.0) { synced = true; break; }
    double offset = rd_delay_;
    if (mode_ == Mode::LyricsEdit && lyrics_edit_from_reader_) { std::lock_guard<std::mutex> lk(lyrics_mutex_); offset = lyrics_result_.delay; }   // the timing overlay is open: live
    const double lt = elapsed - offset;
    int active = -1;
    if (plays) {
        if (synced) {
            for (size_t i = 0; i < rd_timed_.size(); ++i) if (rd_timed_[i].start_time >= 0.0 && rd_timed_[i].start_time <= lt) active = static_cast<int>(i);
        } else if (total_sec_ > 0) {   // plain lyrics: spread over the track
            active = std::clamp(static_cast<int>(elapsed / static_cast<double>(total_sec_) * static_cast<double>(rd_lines_.size())), 0, static_cast<int>(rd_lines_.size()) - 1);
        }
    }
    Settings ks = settings_;   // centred whole-line look, as in the karaoke view
    ks.lyrics_alignment = 0;
    std::vector<Row> rows;
    int active_row = -1;
    for (size_t i = 0; i < rd_lines_.size(); ++i) {
        if (rd_lines_[i].empty()) { rows.push_back({std::string(), 2}); continue; }
        const bool is_active = plays && static_cast<int>(i) == active;
        if (is_active) active_row = static_cast<int>(rows.size());
        if (plays) {
            LyricLine ll = rd_timed_[i];
            if (translit_enabled()) { ll.full_text = transliterate(ll.full_text); for (auto& w : ll.words) w.second = transliterate(w.second); }
            const bool timed = synced && ll.start_time >= 0.0;
            for (auto& w : render_lyric_line_wrapped(ll, timed ? lt : 1e12, wrap_w, is_active, ks)) rows.push_back({w, 0, true});
        } else {
            for (auto& w : wrap_lines(shown_text(rd_lines_[i]), wrap_w, 8)) rows.push_back({w, 0});
        }
        if (show) {
            const std::string& t = rd_trans_[i];
            if (!t.empty() && ascii_lower_str(t) != ascii_lower_str(rd_lines_[i]))
                for (auto& w : wrap_lines("(" + shown_text(t) + ")", wrap_w, 8)) rows.push_back({w, is_active ? 3 : 1});
        }
    }
    if (rd_lines_.empty()) {
        rows.push_back({rd_versions_.empty() ? "no lyrics saved for this track" : "this file has no readable lyrics", 2});
    }
    if (plays && rd_follow_ && active_row >= 0) rd_scroll_ = std::max(0, active_row - text_rows / 2 + 1);   // (clamped below)
    const int max_scroll = std::max(0, static_cast<int>(rows.size()) - text_rows);
    rd_scroll_ = std::clamp(rd_scroll_, 0, max_scroll);

    std::string info;
    if (!rows.empty() && !rd_lines_.empty())
        info = "row " + std::to_string(std::min<int>(rd_scroll_ + text_rows, static_cast<int>(rows.size()))) + "/" + std::to_string(rows.size());
    if (plays) info += std::string(info.empty() ? "" : "  ") + (player_.is_paused() ? "paused " : "playing ") + fmt_mmss(elapsed) + " / " + fmt_mmss(static_cast<double>(total_sec_)) +
                       (rd_follow_ ? "" : "  (follow off: [f])");
    const std::string sep2 = info.empty() ? "" : "  ";
    if (show) info += sep2 + "translation: " + language_name(rd_from_) + " > " + language_name(rd_to_) +
                      (rd_svc_.empty() ? std::string() : " (" + translate_service_name(rd_svc_) + ")");
    else if (rd_same_ && rd_tlist_.empty()) info += sep2 + "already " + language_name(settings_.translate_to);
    else if (!rd_tlist_.empty()) info += sep2 + "translation hidden -- [SHIFT+t] shows it";

    frame << box_top(truncate_str(rd_title_, std::max(10, W - 8)), W, border) << "\n";
    const std::string orig_sgr = ansi_for(settings_.active_line_color, false);
    const std::string trans_sgr = ansi_for(settings_.inactive_line_color, false);
    for (int k = 0; k < body; ++k) {
        const int idx = rd_scroll_ + k - margin;
        std::string painted;
        if (k >= margin && k < body - margin && idx >= 0 && idx < static_cast<int>(rows.size())) {
            const Row& r = rows[static_cast<size_t>(idx)];
            if (r.pre) {   // painted by the lyrics engine, wrap_w columns wide
                const int lead = std::max(0, (inner - wrap_w) / 2);
                painted = std::string(static_cast<size_t>(lead), ' ') + r.text + R + std::string(static_cast<size_t>(std::max(0, inner - lead - wrap_w)), ' ');
            } else {
                const std::string cut = truncate_str(r.text, inner);
                const int lead = std::max(0, (inner - display_width(cut)) / 2);   // centred
                const std::string t = pad_right(std::string(static_cast<size_t>(lead), ' ') + cut, inner);
                painted = r.style == 0 ? orig_sgr + t + R : r.style == 1 ? trans_sgr + t + R : r.style == 3 ? orig_sgr + t + R : t;
            }
        } else {
            painted = std::string(static_cast<size_t>(inner), ' ');
        }
        frame << boxed(painted) << "\n";
    }
    frame << box_bottom(W, truncate_str(info, std::max(4, W - 6)), border_bottom) << "\n";

    for (const auto& l : hint_lines) frame << l << "\n";
}


} // namespace muisc
