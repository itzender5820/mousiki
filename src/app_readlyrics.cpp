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
    rd_->track = track;
    rd_->opened_hit = LyricsFileHit();
    rd_->title = title.empty() ? path_utf8(track.stem()) : title;
    rd_->versions = list_lyrics_versions(track);
    rd_->sel = 0;
    for (size_t i = 0; i < rd_->versions.size(); ++i) if (rd_->versions[i].active) { rd_->sel = static_cast<int>(i); break; }
    rd_->first = 0;
    rd_->pane = 0;
    rd_->confirm_del = false;
    rd_->opts_open = false;
    rd_->exp_open = false;
    rd_->search_open = false;
    rd_->add_open = false;
    rd_->te_open = false;
    rd_->opts_warn.clear();
    rd_->status.clear();
    rd_->status_warn = false;
    rd_->follow = true;
    if (rd_->busy) { rd_->status = "a translation is still running (" + language_name(rd_->job_to) + " with " + translate_service_name(rd_->job_svc) + ")"; }
    readlyrics_load_version();
    mode_ = Mode::ReadLyrics;
    force_redraw_ = true;   // full-screen takeover
}

void App::readlyrics_close() {
    if (rd_->opts_dirty) { save_settings(settings_); rd_->opts_dirty = false; }
    rd_->opts_open = false;
    rd_->exp_open = false;
    rd_->search_open = false;
    rd_->add_open = false;
    rd_->te_open = false;
    rd_->confirm_del = false;
    // A translation that is still running is NOT stopped: it is saved when done (readlyrics_poll() tells).
    mode_ = Mode::Browse;
    force_redraw_ = true;
}

void App::readlyrics_load_version() {
    rd_->lines.clear();
    rd_->timed.clear();
    rd_->delay = 0.0;
    rd_->trans.clear();
    rd_->tlist.clear();
    rd_->from.clear();
    rd_->show = rd_->same = false;
    rd_->scroll = 0;
    rd_->tsel = rd_->tfirst = 0;
    rd_->confirm_del = false;
    if (rd_->sel < 0 || rd_->sel >= static_cast<int>(rd_->versions.size())) return;
    const LyricsVersion& v = rd_->versions[static_cast<size_t>(rd_->sel)];
    rd_->timed = lyrics_read_lines(read_lyrics_version_text(rd_->track, v), &rd_->delay);
    for (const LyricLine& l : rd_->timed) rd_->lines.push_back(l.full_text);
    translit_kanji_waiting(rd_->lines);   // (asks the kanji reader for every line now: an export later does not have to wait for it)
    readlyrics_refresh_translations(settings_.translate_to);
    rd_->show = !rd_->tlist.empty();   // a translation that is there is shown at once
}

bool App::readlyrics_track_plays() const {
    return has_track_ && !current_path_.empty() && current_path_ == rd_->track;
}

// [p]: the track of the reader starts (the library entry, or the file when it is not in the library); when it is the loaded one already
// it pauses / resumes. The reader stays open and follows the song.
void App::readlyrics_play() {
    rd_->status_warn = false;
    if (readlyrics_track_plays()) {
        act_play_pause();
        rd_->status = player_.is_paused() ? "paused" : "playing";
        return;
    }
    std::error_code ec;
    if (rd_->track.empty() || !fs::exists(rd_->track, ec)) {
        rd_->status = "this lyrics file has no track to play (it came from the search) -- open the reader for a track with CTRL+L";
        rd_->status_warn = true;
        return;
    }
    LocalTrack t;
    bool found = false;
    for (const auto& lt : all_local_tracks_) if (lt.path == rd_->track) { t = lt; found = true; break; }
    if (!found) {
        t.path = rd_->track;
        t.title = path_utf8(rd_->track.stem());
        t.folder_artist = path_utf8(rd_->track.parent_path().filename());
    }
    start_local_track(t);
    rd_->follow = true;
    rd_->status = "playing: " + t.title;
}

// Reads the saved translations of the chosen file again; `prefer_to` is the language to select when it is there.
void App::readlyrics_refresh_translations(const std::string& prefer_to, const std::string& prefer_svc) {
    rd_->tlist.clear();
    rd_->same = false;
    if (rd_->lines.empty() || rd_->sel < 0 || rd_->sel >= static_cast<int>(rd_->versions.size())) { readlyrics_select_translation(-1); return; }
    const LyricsVersion& v = rd_->versions[static_cast<size_t>(rd_->sel)];
    rd_->tlist = list_lyrics_translations(rd_->track, v, rd_->lines);
    int pick = rd_->tlist.empty() ? -1 : 0;
    // the preferred language; among several services the preferred one (the one just used, else the one chosen in the options)
    const std::string svc = prefer_svc.empty() ? settings_.translate_service : prefer_svc;
    for (size_t i = 0; i < rd_->tlist.size(); ++i)
        if (rd_->tlist[i].to == prefer_to && (rd_->tlist[static_cast<size_t>(pick)].to != prefer_to || rd_->tlist[i].service == svc)) pick = static_cast<int>(i);
    readlyrics_select_translation(pick);
    LyricsTranslation note;   // "these lyrics are in <language> already", saved by an earlier try
    if (load_lyrics_translation(rd_->track, v, settings_.translate_to, rd_->lines, note, settings_.translate_service) && note.same) rd_->same = true;
}

void App::readlyrics_select_translation(int index) {
    rd_->trans.clear();
    rd_->from.clear();
    rd_->to.clear();
    rd_->svc.clear();
    if (index < 0 || index >= static_cast<int>(rd_->tlist.size()) || rd_->sel < 0 || rd_->sel >= static_cast<int>(rd_->versions.size())) {
        rd_->tsel = 0;
        return;
    }
    rd_->tsel = index;
    const TranslationInfo& info = rd_->tlist[static_cast<size_t>(index)];
    LyricsTranslation t;
    if (load_lyrics_translation(rd_->track, rd_->versions[static_cast<size_t>(rd_->sel)], info.to, rd_->lines, t, info.service) && !t.same) {
        rd_->trans = std::move(t.lines);
        rd_->from = t.from;
        rd_->svc = t.service;
        rd_->to = info.to;
    }
}

// SHIFT+t: hidden -> the chosen translation -> the next one ... -> hidden. Never fetches anything.
void App::readlyrics_toggle_translations() {
    rd_->status_warn = false;
    if (rd_->tlist.empty()) {
        rd_->status = rd_->busy ? "no translation yet -- the running one is not finished" : "no translation saved for this file -- [t] fetches one";
        rd_->status_warn = true;
        return;
    }
    const int n = static_cast<int>(rd_->tlist.size());
    if (!rd_->show) {
        rd_->show = true;
    } else if (rd_->tsel < n - 1) {
        readlyrics_select_translation(rd_->tsel + 1);
    } else {
        rd_->show = false;
        readlyrics_select_translation(0);
    }
    if (rd_->show) {
        const TranslationInfo& t = rd_->tlist[static_cast<size_t>(rd_->tsel)];
        rd_->status = "showing the " + language_name(t.to) + " translation" + (n > 1 ? " (" + std::to_string(rd_->tsel + 1) + "/" + std::to_string(n) + ", SHIFT+t for the next)" : std::string());
    } else {
        rd_->status = "translations hidden";
    }
}

void App::readlyrics_delete_translation() {
    rd_->confirm_del = false;
    if (rd_->tsel < 0 || rd_->tsel >= static_cast<int>(rd_->tlist.size()) || rd_->sel < 0 || rd_->sel >= static_cast<int>(rd_->versions.size())) return;
    const std::string to = rd_->tlist[static_cast<size_t>(rd_->tsel)].to, svc = rd_->tlist[static_cast<size_t>(rd_->tsel)].service;
    const bool ok = delete_lyrics_translation(rd_->track, rd_->versions[static_cast<size_t>(rd_->sel)], to, svc);
    const int keep = rd_->tsel;
    readlyrics_refresh_translations("");
    readlyrics_select_translation(rd_->tlist.empty() ? -1 : std::min(keep, static_cast<int>(rd_->tlist.size()) - 1));
    if (rd_->tlist.empty()) rd_->show = false;
    rd_->status_warn = !ok;
    rd_->status = ok ? "deleted the " + language_name(to) + " translation (" + translate_service_name(svc) + ")" : "could not delete the " + language_name(to) + " translation";
}

