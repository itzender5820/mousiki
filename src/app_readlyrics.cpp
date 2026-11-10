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
    if (has_track_ && !current_.path.empty()) {
        track = current_.path;
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
    return has_track_ && !current_.path.empty() && current_.path == rd_->track;
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
    if (rd_->watch_epoch >= 0 && mode_ == Mode::ReadLyrics && lyrics_.epoch.load() != rd_->watch_epoch) {
        bool done = false;
        {
            std::lock_guard<std::mutex> lk(lyrics_.mutex);
            done = lyrics_.ready && lyrics_.path == rd_->track;
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

} // namespace muisc