// ENTER in the translate overlay: fetch a translation with the service and languages set there.
void App::readlyrics_translate() {
    rd_->opts_warn.clear();
    if (rd_->busy) {
        rd_->opts_warn = "WARNING: the last translation is not finished yet -- please wait";
        return;
    }
    if (rd_->lines.empty() || rd_->sel < 0 || rd_->sel >= static_cast<int>(rd_->versions.size())) {
        rd_->opts_warn = "there are no lyrics to translate";
        return;
    }
    TranslateRequest req;
    req.service = settings_.translate_service;
    req.from = settings_.translate_from;
    req.to = settings_.translate_to;
    rd_->opts_open = false;
    if (rd_->opts_dirty) { save_settings(settings_); rd_->opts_dirty = false; }
    rd_->busy = true;
    rd_->job_to = req.to;
    rd_->job_svc = req.service;
    rd_->job_track = rd_->track;
    rd_->job_version = rd_->versions[static_cast<size_t>(rd_->sel)];
    rd_->status_warn = false;
    rd_->status = "translating into " + language_name(req.to) + " with " + translate_service_name(req.service) +
                 " ... this takes a while (needs an internet connection); you can go on reading";
    const std::vector<std::string> plain = rd_->lines;
    const LyricsVersion v = rd_->job_version;
    const fs::path track = rd_->track, script = translate_script_;
    spawn_bg("lyrics translation", [this, plain, v, track, script, req]() {
        LyricsTranslation t = translate_lyrics_lines(plain, req, script);
        if (t.ok) save_lyrics_translation(track, v, req.to, plain, t);
        std::lock_guard<std::mutex> lk(rd_->mutex);
        rd_->result = std::move(t);
        rd_->result_ready = true;
    });
}

void App::readlyrics_poll() {
    // The re-fetch overlay was used over the reader: when its fetch is done, the reader shows the lyrics now in use.
    if (rd_->watch_epoch >= 0 && mode_ == Mode::ReadLyrics && lyrics_epoch_.load() != rd_->watch_epoch) {
        bool done = false;
        {
            std::lock_guard<std::mutex> lk(lyrics_mutex_);
            done = lyrics_ready_ && lyrics_path_ == rd_->track;
        }
        if (done) {
            rd_->watch_epoch = -1;
            rd_->versions = list_lyrics_versions(rd_->track);
            rd_->sel = 0;
            for (size_t i = 0; i < rd_->versions.size(); ++i) if (rd_->versions[i].active) { rd_->sel = static_cast<int>(i); break; }
            rd_->first = 0;
            readlyrics_load_version();
            rd_->status = rd_->lines.empty() ? "no lyrics found for this track" : "lyrics loaded: " + std::to_string(rd_->lines.size()) + " lines";
            rd_->status_warn = rd_->lines.empty();
        }
    }
    LyricsTranslation t;
    {
        std::lock_guard<std::mutex> lk(rd_->mutex);   // (the flag is written by the worker thread under this lock: never read it without)
        if (!rd_->result_ready) return;
        t = std::move(rd_->result);
        rd_->result = LyricsTranslation{};
        rd_->result_ready = false;
    }
    rd_->busy = false;
    const std::string what = language_name(rd_->job_to);
    // Is the file the translation was made for the one on the screen?
    const bool on_screen = mode_ == Mode::ReadLyrics && rd_->sel >= 0 && rd_->sel < static_cast<int>(rd_->versions.size()) &&
        lyrics_translation_file(rd_->track, rd_->versions[static_cast<size_t>(rd_->sel)], "xx") == lyrics_translation_file(rd_->job_track, rd_->job_version, "xx");
    std::string msg;
    bool warn = false;
    if (!t.ok) { msg = "translation into " + what + " failed: " + t.error; warn = true; }
    else if (t.same) msg = "these lyrics are in " + what + " already -- nothing to translate ([t] changes the languages)";
    else msg = "translated from " + language_name(t.from) + " to " + what + " with " + translate_service_name(rd_->job_svc) + " -- saved in the lyrics folder (translations)";
    if (on_screen) {
        if (t.ok) {
            readlyrics_refresh_translations(rd_->job_to, rd_->job_svc);
            if (!t.same && !rd_->tlist.empty()) rd_->show = true;
            if (t.same) rd_->same = true;
        }
        rd_->status = msg;
        rd_->status_warn = warn;
    } else {
        status_line_ = "lyrics: " + msg;      // READ LYRICS is closed (or shows another file): tell on the main screen
        if (mode_ == Mode::ReadLyrics) { rd_->status = msg; rd_->status_warn = warn; }
    }
}

// The translate overlay ([t]): UP / DOWN pick the row, LEFT / RIGHT change it (a letter jumps to the next language
// starting with it), ENTER fetches the translation, ESC closes it. A running translation is not touched by any of it.
void App::readlyrics_options_key(int key) {
    const bool arrow = last_key_was_arrow();
    auto close_panel = [&]() {
        rd_->opts_open = false;
        rd_->opts_warn.clear();
        if (rd_->opts_dirty) { save_settings(settings_); rd_->opts_dirty = false; }
    };
    if (key == 27 || key_is("HKeyReadLyrics", key)) { close_panel(); return; }
    if (key == '\r' || key == '\n') { readlyrics_translate(); return; }
    if (arrow && key == 'A') { rd_->opts_row = std::max(0, rd_->opts_row - 1); return; }
    if (arrow && key == 'B') { rd_->opts_row = std::min(2, rd_->opts_row + 1); return; }
    // the value lists: 0 services, 1 target languages, 2 source languages ("auto" first)
    std::vector<std::string> ids;
    std::vector<std::string> names;
    std::string* cur = nullptr;
    if (rd_->opts_row == 0) {
        for (const auto& s : translate_services()) { ids.push_back(s.first); names.push_back(s.second); }
        cur = &settings_.translate_service;
    } else {
        if (rd_->opts_row == 2) { ids.push_back("auto"); names.push_back("Auto-detect"); }
        for (const auto& l : translate_languages()) { ids.push_back(l.first); names.push_back(l.second); }
        cur = rd_->opts_row == 1 ? &settings_.translate_to : &settings_.translate_from;
    }
    int idx = 0;
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == *cur) { idx = static_cast<int>(i); break; }
    const int n = static_cast<int>(ids.size());
    int next = idx;
    if (arrow && (key == 'C' || key == 'D')) next = (idx + (key == 'C' ? 1 : n - 1)) % n;
    else if (rd_->opts_row > 0 && std::isalpha(key) && key < 128) {   // jump to the next language that starts with the letter
        const char want = static_cast<char>(std::tolower(key));
        for (int k = 1; k <= n; ++k) {
            const int i = (idx + k) % n;
            if (!names[static_cast<size_t>(i)].empty() && std::tolower(static_cast<unsigned char>(names[static_cast<size_t>(i)][0])) == want) { next = i; break; }
        }
    } else return;
    if (next == idx) return;
    *cur = ids[static_cast<size_t>(next)];
    rd_->opts_dirty = true;
    rd_->opts_warn.clear();
    rd_->same = false;
}

void App::handle_readlyrics_key(int key) {
    if (key == 0) return;
    if (rd_->opts_open) { readlyrics_options_key(key); return; }
    if (rd_->exp_open) { readlyrics_export_key(key); return; }
    if (rd_->search_open) { readlyrics_search_key(key); return; }
    if (rd_->add_open) { readlyrics_add_key(key); return; }
    if (rd_->te_open) { readlyrics_te_key(key); return; }
    const bool arrow = last_key_was_arrow();
    if (!arrow && key_is("HKeyTranslit", key)) { toggle_latin_letters(); return; }   // Latin letters on / off, also here
    if (!arrow && key_is("HKeyLyricsTiming", key)) { lyrics_edit_open_over_reader(); return; }   // the lyrics timing overlay, as on the main screen
    if (!arrow && key_is("HKeyRetryLyrics", key) && !rd_->confirm_del) { rl_open_over_reader(); return; }   // the re-fetch / version control overlay
    if (!arrow && key_is("HKeySearch", key) && !rd_->confirm_del) { readlyrics_search_open(); return; }   // "/" (SHIFT+7 on a German keyboard): search the lyrics files
    if (!rd_->confirm_del && (key == kKeyCtrlN || (!arrow && (key == 'a' || key == 'A')))) { readlyrics_add_open(); return; }   // add a lyrics file by hand
    if (rd_->confirm_del) {   // "delete this translation?" -- only y confirms
        if (!arrow && (key == 'y' || key == 'Y')) readlyrics_delete_translation();
        else { rd_->confirm_del = false; rd_->status = "kept"; rd_->status_warn = false; }
        return;
    }
    if (key == 27 || key_is("HKeyReadLyrics", key) || key == 'q' || key == 'Q') { readlyrics_close(); return; }
    if (!arrow && key == '\t') { rd_->pane = rd_->pane == 0 ? 1 : 0; rd_->status.clear(); return; }
    if (!arrow && (key == 'p' || key == 'P')) { readlyrics_play(); return; }
    if (!arrow && (key == 'f' || key == 'F')) {
        rd_->follow = !rd_->follow;
        rd_->status = rd_->follow ? "the view follows the song" : "the view stays where it is -- [f] follows the song again";
        rd_->status_warn = false;
        return;
    }
    if (key == kKeyShiftLeft || key == kKeyShiftRight) {   // SHIFT+left / right: 5 s back / forward, to hear a line again
        if (readlyrics_track_plays()) player_.seek_relative(key == kKeyShiftLeft ? -5.0 : 5.0);
        return;
    }
    if (!arrow && key == 'C' && !rd_->confirm_del) { readlyrics_create_translation(); return; }   // SHIFT+c: an empty translation file to fill in by hand
    if (!arrow && key == 'E' && !rd_->confirm_del) { readlyrics_te_open(); return; }   // SHIFT+e: edit / add a translation by hand
    if (!arrow && key == 'e') {   // [e]: export these lyrics
        if (rd_->lines.empty()) { rd_->status = "there are no lyrics to export"; rd_->status_warn = true; return; }
        std::vector<int> fl; std::vector<std::string> nm;
        readlyrics_export_choices(fl, nm);
        if (std::find(fl.begin(), fl.end(), rd_->exp_choice) == fl.end()) rd_->exp_choice = fl.back();   // (the most there is)
        rd_->exp_open = true;
        rd_->exp_wait = false;
        translit_kanji_waiting(rd_->lines);
        rd_->exp_msg.clear();
        rd_->status.clear();
        return;
    }
    if (!arrow && key == 't') { rd_->opts_open = true; rd_->opts_row = 0; rd_->opts_warn.clear(); return; }
    if (!arrow && key == 'T') { readlyrics_toggle_translations(); return; }
    const int n = static_cast<int>(rd_->versions.size());
    // LEFT / RIGHT choose in the pane that has the keys ('D' is Left, 'C' is Right)
    if (arrow && (key == 'D' || key == 'C')) {
        const int dir = key == 'D' ? -1 : 1;
        if (rd_->pane == 0) {
            const int ns = n > 1 ? (rd_->sel + dir + n) % n : 0;   // (round the ends: with several files LEFT / RIGHT toggle through them)
            if (ns != rd_->sel) { rd_->sel = ns; rd_->status.clear(); readlyrics_load_version(); }
        } else if (!rd_->tlist.empty()) {
            const int nt = static_cast<int>(rd_->tlist.size());
            const int ns = (rd_->tsel + dir + nt) % nt;
            if (ns != rd_->tsel || !rd_->show) { readlyrics_select_translation(ns); rd_->show = true; rd_->status.clear(); }
        }
        return;
    }
    if (rd_->pane == 1 && (key == kKeyDelete || (!arrow && (key == 'd' || key == 'D')))) {
        if (rd_->tlist.empty()) { rd_->status = "there is no translation to delete"; rd_->status_warn = true; }
        else rd_->confirm_del = true;
        return;
    }
    const bool scrolls = (arrow && (key == 'A' || key == 'B')) || key == ' ' || key == 'b' || key == 'B' || key == kKeyHome || key == kKeyEnd;
    if (scrolls && rd_->follow && readlyrics_track_plays()) { rd_->follow = false; rd_->status = "the view stays where it is -- [f] follows the song again"; rd_->status_warn = false; }
    if (arrow && last_key_was_shifted() && (key == 'A' || key == 'B')) {   // SHIFT+up / down: a page
        const int page = std::max(1, term_rows_ - 14);
        rd_->scroll = key == 'A' ? std::max(0, rd_->scroll - page) : rd_->scroll + page;
        return;
    }
    if (arrow && key == 'A') { if (rd_->scroll > 0) --rd_->scroll; return; }
    if (arrow && key == 'B') { ++rd_->scroll; return; }          // clamped against the real content while drawing
    if (key == ' ') { rd_->scroll += std::max(1, term_rows_ - 14); return; }
    if (key == 'b' || key == 'B') { rd_->scroll = std::max(0, rd_->scroll - std::max(1, term_rows_ - 14)); return; }
    if (key == kKeyHome) { rd_->scroll = 0; return; }
    if (key == kKeyEnd) { rd_->scroll = 1 << 20; return; }       // clamped to the last page while drawing
}

// --- CTRL+s: search the lyrics files ------------------------------------------------------------------------------
// Lyrics live in a "lyrics" folder inside the folder of each track (and of the library folders / the download cache),
// so those are the folders that are searched: one listing per distinct folder, done when the search is opened.
void App::readlyrics_search_open() {
    std::set<std::string> dirs;
    auto add_dir = [&](const fs::path& folder) { if (!folder.empty()) dirs.insert(path_utf8(folder / "lyrics")); };
    for (const auto& t : all_local_tracks_) add_dir(t.path.parent_path());
    for (const auto& p : settings_.local_music_paths) add_dir(path_from_utf8(p));
    add_dir(rd_->track.parent_path());
    rd_->search_all.clear();
    rd_->search_dirs = 0;
    for (const auto& d : dirs) {
        std::error_code ec;
        const fs::path dir = path_from_utf8(d);
        if (!fs::is_directory(dir, ec)) continue;
        ++rd_->search_dirs;
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
            rd_->search_all.push_back(std::move(h));
            if (rd_->search_all.size() >= 200000) break;
        }
    }
    std::sort(rd_->search_all.begin(), rd_->search_all.end(), [](const LyricsFileHit& a, const LyricsFileHit& b) {
        return a.lname != b.lname ? a.lname < b.lname : a.folder < b.folder;
    });
    rd_->search_q.clear();
    rd_->search_sel = rd_->search_first = 0;
    readlyrics_search_rebuild();
    rd_->search_open = true;
    rd_->status.clear();
}

// Every word of the query has to be somewhere in the file name (any order, case-insensitive for ASCII letters).
void App::readlyrics_search_rebuild() {
    std::vector<std::string> words;
    {
        std::istringstream iss(ascii_lower_str(rd_->search_q));
        std::string w;
        while (iss >> w) words.push_back(w);
    }
    rd_->search_view.clear();
    for (size_t i = 0; i < rd_->search_all.size(); ++i) {
        bool ok = true;
        for (const auto& w : words) if (rd_->search_all[i].lname.find(w) == std::string::npos) { ok = false; break; }
        if (ok) rd_->search_view.push_back(static_cast<int>(i));
    }
    const int n = static_cast<int>(rd_->search_view.size());
    rd_->search_sel = std::clamp(rd_->search_sel, 0, std::max(0, n - 1));
    rd_->search_first = std::clamp(rd_->search_first, 0, std::max(0, n - 1));
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
    rd_->track = hit.path.parent_path().parent_path() / path_from_utf8(hit.name + ".audio");
    rd_->title = hit.name;
    rd_->opened_hit = hit;
    rd_->versions.assign(1, v);
    rd_->sel = 0;
    rd_->first = 0;
    rd_->pane = 0;
    readlyrics_load_version();
    rd_->status = "showing a lyrics file from the search -- CTRL+L closes the reader, open it again for the playing track";
    rd_->status_warn = false;
}

void App::readlyrics_search_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int n = static_cast<int>(rd_->search_view.size());
    if (key == 27) { rd_->search_open = false; return; }
    if (key == '\r' || key == '\n') {
        if (n == 0) return;
        const LyricsFileHit hit = rd_->search_all[static_cast<size_t>(rd_->search_view[static_cast<size_t>(rd_->search_sel)])];
        rd_->search_open = false;
        readlyrics_show_file(hit);
        return;
    }
    if (arrow && key == 'A') { rd_->search_sel = std::max(0, rd_->search_sel - 1); return; }
    if (arrow && key == 'B') { rd_->search_sel = std::min(std::max(0, n - 1), rd_->search_sel + 1); return; }
    if (arrow) return;
    if (key == kKeyHome) { rd_->search_sel = 0; return; }
    if (key == kKeyEnd) { rd_->search_sel = std::max(0, n - 1); return; }
    if (key == 127 || key == 8) { pop_utf8_char(rd_->search_q); rd_->search_sel = rd_->search_first = 0; readlyrics_search_rebuild(); return; }
    if (is_text_key(key) && rd_->search_q.size() < 100) {
        rd_->search_q += static_cast<char>(key);
        rd_->search_sel = rd_->search_first = 0;
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
        std::string shown = rd_->search_q;   // a long query keeps its END visible: cut from the front
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
    const int n = static_cast<int>(rd_->search_view.size());
    int first = std::clamp(rd_->search_first, 0, std::max(0, n - 1));
    const int sel = std::clamp(rd_->search_sel, 0, std::max(0, n - 1));
    if (sel < first) first = sel;
    if (sel >= first + list_rows) first = sel - list_rows + 1;
    for (int r = 0; r < list_rows; ++r) {
        const int i = first + r;
        std::string painted;
        if (i < n) {
            const LyricsFileHit& h = rd_->search_all[static_cast<size_t>(rd_->search_view[static_cast<size_t>(i)])];
            const std::string tail = h.folder.empty() ? std::string() : "  (" + h.folder + ")";
            const int tail_w = std::min(display_width(tail), inner / 3);
            std::string left = truncate_str(h.name, std::max(5, inner - tail_w));
            std::string line = pad_right(left, inner - tail_w) + pad_right(truncate_str(tail, tail_w), tail_w);
            painted = i == sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + line + R : line;
        } else if (r == 0 && n == 0) {
            painted = pad_right(rd_->search_all.empty() ? "no lyrics files found in the lyrics folders of the library" : "no lyrics file matches", inner);
        } else {
            painted = std::string(static_cast<size_t>(inner), ' ');
        }
        out.push_back(bar + " " + painted + " " + bar);
    }
    out.push_back(bar + " " + std::string(static_cast<size_t>(inner), ' ') + " " + bar);
    const std::string info = std::to_string(n) + " of " + std::to_string(rd_->search_all.size()) + " files  in " + std::to_string(rd_->search_dirs) + (rd_->search_dirs == 1 ? " folder" : " folders");
    out.push_back(bar + " " + legend_sgr(settings_) + pad_right(truncate_str(info, inner), inner) + R + " " + bar);
    out.push_back(box_bottom(PW, "[↑↓] Pick  [ENTER] Open  [ESC] Close", border_bottom));
    return out;
}

// --- [a] / CTRL+n: add a lyrics file to a track by hand ---------------------------------------------------------
std::string App::readlyrics_add_track_label(int idx) const {
    if (idx < 0) {
        if (rd_->add_cur.empty()) return rd_->opened_hit.path.empty() ? "Current track:  (none -- play or open a track first)" : "Current track:  (none -- no track is named like the opened lyrics)";
        return "Current track:  " + (rd_->add_names ? path_utf8(rd_->add_cur.filename()) : rd_->add_cur_title);
    }
    const LocalTrack& t = all_local_tracks_[static_cast<size_t>(idx)];
    if (rd_->add_names) {
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

void App::build_readlyrics_screen(std::ostringstream& frame, int W, int target_height) {
    if (W < 60) W = 60;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const int inner = W - 4;
    auto boxed = [&](const std::string& painted) { return bar + " " + painted + " " + bar; };

    // One line of choices that scrolls sideways: the lyrics files / the translations. `sel` is highlighted.
    auto chooser = [&](const std::vector<std::string>& items, int& sel, int& first, int pane_inner) {
        const int n = static_cast<int>(items.size());
        const int avail = std::max(6, pane_inner - 4);   // the two 2-cell scroll marks
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

    // The two top panes sit side by side: the lyrics files left, the translations right (each one line, scrolling sideways).
    const int wL = W / 2, wR = W - wL;
    const int inL = wL - 4, inR = wR - 4;
    auto boxed_w = [&](const std::string& painted) { return bar + " " + painted + " " + bar; };
    std::string top_l, top_r, mid_l, mid_r, bot_l, bot_r;

    // --- top pane 1: ONE line with the lyrics files of the track ---
    top_l = box_top("READ LYRICS" + (rd_->pane == 0 ? focus_indicator(settings_, border) : std::string()), wL, border);
    {
        std::string line;
        if (rd_->versions.empty()) {
            line = pad_right(truncate_str("no lyrics saved yet (fetched while the track plays)", inL), inL);
        } else {
            std::vector<std::string> items;
            for (const auto& v : rd_->versions) {
                std::string t = (v.active ? "● " : "") + v.label;
                if (!v.detail.empty()) t += " (" + v.detail + ")";
                items.push_back(t);
            }
            line = chooser(items, rd_->sel, rd_->first, inL);
        }
        mid_l = boxed_w(line);
    }
    bot_l = box_bottom(wL, n_files_label(rd_->versions.size()), border_bottom);

    // --- top pane 2: ONE line with the translations saved for the chosen file ---
    const bool show = rd_->show && !rd_->tlist.empty() && rd_->trans.size() == rd_->lines.size();
    top_r = box_top("TRANSLATION" + (rd_->pane == 1 ? focus_indicator(settings_, border) : std::string()), wR, border);
    {
        std::string line;
        if (rd_->tlist.empty()) {
            line = pad_right(truncate_str(rd_->lines.empty() ? "nothing to translate" : "no translation yet -- [t] translates, [SHIFT+c] creates one", inR), inR);
        } else {
            std::vector<std::string> items;
            for (size_t i = 0; i < rd_->tlist.size(); ++i) {
                const TranslationInfo& t = rd_->tlist[i];
                items.push_back(std::string(show && static_cast<int>(i) == rd_->tsel ? "● " : "") +
                                (translate_service_is_manual(t.service) ? translate_service_name(t.service)
                                 : language_name(t.to) + (t.service.empty() ? std::string() : " (" + translate_service_name(t.service) + ")")));
            }
            line = chooser(items, rd_->tsel, rd_->tfirst, inR);
        }
        mid_r = boxed_w(line);
    }
    {
        std::string label;
        if (rd_->busy) label = "translating into " + language_name(rd_->job_to) + " with " + translate_service_name(rd_->job_svc) + " ...";
        else if (!rd_->tlist.empty()) label = std::to_string(rd_->tlist.size()) + (rd_->tlist.size() == 1 ? " translation" : " translations") + (show ? "" : " (hidden)");
        bot_r = box_bottom(wR, label, border_bottom);
    }
    frame << top_l << top_r << "\n" << mid_l << mid_r << "\n" << bot_l << bot_r << "\n";

    // --- footer first (the lyrics pane takes the rows it leaves) ---
    // The legend is a small tree table (Move / Translate / Session), three rows whatever the width; the same in both panes
    // (TAB only moves the focus indicator): the keys that act on the focused pane say so in one entry.
    std::vector<std::string> hint_lines;
    if (rd_->confirm_del && rd_->tsel >= 0 && rd_->tsel < static_cast<int>(rd_->tlist.size())) {
        hint_lines.push_back(legend_sgr(settings_) + truncate_str(" Delete the " + language_name(rd_->tlist[static_cast<size_t>(rd_->tsel)].to) + " translation (" + translate_service_name(rd_->tlist[static_cast<size_t>(rd_->tsel)].service) + ")?  [y] Yes | [any other key] No", W) + "\x1b[0m");
        hint_lines.push_back(std::string());
        hint_lines.push_back(std::string());
    } else {
        std::vector<LegendGroup> groups = {
            {"Move", {"[\u2190\u2192] Choose file/Translation\001Choose", "[TAB] Focus", "[\u2191\u2193] Scroll", "[SHIFT+\u2191\u2193] Page up/down\001Page", "[HOME/END] Top/Bottom"}},
            {"Play", {"[p] Play/Pause", "[SHIFT+\u2190\u2192] Seek 5 s\001Seek", "[f] Follow the song\001Follow", "[" + hotkey_text("HKeyLyricsTiming", "ALT+L") + "] Lyrics timing\001Timing"}},
            {"Translate", {"[t] Translate", "[SHIFT+t] Show/Hide translations\001Show/Hide", "[d/DEL] Delete translation\001Delete", "[SHIFT+c] Create","[SHIFT+e] Edit/Add translation\001Edit/Add", "[e] Export"}},
            {"Session", {"[" + hotkey_text("HKeySearch", "/") + "] Search", "[" + hotkey_text("HKeyRetryLyrics", "l") + "] Re-fetch lyrics\001Re-fetch", "[a] Add / Assign lyrics manually\001Add / Assign", "[ESC] Close"}},
        };
        const auto lrows = layout_legend_tree(groups, W - 1);
        for (size_t i = 0; i < lrows.size(); ++i) hint_lines.push_back(" " + paint_legend(settings_, lrows, i, border, 0, true));
    }
    const int legend_rows = static_cast<int>(hint_lines.size());
    const int status_rows = 0;   // no status line here: the status message and the log overlay show rd_->status (it used to change the size of the panes)
    const int budget = std::min(target_height, term_rows_ - 1);
    const int body = std::max(3, budget - 3 /*the two top panes, side by side*/ - 2 /*lyrics pane borders*/ - legend_rows - status_rows);
    // two blank rows above and below the lyrics (none on a terminal too short for them)
    const int margin = body >= 10 ? 2 : 0;
    const int text_rows = body - 2 * margin;
    const int wrap_w = std::max(10, inner - 8);   // a little air left and right: the lines are centred

    // --- lyrics pane ---
    // The track of the reader is the loaded one: its lyrics are drawn the way the lyrics engine draws them (the active line, the words that
    // are sung, the animation of Settings), the view follows the song, and the translation of the active line is lit with it.
    struct Row { std::string text; int style; bool pre = false; };   // 0 original, 1 translation, 2 blank, 3 translation of the active line; pre: painted already (wrap_w wide)
    const bool plays = readlyrics_track_plays() && rd_->timed.size() == rd_->lines.size() && !rd_->lines.empty();
    const double elapsed = plays ? player_.poll_elapsed() : 0.0;
    bool synced = false;
    for (const LyricLine& l : rd_->timed) if (l.start_time >= 0.0) { synced = true; break; }
    double offset = rd_->delay;
    if (mode_ == Mode::LyricsEdit && lyrics_edit_from_reader_) { std::lock_guard<std::mutex> lk(lyrics_mutex_); offset = lyrics_result_.delay; }   // the timing overlay is open: live
    const double lt = elapsed - offset;
    int active = -1;
    if (plays) {
        if (synced) {
            for (size_t i = 0; i < rd_->timed.size(); ++i) if (rd_->timed[i].start_time >= 0.0 && rd_->timed[i].start_time <= lt) active = static_cast<int>(i);
        } else if (total_sec_ > 0) {   // plain lyrics: spread over the track
            active = std::clamp(static_cast<int>(elapsed / static_cast<double>(total_sec_) * static_cast<double>(rd_->lines.size())), 0, static_cast<int>(rd_->lines.size()) - 1);
        }
    }
    Settings ks = settings_;   // centred whole-line look, as in the karaoke view
    ks.lyrics_alignment = 0;
    std::vector<Row> rows;
    int active_row = -1;
    for (size_t i = 0; i < rd_->lines.size(); ++i) {
        if (rd_->lines[i].empty()) { rows.push_back({std::string(), 2}); continue; }
        const bool is_active = plays && static_cast<int>(i) == active;
        if (is_active) active_row = static_cast<int>(rows.size());
        if (plays) {
            LyricLine ll = rd_->timed[i];
            if (translit_enabled()) { ll.full_text = transliterate(ll.full_text); for (auto& w : ll.words) w.second = transliterate(w.second); }
            const bool timed = synced && ll.start_time >= 0.0;
            for (auto& w : render_lyric_line_wrapped(ll, timed ? lt : 1e12, wrap_w, is_active, ks)) rows.push_back({w, 0, true});
        } else {
            for (auto& w : wrap_lines(shown_text(rd_->lines[i]), wrap_w, 8)) rows.push_back({w, 0});
        }
        if (show) {
            const std::string& t = rd_->trans[i];
            if (!t.empty() && ascii_lower_str(t) != ascii_lower_str(rd_->lines[i]))
                for (auto& w : wrap_lines("(" + shown_text(t) + ")", wrap_w, 8)) rows.push_back({w, is_active ? 3 : 1});
        }
    }
    if (rd_->lines.empty()) {
        rows.push_back({rd_->versions.empty() ? "no lyrics saved for this track" : "this file has no readable lyrics", 2});
    }
    if (plays && rd_->follow && active_row >= 0) rd_->scroll = std::max(0, active_row - text_rows / 2 + 1);   // (clamped below)
    const int max_scroll = std::max(0, static_cast<int>(rows.size()) - text_rows);
    rd_->scroll = std::clamp(rd_->scroll, 0, max_scroll);

    std::string info;
    if (!rows.empty() && !rd_->lines.empty())
        info = "row " + std::to_string(std::min<int>(rd_->scroll + text_rows, static_cast<int>(rows.size()))) + "/" + std::to_string(rows.size());
    if (plays) info += std::string(info.empty() ? "" : "  ") + (player_.is_paused() ? "paused " : "playing ") + fmt_mmss(elapsed) + " / " + fmt_mmss(static_cast<double>(total_sec_)) +
                       (rd_->follow ? "" : "  (follow off: [f])");
    const std::string sep2 = info.empty() ? "" : "  ";
    if (show && translate_service_is_manual(rd_->svc)) info += sep2 + "translation: " + translate_service_name(rd_->svc) + " (" + language_name(rd_->to) + ")";
    else if (show) info += sep2 + "translation: " + language_name(rd_->from) + " > " + language_name(rd_->to) +
                      (rd_->svc.empty() ? std::string() : " (" + translate_service_name(rd_->svc) + ")");
    else if (rd_->same && rd_->tlist.empty()) info += sep2 + "already " + language_name(settings_.translate_to);
    else if (!rd_->tlist.empty()) info += sep2 + "translation hidden -- [SHIFT+t] shows it";

    frame << box_top(truncate_str(rd_->title, std::max(10, W - 8)), W, border) << "\n";
    const std::string orig_sgr = ansi_for(settings_.active_line_color, false);
    const std::string trans_sgr = ansi_for(settings_.inactive_line_color, false);
    for (int k = 0; k < body; ++k) {
        const int idx = rd_->scroll + k - margin;
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


// --- SHIFT+e: edit a translation by hand / write a new one from scratch -------------------------------------------------
// One translation line per lyrics line, side by side with the original. A machine translation that is edited is saved as a NEW
// "Manual Translation" (the original of the service stays as it was); a manual translation that is edited is changed in place.
// The language can be chosen (TAB). The lines that are empty in the lyrics (the gaps between stanzas) are not edited.
int App::readlyrics_te_body_rows() const {
    return std::max(3, std::max(20, term_rows_ - 1) - kTeLegendRows - 5);   // top border, header, column titles, status, bottom border
}

void App::readlyrics_te_step(int dir) {
    int i = rd_->te_row;
    const int n = static_cast<int>(rd_->te_lines.size());
    for (i += dir; i >= 0 && i < n; i += dir) if (!readlyrics_te_blank(static_cast<size_t>(i))) { rd_->te_row = i; return; }
}

// SHIFT+c: creates an EMPTY translation file for the chosen lyrics file (a "Manual Translation", into the Translate To language) and
// selects it; [SHIFT+e] writes it.
void App::readlyrics_create_translation() {
    rd_->status_warn = true;
    bool any = false;
    for (size_t i = 0; i < rd_->lines.size(); ++i) if (!readlyrics_te_blank(i)) any = true;
    if (!any) { rd_->status = "there are no lyrics to translate"; return; }
    if (rd_->sel < 0 || rd_->sel >= static_cast<int>(rd_->versions.size())) { rd_->status = "no lyrics file is chosen"; return; }
    const LyricsVersion& v = rd_->versions[static_cast<size_t>(rd_->sel)];
    LyricsTranslation t;
    t.ok = true;
    t.service = translate_next_manual_service(rd_->tlist);
    t.lines.assign(rd_->lines.size(), std::string());
    const std::string to = settings_.translate_to;
    if (!save_lyrics_translation(rd_->track, v, to, rd_->lines, t)) { rd_->status = "could not create the translation (is the lyrics folder writable?)"; return; }
    readlyrics_refresh_translations(to, t.service);
    rd_->show = !rd_->tlist.empty();
    rd_->pane = 1;
    rd_->status_warn = false;
    rd_->status = "created an empty " + translate_service_name(t.service) + " (" + language_name(to) + ") -- [SHIFT+e] writes it";
    log_event("empty translation created: " + translate_service_name(t.service) + " " + to);
    force_redraw_ = true;
}

void App::readlyrics_te_open() {
    rd_->status_warn = true;
    bool any = false;
    for (size_t i = 0; i < rd_->lines.size(); ++i) if (!readlyrics_te_blank(i)) any = true;
    if (!any) { rd_->status = "there are no lyrics to translate"; return; }
    rd_->status_warn = false;
    rd_->te_lines.assign(rd_->lines.size(), std::string());
    rd_->te_editing = false;
    rd_->te_to = settings_.translate_to;
    rd_->te_orig_to.clear();
    rd_->te_msg.clear();
    rd_->te_warn = false;
    const bool have = rd_->tsel >= 0 && rd_->tsel < static_cast<int>(rd_->tlist.size()) && rd_->trans.size() == rd_->lines.size();
    if (have) {
        const TranslationInfo& t = rd_->tlist[static_cast<size_t>(rd_->tsel)];
        rd_->te_lines = rd_->trans;
        rd_->te_to = t.to;
        if (translate_service_is_manual(t.service)) {
            rd_->te_editing = true;
            rd_->te_svc = t.service;
            rd_->te_orig_to = t.to;
            rd_->te_msg = "changing " + translate_service_name(t.service);
        } else {
            rd_->te_svc = translate_next_manual_service(rd_->tlist);
            rd_->te_msg = "a copy of the " + translate_service_name(t.service) + " text: it is saved as a new " + translate_service_name(rd_->te_svc);
        }
    } else {
        rd_->te_svc = translate_next_manual_service(rd_->tlist);
        rd_->te_msg = "a new translation from scratch: it is saved as " + translate_service_name(rd_->te_svc);
    }
    for (size_t i = 0; i < rd_->te_lines.size(); ++i) if (readlyrics_te_blank(i)) rd_->te_lines[i].clear();
    rd_->te_row = -1;
    readlyrics_te_step(+1);
    rd_->te_mark = -1;
    rd_->te_hist.clear(); rd_->te_redo.clear(); rd_->te_kind = 0;
    rd_->te_first = 0;
    rd_->te_focus = 0;
    rd_->te_dirty = rd_->te_ask = rd_->te_new_ask = false;
    edit_owner_.clear();
    rd_->te_open = true;
    force_redraw_ = true;
}

bool App::readlyrics_te_save() {
    rd_->te_warn = true;
    bool filled = false;
    for (const std::string& l : rd_->te_lines) if (l.find_first_not_of(" \t\r") != std::string::npos) filled = true;
    if (!filled) { rd_->te_msg = "nothing is written yet -- nothing to save"; return false; }
    if (rd_->sel < 0 || rd_->sel >= static_cast<int>(rd_->versions.size())) { rd_->te_msg = "no lyrics file is chosen"; return false; }
    const LyricsVersion& v = rd_->versions[static_cast<size_t>(rd_->sel)];
    LyricsTranslation t;
    t.ok = true;
    t.service = rd_->te_svc;
    t.lines = rd_->te_lines;
    for (size_t i = 0; i < t.lines.size(); ++i) if (readlyrics_te_blank(i)) t.lines[i].clear();
    if (!save_lyrics_translation(rd_->track, v, rd_->te_to, rd_->lines, t)) { rd_->te_msg = "could not save the translation (is the lyrics folder writable?)"; return false; }
    if (rd_->te_editing && !rd_->te_orig_to.empty() && rd_->te_orig_to != rd_->te_to) delete_lyrics_translation(rd_->track, v, rd_->te_orig_to, rd_->te_svc);   // the language changed: the file moves
    const std::string svc = rd_->te_svc, to = rd_->te_to;
    rd_->te_open = false;
    rd_->te_dirty = false;
    text_disengage();
    readlyrics_refresh_translations(to, svc);
    rd_->show = !rd_->tlist.empty();
    rd_->pane = 1;
    rd_->status_warn = false;
    rd_->status = "saved: " + translate_service_name(svc) + " (" + language_name(to) + ") -- in the lyrics folder (translations)";
    log_event("manual translation saved: " + translate_service_name(svc) + " " + to);
    force_redraw_ = true;
    return true;
}

void App::readlyrics_te_key(int key) {
    const bool arrow = last_key_was_arrow();
    auto close_editor = [&]() { rd_->te_open = false; rd_->te_ask = rd_->te_new_ask = false; text_disengage(); force_redraw_ = true; };
    if (rd_->te_ask) {   // "leave without saving?"
        if (!arrow && (key == 'y' || key == 'Y')) close_editor();
        else rd_->te_ask = false;
        return;
    }
    if (rd_->te_new_ask) {   // "start over with an empty translation?"
        if (!arrow && (key == 'y' || key == 'Y')) {
            std::fill(rd_->te_lines.begin(), rd_->te_lines.end(), std::string());
            rd_->te_svc = translate_next_manual_service(rd_->tlist);
            rd_->te_editing = false;
            rd_->te_orig_to.clear();
            rd_->te_dirty = false;
            rd_->te_row = -1;
            readlyrics_te_step(+1);
            rd_->te_mark = -1;
            rd_->te_hist.clear(); rd_->te_redo.clear(); rd_->te_kind = 0;
            edit_owner_.clear();
            rd_->te_warn = false;
            rd_->te_msg = "empty translation: it is saved as " + translate_service_name(rd_->te_svc);
        }
        rd_->te_new_ask = false;
        return;
    }
    if (key == 27) {
        if (rd_->te_mark >= 0) { rd_->te_mark = -1; rd_->te_msg.clear(); return; }   // first ESC: the marking goes
        if (rd_->te_focus == 1) { rd_->te_focus = 0; return; }
        if (rd_->te_dirty) { rd_->te_ask = true; return; }
        close_editor();
        return;
    }
    if (key == kKeyCtrlS) { if (readlyrics_te_save()) text_disengage(); return; }
    if (key == kKeyCtrlN) {
        bool filled = false;
        for (const std::string& l : rd_->te_lines) if (l.find_first_not_of(" \t\r") != std::string::npos) filled = true;
        if (!filled && !rd_->te_editing) { rd_->te_msg = "this translation is empty already"; rd_->te_warn = false; return; }
        rd_->te_new_ask = true;
        return;
    }
    // Latin letters on / off (the original column shows kana / Hangul / Chinese in Latin letters). CTRL+T works everywhere (T for transliteration; CTRL+2 is the same, but many terminals and keyboard layouts send nothing for it); the
    // " sign (the main key of it) only while the language is chosen, because in a line it is a character that can be typed.
    if (key == kKeyCtrlT || key == kKeyCtrl2 || (!arrow && key == '"' && rd_->te_focus == 1)) {
        toggle_latin_letters();
        rd_->te_msg = translit_enabled() ? "Latin letters: on -- the original is shown in Latin letters (your text is not changed)" : "Latin letters: off";
        rd_->te_warn = false;
        return;
    }
    if (!arrow && key == '\t') { rd_->te_focus = rd_->te_focus == 0 ? 1 : 0; return; }
    if (rd_->te_focus == 1) {   // the language: LEFT / RIGHT (or the first letter) choose, ENTER goes back to the lines
        if (key == '\r' || key == '\n') { rd_->te_focus = 0; return; }
        const auto& langs = translate_languages();
        int idx = 0;
        for (size_t i = 0; i < langs.size(); ++i) if (langs[i].first == rd_->te_to) idx = static_cast<int>(i);
        const int n = static_cast<int>(langs.size());
        int next = idx;
        if (arrow && (key == 'C' || key == 'B')) next = (idx + 1) % n;
        else if (arrow && (key == 'D' || key == 'A')) next = (idx + n - 1) % n;
        else if (!arrow && key < 128 && std::isalpha(key)) {
            const char want = static_cast<char>(std::tolower(key));
            for (int k = 1; k <= n; ++k) {
                const int i = (idx + k) % n;
                if (!langs[static_cast<size_t>(i)].second.empty() && std::tolower(static_cast<unsigned char>(langs[static_cast<size_t>(i)].second[0])) == want) { next = i; break; }
            }
        }
        if (next != idx) { rd_->te_to = langs[static_cast<size_t>(next)].first; rd_->te_dirty = true; rd_->te_msg.clear(); }
        return;
    }
    const int last = static_cast<int>(rd_->te_lines.size()) - 1;
    rd_->te_row = std::clamp(rd_->te_row, 0, last);
    if (arrow && last_key_was_page() && (key == 'A' || key == 'B')) {
        for (int i = 0; i < 10; ++i) readlyrics_te_step(key == 'A' ? -1 : +1);
        return;
    }
    // ---- whole lines: SHIFT+up/down marks, CTRL+c / x / v copy, cut and paste them, DEL clears them, CTRL+z undoes ----
    auto marked_from = [&]() { return rd_->te_mark < 0 ? rd_->te_row : std::min(rd_->te_mark, rd_->te_row); };
    auto marked_to = [&]() { return rd_->te_mark < 0 ? rd_->te_row : std::max(rd_->te_mark, rd_->te_row); };
    // The undo history: up to 10 states. A typed run in one line is one step; a deletion, a paste, a cut, a clear is one step each.
    auto push_snap = [&](const std::vector<std::string>& lines, int row) {
        rd_->te_hist.push_back({lines, row});
        if (rd_->te_hist.size() > 10) rd_->te_hist.erase(rd_->te_hist.begin());
        rd_->te_redo.clear();
    };
    auto remember = [&]() { push_snap(rd_->te_lines, rd_->te_row); rd_->te_kind = 0; };
    auto lines_text = [&](int a, int b) {   // one clipboard line per row, so the rows keep their place (gaps between stanzas stay empty lines)
        std::string t;
        for (int i = a; i <= b; ++i) { if (i > a) t += '\n'; if (!readlyrics_te_blank(static_cast<size_t>(i))) t += rd_->te_lines[static_cast<size_t>(i)]; }
        return t;
    };
    auto count_word = [](int n) { return std::to_string(n) + (n == 1 ? " line" : " lines"); };
    // text with several lines (clipboard) goes into the rows one after the other from `from` on; the rows that are empty in the original
    // take their line of the text but stay empty (so a text copied with its stanza gaps lines up)
    auto paste_lines = [&](int from, const std::string& clip) {
        std::vector<std::string> in;
        size_t pos = 0;
        while (true) {
            size_t nl = clip.find('\n', pos);
            std::string l = clip.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            while (!l.empty() && l.back() == '\r') l.pop_back();
            in.push_back(l);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        while (in.size() > 1 && in.back().empty()) in.pop_back();   // the line break at the very end of a copied text
        remember();
        int put = 0;
        for (size_t k = 0; k < in.size() && from + static_cast<int>(k) <= last; ++k) {
            const size_t i = static_cast<size_t>(from) + k;
            if (readlyrics_te_blank(i)) continue;
            std::string l = in[k];
            if (l.size() > 1000) l.resize(1000);
            rd_->te_lines[i] = l;
            ++put;
        }
        rd_->te_dirty = true;
        rd_->te_warn = false;
        rd_->te_msg = "pasted " + count_word(put) + " from line " + std::to_string(from + 1);
    };
    if (key == kKeyCtrlZ || key == 25) {   // undo (CTRL+z; CTRL+y is the same key on a QWERTZ keyboard)
        if (rd_->te_hist.empty()) { rd_->te_msg = "nothing to undo"; rd_->te_warn = false; return; }
        rd_->te_redo.push_back({rd_->te_lines, rd_->te_row});
        rd_->te_lines = rd_->te_hist.back().lines;
        rd_->te_row = rd_->te_hist.back().row;
        rd_->te_hist.pop_back();
        rd_->te_mark = -1;
        rd_->te_kind = 0;
        rd_->te_dirty = true;
        edit_owner_.clear();
        rd_->te_msg = "undone (" + std::to_string(rd_->te_hist.size()) + " more step" + (rd_->te_hist.size() == 1 ? "" : "s") + " can be undone; CTRL+r brings the change back)";
        rd_->te_warn = false;
        return;
    }
    if (key == 18 || key == kKeyCtrlShiftZ) {   // redo (CTRL+r, or CTRL+SHIFT+z in a terminal that reports it)
        if (rd_->te_redo.empty()) { rd_->te_msg = "nothing to redo"; rd_->te_warn = false; return; }
        rd_->te_hist.push_back({rd_->te_lines, rd_->te_row});
        rd_->te_lines = rd_->te_redo.back().lines;
        rd_->te_row = rd_->te_redo.back().row;
        rd_->te_redo.pop_back();
        rd_->te_mark = -1;
        rd_->te_kind = 0;
        rd_->te_dirty = true;
        edit_owner_.clear();
        rd_->te_msg = "redone";
        rd_->te_warn = false;
        return;
    }
    if (arrow && (key == 'A' || key == 'B') && last_key_was_shifted() && !last_key_was_page()) {   // SHIFT+up / down: mark lines
        if (rd_->te_mark < 0) rd_->te_mark = rd_->te_row;
        readlyrics_te_step(key == 'A' ? -1 : +1);
        const int n = marked_to() - marked_from() + 1;
        rd_->te_msg = count_word(n) + " marked (CTRL+c copy, CTRL+x cut, CTRL+v paste, DEL clear)";
        rd_->te_warn = false;
        edit_owner_.clear();
        return;
    }
    if (rd_->te_mark >= 0) {
        const int a = marked_from(), b = marked_to();
        if (key == kKeyCtrlC || key == kKeyCtrlX) {
            clipboard_set(lines_text(a, b));
            if (key == kKeyCtrlX) {
                remember();
                for (int i = a; i <= b; ++i) rd_->te_lines[static_cast<size_t>(i)].clear();
                rd_->te_dirty = true;
            }
            rd_->te_msg = std::string(key == kKeyCtrlX ? "cut " : "copied ") + count_word(b - a + 1);
            rd_->te_warn = false;
            rd_->te_mark = -1;
            return;
        }
        if (key == kKeyCtrlV) { paste_lines(a, clipboard_get()); rd_->te_mark = -1; rd_->te_row = a; edit_owner_.clear(); return; }
        if (key == kKeyDelete || key == 127) {
            remember();
            for (int i = a; i <= b; ++i) rd_->te_lines[static_cast<size_t>(i)].clear();
            rd_->te_dirty = true;
            rd_->te_msg = "cleared " + count_word(b - a + 1);
            rd_->te_warn = false;
            rd_->te_mark = -1;
            edit_owner_.clear();
            return;
        }
        if (key == kKeyCtrlO) {
            remember();
            int put = 0;
            for (int i = a; i <= b; ++i) if (!readlyrics_te_blank(static_cast<size_t>(i))) { rd_->te_lines[static_cast<size_t>(i)] = rd_->lines[static_cast<size_t>(i)]; ++put; }
            rd_->te_dirty = true;
            rd_->te_msg = "the original copied into " + count_word(put);
            rd_->te_warn = false;
            rd_->te_mark = -1;
            edit_owner_.clear();
            return;
        }
        rd_->te_mark = -1;   // anything else ends the marking and does what it normally does
        edit_owner_.clear();
    }
    if (arrow && key == 'A') { readlyrics_te_step(-1); return; }
    if (arrow && key == 'B') { readlyrics_te_step(+1); return; }
    if (!arrow && (key == '\r' || key == '\n')) { readlyrics_te_step(+1); return; }
    if (key == kKeyCtrlV) {   // text with line breaks (a whole translation copied from somewhere): it goes into the lines from here on
        const std::string clip = clipboard_get();
        // (text marked inside the line: the paste replaces just that text, so the line-by-line fill is for an unmarked caret only)
        const bool marked_here = edit_owner_ == "rd-te-" + std::to_string(rd_->te_row) && edit_caret_ != edit_anchor_;
        if (!marked_here && clip.find('\n') != std::string::npos) { paste_lines(rd_->te_row, clip); edit_owner_.clear(); return; }
    }
    std::string& cur = rd_->te_lines[static_cast<size_t>(rd_->te_row)];
    const std::string owner = "rd-te-" + std::to_string(rd_->te_row);
    edit_focus(owner, cur);
    if (key == kKeyCtrlO) {   // the original line as the translation (names, "oh oh oh" ...)
        remember();
        cur = rd_->lines[static_cast<size_t>(rd_->te_row)];
        edit_caret_ = edit_anchor_ = cur.size();
        rd_->te_dirty = true;
        rd_->te_msg = "the original line copied";
        rd_->te_warn = false;
        return;
    }
    std::string st;
    const std::vector<std::string> before_lines = rd_->te_lines;
    if (edit_text_key(cur, edit_caret_, edit_anchor_, key, 1000, &st)) {
        const bool typed = is_text_key(key) && !arrow;
        if (!(typed && rd_->te_kind == 1 && rd_->te_kind_row == rd_->te_row)) push_snap(before_lines, rd_->te_row);
        rd_->te_redo.clear();
        rd_->te_kind = typed ? 1 : 2;
        rd_->te_kind_row = rd_->te_row;
        rd_->te_dirty = true; rd_->te_msg.clear();
    } else {
        rd_->te_kind = 0;
        if (!st.empty()) { rd_->te_msg = st; rd_->te_warn = false; }
    }
}

std::vector<std::string> App::build_readlyrics_te_panel(int W) const {
    const int PW = std::max(70, W), inner = PW - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const std::string hdr = header_sgr(settings_);
    const std::string leg = legend_sgr(settings_);
    const std::string ink = ansi_for(settings_.list_color, false);
    std::vector<std::string> out;
    out.push_back(box_top("EDIT / ADD TRANSLATION", PW, border));
    auto row = [&](const std::string& painted, int used) { out.push_back(bar + " " + painted + std::string(static_cast<size_t>(std::max(0, inner - used)), ' ') + " " + bar); };
    // header: the language (TAB chooses it) and the name the translation is saved under
    {
        const std::string lang = language_name(rd_->te_to);
        const std::string saved = "Saved as: " + translate_service_name(rd_->te_svc);
        const std::string lab = "Language: ";
        const std::string lang_txt = truncate_str(lang, std::max(4, inner / 2 - display_width(lab)));
        const std::string sh = truncate_str(saved, std::max(0, inner - display_width(lab) - display_width(lang_txt) - 3));
        const std::string lpart = rd_->te_focus == 1 ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + lang_txt + R : ink + lang_txt + R;
        row(hdr + lab + R + lpart + "   " + leg + sh + R, display_width(lab) + display_width(lang_txt) + 3 + display_width(sh));
    }
    const int half = (inner - 3) / 2;                 // the original's column; the translation gets the rest
    const int rw = inner - half - 3;
    {
        const std::string a = truncate_str("ORIGINAL", half), b = truncate_str("TRANSLATION", rw);
        row(hdr + pad_right(a, half) + R + "   " + hdr + b + R, half + 3 + display_width(b));
    }
    const int body = readlyrics_te_body_rows();
    const int n = static_cast<int>(rd_->te_lines.size());
    const int cur = std::clamp(rd_->te_row, 0, std::max(0, n - 1));
    // A line that does not fit its column is broken into several rows -- in both columns at once, so a translation always stays beside
    // its original line. (One column is kept free in the translation for the caret.)
    const int tw = std::max(1, rw - 1);
    auto wrap_at = [](const std::string& t, int w) {   // byte ranges of the rows: at a space when there is one, else in the word
        std::vector<std::pair<size_t, size_t>> r;
        const std::vector<size_t> cuts = utf8_cuts(t);
        size_t start = 0;
        while (true) {
            int cw = 0;
            size_t end = t.size(), lastsp = std::string::npos;
            bool broke = false;
            for (size_t k = 0; k + 1 < cuts.size(); ++k) {
                if (cuts[k] < start) continue;
                const size_t from = cuts[k], to = cuts[k + 1];
                const int gw = display_width(t.substr(from, to - from));
                if (cw + gw > w) {
                    end = (lastsp != std::string::npos && lastsp > start) ? lastsp : (from > start ? from : to);
                    broke = true;
                    break;
                }
                cw += gw;
                if (t[from] == ' ') lastsp = to;
            }
            r.push_back({start, end});
            if (!broke || end >= t.size()) break;
            start = end;
        }
        return r;
    };
    std::vector<std::vector<std::pair<size_t, size_t>>> orow(static_cast<size_t>(n)), trow(static_cast<size_t>(n));
    std::vector<std::string> oshown(static_cast<size_t>(n));
    std::vector<int> hgt(static_cast<size_t>(n), 1);
    for (int i = 0; i < n; ++i) {
        oshown[static_cast<size_t>(i)] = shown_text(rd_->lines[static_cast<size_t>(i)]);
        orow[static_cast<size_t>(i)] = wrap_at(oshown[static_cast<size_t>(i)], half);
        trow[static_cast<size_t>(i)] = wrap_at(rd_->te_lines[static_cast<size_t>(i)], tw);
        hgt[static_cast<size_t>(i)] = static_cast<int>(std::max(orow[static_cast<size_t>(i)].size(), trow[static_cast<size_t>(i)].size()));
    }
    int first = std::clamp(rd_->te_first, 0, std::max(0, n - 1));
    if (cur < first) first = cur;
    for (;;) {   // the caret's line (all its rows) has to be inside the window
        int used = 0;
        for (int i = first; i <= cur; ++i) used += hgt[static_cast<size_t>(i)];
        if (used <= body || first >= cur) break;
        ++first;
    }
    rd_->te_first = first;
    const bool typing = rd_->te_focus == 0 && !rd_->te_ask && !rd_->te_new_ask;
    const int mlo = rd_->te_mark < 0 ? -1 : std::min(rd_->te_mark, cur), mhi = rd_->te_mark < 0 ? -1 : std::max(rd_->te_mark, cur);
    const std::string sel = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
    int shown_rows = 0;
    for (int i = first; i < n && shown_rows < body; ++i) {
        const bool here = i == cur;
        const bool marked = i >= mlo && i <= mhi && mlo >= 0;
        const std::string& orig = oshown[static_cast<size_t>(i)];
        const std::string& tr = rd_->te_lines[static_cast<size_t>(i)];
        const bool edit_here = here && typing && !marked;
        const bool mine = edit_owner_ == "rd-te-" + std::to_string(i);
        size_t caret = mine ? std::min(edit_caret_, tr.size()) : tr.size();
        size_t anchor = mine ? std::min(edit_anchor_, tr.size()) : tr.size();
        const size_t slo = std::min(caret, anchor), shi = std::max(caret, anchor);
        for (int k = 0; k < hgt[static_cast<size_t>(i)] && shown_rows < body; ++k, ++shown_rows) {
            std::string left, right;
            int lused = 0, rused = 0;
            const std::string weight = (here || marked) ? "\x1b[1m" : std::string();
            if (k < static_cast<int>(orow[static_cast<size_t>(i)].size())) {
                const auto& rg = orow[static_cast<size_t>(i)][static_cast<size_t>(k)];
                std::string t = orig.substr(rg.first, rg.second - rg.first);
                while (!t.empty() && t.back() == ' ') t.pop_back();
                lused = display_width(t);
                left = (marked ? sel : weight + ink) + t + R;
            }
            left += std::string(static_cast<size_t>(std::max(0, half - lused)), ' ');
            if (k < static_cast<int>(trow[static_cast<size_t>(i)].size())) {
                const auto& rg = trow[static_cast<size_t>(i)][static_cast<size_t>(k)];
                if (edit_here) {
                    // the caret goes where it belongs; at the end of a row that has a successor it belongs to the start of the successor
                    const bool last_row = k + 1 == static_cast<int>(trow[static_cast<size_t>(i)].size());
                    std::string o = change_sgr(settings_);
                    int cols = 0;
                    const std::vector<size_t> cuts = utf8_cuts(tr);
                    auto in_row = [&](size_t off) { return off >= rg.first && (off < rg.second || (off == rg.second && last_row)); };
                    for (size_t c = 0; c + 1 < cuts.size() || (c < cuts.size() && cuts[c] == tr.size()); ++c) {
                        const size_t from = cuts[c];
                        if (from < rg.first) continue;
                        if (from > rg.second || (from == rg.second && !last_row)) break;
                        const bool is_caret = from == caret && in_row(from);
                        const bool is_sel = from >= slo && from < shi;
                        if (from == tr.size() || from >= rg.second) {   // the caret behind the last character
                            if (is_caret) { o += "\xe2\x96\x88" + R + change_sgr(settings_); cols += 1; }
                            break;
                        }
                        const std::string g = tr.substr(from, cuts[c + 1] - from);
                        if (is_caret) { o += "\xe2\x96\x88"; cols += 1; if (!is_sel) { o += g; cols += display_width(g); } else { o += "\x1b[7m" + g + "\x1b[0m" + change_sgr(settings_); cols += display_width(g); } }
                        else if (is_sel) { o += "\x1b[7m" + g + "\x1b[0m" + change_sgr(settings_); cols += display_width(g); }
                        else { o += g; cols += display_width(g); }
                    }
                    right = o + R;
                    rused = cols;
                } else {
                    std::string t = tr.substr(rg.first, rg.second - rg.first);
                    while (!t.empty() && t.back() == ' ') t.pop_back();
                    rused = display_width(t);
                    right = (marked ? sel : weight + ink) + t + R;
                }
            } else if (edit_here && k == 0) {   // an empty line that has the caret
                right = change_sgr(settings_) + "\xe2\x96\x88" + R;
                rused = 1;
            }
            row(left + border + " " + settings_.box_vertical + " " + R + right, half + 3 + rused);
        }
    }
    for (; shown_rows < body; ++shown_rows) row("", 0);
    // status: what happened / what is asked / where the caret is
    {
        std::string s;
        std::string sgr = leg;
        if (rd_->te_ask) s = "Leave without saving?  [y] Yes | [any other key] No";
        else if (rd_->te_new_ask) s = "Start over with an empty translation? What is written here is dropped.  [y] Yes | [any other key] No";
        else if (!rd_->te_msg.empty()) { s = rd_->te_msg; if (rd_->te_warn) sgr = std::string("\x1b[31m"); }
        else {
            int filled = 0, total = 0;
            for (int i = 0; i < n; ++i) if (!readlyrics_te_blank(static_cast<size_t>(i))) { ++total; if (rd_->te_lines[static_cast<size_t>(i)].find_first_not_of(" \t\r") != std::string::npos) ++filled; }
            s = "line " + std::to_string(cur + 1) + "/" + std::to_string(n) + "   " + std::to_string(filled) + " of " + std::to_string(total) + " lines written" + (rd_->te_dirty ? "   (not saved)" : "") +
                (translit_enabled() ? "   [Latin letters on]" : "");
        }
        s = truncate_str(s, inner);
        row(sgr + s + R, display_width(s));
    }
    out.push_back(box_bottom(PW, "[CTRL+s] Save  [ESC] Close", border_bottom));
    return out;
}

std::vector<std::string> App::build_readlyrics_te_legend(int W) const {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::vector<LegendGroup> groups = {
        {"Move", {"[↑↓/ENTER] Line", "[PgUp/PgDn] Page up/down\001Page", "[TAB] Language / Lines\001Language"}},
        {"Edit", {"[CTRL+o] Copy original line\001Copy line", "[CTRL+n] Start from scratch\001Scratch", "[CTRL+c/x/v] Copy/Cut/Paste\001Clipboard"}},
        {"Lines", {"[SHIFT+↑↓] Mark lines\001Mark", "[CTRL+c/x/v] Copy/Cut/Paste lines\001Clipboard", "[DEL] Clear", "[CTRL+z/y] Undo", "[CTRL+r] Redo"}},
        {"Session", {"[CTRL+t] Latin letters\001Latin", "[CTRL+s] Save", "[ESC] Close"}},
    };
    std::vector<std::string> out;
    const auto lrows = layout_legend_tree(groups, std::max(20, W - 1));
    for (size_t i = 0; i < lrows.size(); ++i) out.push_back(" " + paint_legend(settings_, lrows, i, border, 0, true));
    return out;
}


bool App::readlyrics_te_blank(size_t i) const { return i >= rd_->lines.size() || rd_->lines[i].find_first_not_of(" \t\r") == std::string::npos; }

} // namespace muisc
