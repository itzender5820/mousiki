// app_metasession.cpp
// Part of class App (src/app.h): The meta editor session: filters, the edit list, fetching tags, saving.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "video_links.h"
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
// Settings panel
// ---------------------------------------------------------------------

// The overlays (Settings, Console, Cheatsheet, BulkAdd) size themselves from term_rows_, the terminal's
// current row count, so they fit the real window (see term_rows_ in app.h).

// SGR prefix for the section titles drawn by the Settings panel itself:
// bold plus settings_.header_color -- the Colors tab's HEADER row, a
// palette index of the 256 by default (10, which is exactly the color
// the REFERENCE tab's category titles have always been drawn in, and
// what the PATHS tab's LOCAL PATH / DOWNLOAD PATH / PLAYLIST PATH titles borrow). An
// explicit 0/empty means "no color" here like it does everywhere else,
// which degrades these to plain bold rather than to a stray escape.
std::string header_sgr(const Settings& s) {
    std::string params = sgr_params_for(s.header_color);
    return params.empty() ? "\x1b[1m" : "\x1b[1;" + params + "m";
}

// A one-line key legend ("[ESC] Close | [ENTER] OK"): the keys in [square brackets] in the LEGEND colour (KEY-CMD), everything else
// (descriptions, separators) in the COMMAND colour. Each piece is closed with a reset, so the caller needs no trailing one.
std::string legend_colored(const Settings& s, const std::string& text) {
    const std::string R = "\x1b[0m", key = legend_sgr(s), cmd = ansi_for(s.legend_cmd_color, false);
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '[') {
            const size_t e = text.find(']', i);
            const size_t stop = e == std::string::npos ? text.size() : e + 1;
            out += key + text.substr(i, stop - i) + R;
            i = stop;
        } else {
            size_t e = text.find('[', i);
            if (e == std::string::npos) e = text.size();
            out += cmd + text.substr(i, e - i) + R;
            i = e;
        }
    }
    return out;
}

// ---------------------------------------------------------------------
// Meta/tag editor overlay (Mode::MetaEdit, HKeyMetaEditor = Shift+M)
//
// A second full-screen overlay modelled on the playlist editor further up --
// same tab strip, same boxed side-by-side panels, same hint/status footer,
// same Y/N prompts in place of the hint line -- but instead of building a
// playlist it edits the FILES themselves: their names, and their
// artist/title/album/year tags.
//
// Three rules shape the whole thing:
//
//  1. Edits are session state, never a side effect. Everything typed or
//     fetched goes into meta_session_ and is written to an autosave backup
//     (meta_editor.h, under ~/.cache/mousiki/meta_session/) after every
//     single change. The audio files stay untouched until Ctrl+Shift+S
//     explicitly applies the session.
//  2. Leaving is always safe. ESC, quitting, crashing between two edits --
//     none of that loses work: the backup is reloaded next run by
//     meta_ensure_session_loaded(), and it is only ever deleted when the
//     session is applied or explicitly discarded with Ctrl+Shift+X.
//  3. What changed stays visible. A field that was touched -- typed OR
//     filled in by AcoustID -- is drawn in header_sgr(), the same bold
//     header colour the Settings section titles use, so "what will
//     Ctrl+Shift+S write?" is answerable at a glance.
// ---------------------------------------------------------------------

// The AcoustID disclaimer shown by the fetch confirmation (in the status line in Browse, in the footer of the menu).
// It says what this particular fetch will do with tags that already have a value.
std::string App::meta_fetch_disclaimer() const {
    const std::string head = "Fetching meta data via AcoustID and MusicBrainz; not always accurate. ";
    if (meta_prompt_opts_ & 1u) return head + "Only empty tags are filled in; existing tags stay as they are. Continue?";
    return head + "Existing " + (meta_prompt_opts_ & 2u ? std::string("tags") : meta_fetch_opt_summary(meta_prompt_opts_) + " tags")
         + " are overwritten. Continue?";
}

void App::meta_open() {
    meta_status_.clear();          // before the loader below, which may set it
    meta_prompt_ = MetaPrompt::None;
    meta_prompt_paths_.clear();
    meta_ensure_session_loaded();  // reopening never loses what's already pending
    mode_ = Mode::MetaEdit;
    text_disengage();
    meta_tab_ = 0;
    meta_focus_ = 0;
    meta_query_.clear();
    meta_filter_ = 0; // a fresh open shows the whole library again
    meta_lib_selected_ = 0;
    meta_field_ = 0;
    meta_field_editing_ = false;
    meta_fetch_selected_ = 0;
    meta_refresh_lib_view();
    // The song that plays now is where the list starts (the view is unfiltered here, whatever was typed or filtered before).
    if (has_track_ && current_is_local_) {
        for (size_t i = 0; i < meta_lib_view_.size(); ++i)
            if (meta_lib_view_[i].path == current_path_) { meta_lib_selected_ = static_cast<int>(i); break; }
    }
}

// Loads the autosaved backup exactly once per run. Called from meta_open()
// AND from every session-touching action, because Shift+B reaches the same
// session from Browse mode without ever opening the menu.
void App::meta_ensure_session_loaded() {
    if (meta_session_loaded_) return;
    meta_session_loaded_ = true;
    std::vector<MetaEditEntry> entries;
    std::vector<std::string> fetch_list;
    unsigned fetch_opts = meta_fetch_opts_;
    if (load_meta_session(entries, fetch_list, fetch_opts)) {
        meta_session_ = std::move(entries);
        meta_fetch_list_ = std::move(fetch_list);
        meta_fetch_opts_ = fetch_opts;
        meta_status_ = "restored autosaved session: " + std::to_string(meta_session_.size())
                     + " file" + (meta_session_.size() == 1 ? "" : "s") + " with pending edits";
        log_event("restored meta edit session (" + std::to_string(meta_session_.size()) + " files)");
    }
    meta_refresh_lib_view();
}

void App::meta_refresh_lib_view() {
    meta_folder_hover_.clear();   // (the folder view's header cursor: the rows are rebuilt)
    // With the 'r' resort on, this rebuild re-orders the pane underneath the
    // cursor, so remember the hovered row by PATH first: clamping the old
    // INDEX would silently point the picker -- and with it the fields panel,
    // which is what the user is editing -- at a different track.
    std::string keep;
    if ((meta_resort_edited_ || meta_sort_added_) && !meta_lib_view_.empty()) keep = meta_hovering_path();

    meta_lib_view_ = filter_local_or_folders(meta_query_, meta_media_);   // (SHIFT+v: audio / video + audio / video; the search, folder view and the missing-tag filters below still apply)

    // Missing-tag filter ('x' / Shift+T/A/Y). meta_row_meta() is a native,
    // in-process tag read cached in row_meta_cache_, so the first pass costs
    // one header parse per file and everything after that is a lookup --
    // no ffprobe, no subprocess. A pending session edit counts as the value
    // the file WOULD have, so filtering right after typing (or deleting) a
    // title reflects what is on screen instead of what is on disk.
    if (meta_filter_ != 0) {
        auto value_of = [this](const LocalTrack& t, int field) -> std::string {
            const std::string p = path_utf8(t.path);
            const MetaEditEntry* e = meta_entry(p);
            if (e && e->edited[field] && !e->value[field].empty()) return e->value[field];
            RowMeta rm = meta_row_meta(t.path);
            switch (field) {
                case 1: return rm.artist;
                case 2: return rm.title;
                case 3: return rm.album;
                case 4: return rm.year;
                default: return t.title;
            }
        };
        auto matches = [&](const LocalTrack& t) {
            if (meta_filter_ == 1) { // "no meta data" = every tag field empty
                for (int f = 1; f < kMetaFieldCount; ++f)
                    if (!value_of(t, f).empty()) return false;
                return true;
            }
            const int field = (meta_filter_ == 3) ? 1   // ARTIST
                            : (meta_filter_ == 2) ? 2   // TITLE
                            : (meta_filter_ == 5) ? 3   // ALBUM
                            : 4;                        // YEAR
            return value_of(t, field).empty();
        };
        meta_lib_view_.erase(std::remove_if(meta_lib_view_.begin(), meta_lib_view_.end(),
                                            [&](const LocalTrack& t) { return !matches(t); }),
                             meta_lib_view_.end());
    }

    if (meta_sort_added_) {   // 'n': newest arrived first; 'r' (below) still lifts the edited files above this order
        std::vector<std::pair<long long, size_t>> keyed;
        keyed.reserve(meta_lib_view_.size());
        for (size_t i = 0; i < meta_lib_view_.size(); ++i) keyed.emplace_back(added_time_of(meta_lib_view_[i].path), i);
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<LocalTrack> sorted;
        sorted.reserve(keyed.size());
        for (const auto& k : keyed) sorted.push_back(std::move(meta_lib_view_[k.second]));
        meta_lib_view_ = std::move(sorted);
    }

    if (meta_resort_edited_) {
        // Strict "edited before not edited" on a stable sort is a stable
        // partition: every file with a pending edit moves into one block at
        // the top, and both blocks keep the exact order the pane already had
        // -- the normal alphabetical/scan order, or the fuzzy ranking while
        // a search is active (so a query's relevance order is never thrown
        // away). Toggling 'r' off simply rebuilds without this step, i.e.
        // straight back to that order.
        std::stable_sort(meta_lib_view_.begin(), meta_lib_view_.end(),
                         [this](const LocalTrack& a, const LocalTrack& b) {
                             auto edited = [this](const LocalTrack& t) {
                                 const MetaEditEntry* e = meta_entry(path_utf8(t.path));
                                 return e && e->any_edited();
                             };
                             return edited(a) && !edited(b);
                         });
    }
    if (!keep.empty()) {
        for (size_t i = 0; i < meta_lib_view_.size(); ++i) {
            if (path_utf8(meta_lib_view_[i].path) == keep) {
                meta_lib_selected_ = static_cast<int>(i);
                break;
            }
        }
    }

    meta_lib_selected_ = std::clamp(meta_lib_selected_, 0,
        std::max(0, static_cast<int>(meta_lib_view_.size()) - 1));
    meta_fetch_selected_ = std::clamp(meta_fetch_selected_, 0,
        std::max(0, static_cast<int>(meta_fetch_list_.size()) - 1));
}

// 'r': flip the library pane between its normal order and "every file with
// pending edits on top". Nothing else about the list changes -- same rows,
// same filter, same selection (meta_refresh_lib_view() follows it by path).
void App::meta_toggle_resort() {
    meta_resort_edited_ = !meta_resort_edited_;
    meta_refresh_lib_view();
    meta_lib_selected_ = 0;   // the view starts at the top again
    if (meta_resort_edited_) {
        int n = 0;
        for (const auto& e : meta_session_) if (e.any_edited()) ++n;
        meta_status_ = "library resorted: " + std::to_string(n) + " edited file"
                     + (n == 1 ? "" : "s") + " on top" + meta_filter_note();
    } else {
        meta_status_ = "library order: alphabetical again" + meta_filter_note();
    }
}

// The reminder added to the status line of 'r' / 'n': a missing-tag filter (x / T / A / L / Y) that is still on hides every file
// that is not missing that tag, which looks like "the resort found nothing".
std::string App::meta_filter_note() const {
    if (meta_filter_ == 0) return "";
    return std::string("  NOTE: [") + meta_filter_label() + "] filter active, hit [ESC] to clear filter";
}

// 'n': flip the library pane between its normal order and "the files that arrived last first" (see added_time_of()).
void App::meta_toggle_sort_added() {
    meta_sort_added_ = !meta_sort_added_;
    if (meta_sort_added_) { std::lock_guard<std::mutex> lk(added_time_mutex_); added_time_cache_.clear(); }   // fresh times
    meta_refresh_lib_view();
    meta_status_ = std::string(meta_sort_added_ ? "library order: recently added first" : "library order: alphabetical again") + meta_filter_note();
}

const char* App::meta_filter_label() const {
    switch (meta_filter_) {
        case 1: return "NO META DATA";
        case 2: return "MISSING TITLE";
        case 3: return "MISSING ARTIST";
        case 4: return "MISSING YEAR";
        case 5: return "MISSING ALBUM";
        default: return "";
    }
}

// SHIFT+c: the song that plays now is hovered in the list, whatever was typed or filtered (the search text and the missing-tag filter are removed).
void App::meta_jump_to_current() {
    if (!has_track_ || !current_is_local_) { meta_status_ = "no local song plays right now"; return; }
    meta_query_.clear();
    meta_filter_ = 0;
    meta_folder_hover_.clear();
    meta_refresh_lib_view();
    for (size_t i = 0; i < meta_lib_view_.size(); ++i)
        if (meta_lib_view_[i].path == current_path_) { meta_lib_selected_ = static_cast<int>(i); meta_status_ = "the song that plays now (search and filters removed)"; return; }
    meta_status_ = "the song that plays now is not in the library list";
}

// 'x' / Shift+T / Shift+A / Shift+Y: narrow the library pane to files a
// given tag is (or, for 'x', every tag is) missing. The same key clears it,
// which is what keeps four keys enough for five states, and the status line
// always says which filter is up and how much of the library survived it --
// the pane's title carries the label too, so an active filter is never
// something you have to remember.
void App::meta_toggle_filter(int filter) {
    meta_filter_ = (meta_filter_ == filter) ? 0 : filter;
    meta_refresh_lib_view();
    meta_lib_selected_ = 0;   // the view starts at the top again
    meta_folder_hover_.clear();
    const std::string shown = std::to_string(meta_lib_view_.size());
    if (meta_filter_ == 0) meta_status_ = "filter cleared - " + shown + " files";
    else meta_status_ = std::string("filter: ") + meta_filter_label() + " - " + shown + " files (press the key again or [ESC] to clear)";
}

// The autosave: save whenever there is something to restore, remove the file
// when there isn't. Deliberately NOT tied to any "was it applied" flag --
// leaving the app always ends with whatever the user last edited still on
// disk as a backup, which is the whole point of the session file.
void App::meta_persist() {
    if (!meta_session_loaded_) return;
    if (meta_session_.empty() && meta_fetch_list_.empty()) delete_meta_session();
    else save_meta_session(meta_session_, meta_fetch_list_, meta_fetch_opts_);
}

const MetaEditEntry* App::meta_entry(const std::string& path) const {
    for (const auto& e : meta_session_) if (e.path == path) return &e;
    return nullptr;
}

MetaEditEntry& App::meta_touch_entry(const std::string& path) {
    for (auto& e : meta_session_) if (e.path == path) return e;
    MetaEditEntry e;
    e.path = path;
    meta_session_.push_back(std::move(e));
    return meta_session_.back();
}

void App::meta_set_field(const std::string& path, int field, const std::string& value) {
    meta_ensure_session_loaded();
    MetaEditEntry& e = meta_touch_entry(path);
    e.edited[field] = true;
    e.value[field] = value;
    e.fetched[field] = false;           // typed over: it is the user's own value now, not a fetched suggestion
    e.choices[field].clear();
    e.choice_aux[field].clear();
    meta_persist(); // autosave the backup after every keystroke -- it's a ~2KB file
}

// Current tags of one file for the editor. Uses the same cache the main list
// uses, and the same in-process header parser as a render-thread-safe first
// pass (no subprocess on the frame loop -- see ensure_visible_row_meta()'s
// comment for why a stray ffprobe must never run here). Files
// that parser declines are finished off by the background sweep's ffprobe
// pass, which now also reads the date tag, so YEAR arrives there too.
RowMeta App::meta_row_meta(const fs::path& path) {
    std::string key = path_utf8(path);
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        auto it = row_meta_cache_.find(key);
        if (it != row_meta_cache_.end() && it->second.tags_resolved) return it->second;
    }
    RowMeta rm = try_native_row_meta(path);
    bool usable = (rm.duration_sec > 0 || rm.tags_resolved);
    bool promote = usable;
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        auto it = row_meta_cache_.find(key);
        if (it != row_meta_cache_.end()) {
            if (it->second.tags_resolved) { promote = false; rm = it->second; } // a better answer is already cached
            else if (!usable) { rm = it->second; }
        }
        if (promote) { row_meta_cache_[key] = rm; translit_learn(rm.title, rm.artist, rm.album, key); }
    }
    if (promote && rm.tags_resolved) row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);
    return rm;
}

// SHIFT+p in the library pane / fetch list: start the hovered track without
// leaving the editor, to check by ear which file is which. Uses the same
// loader a Browse-list ENTER does; the editor stays open and nothing pending
// is touched.
void App::meta_play_hovering() {
    const std::string p = meta_hovering_path();
    if (p.empty()) { meta_status_ = "no track selected"; return; }
    LocalTrack t;
    bool found = false;
    for (const auto& lt : all_local_tracks_) {
        if (path_utf8(lt.path) == p) { t = lt; found = true; break; }
    }
    if (!found) { // a fetch-list entry that is no longer in the scanned library
        t.path = path_from_utf8(p);
        t.title = path_utf8(t.path.stem());
        t.folder_artist = path_utf8(t.path.parent_path().filename());
    }
    start_local_track(t);
    meta_status_ = "playing: " + t.title;
}

std::string App::meta_hovering_path() const {
    if (meta_tab_ == 1) { // the fetch list's rows ARE the titles
        if (meta_fetch_list_.empty()) return {};
        int i = std::clamp(meta_fetch_selected_, 0, static_cast<int>(meta_fetch_list_.size()) - 1);
        return meta_fetch_list_[i];
    }
    if (meta_lib_view_.empty()) return {};
    int i = std::clamp(meta_lib_selected_, 0, static_cast<int>(meta_lib_view_.size()) - 1);
    return path_utf8(meta_lib_view_[i].path);
}

// What the field editor shows for one field: the pending value if the field
// has been edited (that's what the highlight keys off), otherwise whatever
// the file already carries.
std::string App::meta_display_value(const std::string& path, int field, const MetaEditEntry* e) {
    if (e && e->edited[field]) return e->value[field];
    if (field == static_cast<int>(MetaField::FileName)) {
        // The STEM, not the whole name: typing is meant to feel like editing
        // "Alpha Song" rather than "Alpha Song.mp3", and apply_meta_entry()
        // puts the original extension back on (meta_editor.cpp's
        // normalize_file_name()), so a rename can never change the format.
        return path_utf8(path_from_utf8(path).stem());
    }
    RowMeta rm = meta_row_meta(path_from_utf8(path));
    switch (static_cast<MetaField>(field)) {
        case MetaField::Artist: return rm.artist;
        case MetaField::Title:  return rm.title;
        case MetaField::Album:  return rm.album;
        case MetaField::Year:   return rm.year;
        default: return {};
    }
}

// Precomputes the five displayed values for whatever row is hovered, once
// per frame, because resolving them needs the mutable tag cache while
// build_meta_screen() itself is const. Called from render_frame().
void App::meta_refresh_hover_values() {
    meta_hover_values_.fill(std::string());
    meta_hover_resolved_ = false;
    if (mode_ != Mode::MetaEdit) return;
    std::string path = meta_hovering_path();
    if (path.empty()) return;
    const MetaEditEntry* e = meta_entry(path);
    meta_hover_resolved_ = meta_row_meta(path_from_utf8(path)).tags_resolved;
    for (int i = 0; i < kMetaFieldCount; ++i) meta_hover_values_[i] = meta_display_value(path, i, e);
}

// SHIFT+r in the main UI and in the meta data editor.
void App::rescan_now() {
    const size_t before = all_local_tracks_.size();
    rescan_library();
    if (mode_ == Mode::MetaEdit) meta_refresh_lib_view();
    const long d = static_cast<long>(all_local_tracks_.size()) - static_cast<long>(before);
    std::string msg = "library rescanned: " + std::to_string(all_local_tracks_.size()) + " tracks";
    if (d > 0) msg += " (+" + std::to_string(d) + ")"; else if (d < 0) msg += " (" + std::to_string(d) + ")";
    if (mode_ == Mode::MetaEdit) meta_status_ = msg; else status_line_ = msg;
}

// --- the choice overlay -----------------------------------------------------------------------------------------
void App::meta_choice_open() {
    const std::string path = meta_hovering_path();
    const MetaEditEntry* e = meta_entry(path);
    if (!e || meta_field_ < 0 || meta_field_ >= kMetaFieldCount || !e->needs_choice(meta_field_)) return;
    meta_choice_open_ = true;
    meta_choice_field_ = meta_field_;
    meta_choice_sel_ = 0;
    for (size_t k = 0; k < e->choices[meta_field_].size(); ++k) {
        if (e->choices[meta_field_][k] == e->value[meta_field_]) { meta_choice_sel_ = static_cast<int>(k); break; }
    }
}

void App::meta_choice_pick() {
    const std::string path = meta_hovering_path();
    const MetaEditEntry* ce = meta_entry(path);
    const int f = meta_choice_field_;
    if (!ce || f < 1 || f >= kMetaFieldCount || meta_choice_sel_ < 0 ||
        meta_choice_sel_ >= static_cast<int>(ce->choices[f].size())) { meta_choice_open_ = false; return; }
    MetaEditEntry& e = meta_touch_entry(path);
    const size_t k = static_cast<size_t>(meta_choice_sel_);
    e.value[f] = e.choices[f][k];
    e.edited[f] = true;
    // An album carries its year: while the year is still a fetched suggestion, it follows the album that was picked.
    const int ya = static_cast<int>(MetaField::Year);
    if (f == static_cast<int>(MetaField::Album) && k < e.choice_aux[f].size() && !e.choice_aux[f][k].empty() &&
        e.fetched[ya] && e.edited[ya]) {
        e.value[ya] = e.choice_aux[f][k];
        e.choices[ya].clear();
        e.choice_aux[ya].clear();
        e.fetched[ya] = false;
    }
    e.choices[f].clear();
    e.choice_aux[f].clear();
    e.fetched[f] = false;             // decided: from now on it is drawn like any confirmed edit
    meta_choice_open_ = false;
    meta_persist();
    meta_refresh_lib_view();
    meta_status_ = std::string(meta_field_label(f)) + ": " + e.value[f];
}

void App::meta_choice_discard() {
    const std::string path = meta_hovering_path();
    const int f = meta_choice_field_;
    meta_choice_open_ = false;
    if (f < 1 || f >= kMetaFieldCount) return;
    for (size_t i = 0; i < meta_session_.size(); ++i) {
        if (meta_session_[i].path != path) continue;
        meta_session_[i].clear_field(f);      // back to what the file has
        if (!meta_session_[i].any_edited()) meta_session_.erase(meta_session_.begin() + static_cast<long>(i));
        break;
    }
    meta_persist();
    meta_refresh_lib_view();
    meta_status_ = std::string("discarded the fetched ") + meta_field_label(f) + " and its options";
}

bool App::handle_meta_choice_key(int key) {
    if (!meta_choice_open_) return false;
    const MetaEditEntry* e = meta_entry(meta_hovering_path());
    const int n = (e && meta_choice_field_ >= 0 && meta_choice_field_ < kMetaFieldCount)
                      ? static_cast<int>(e->choices[meta_choice_field_].size()) : 0;
    if (n == 0) { meta_choice_open_ = false; return true; }
    const bool arrow = last_key_was_arrow();
    if (key == 27) { meta_choice_open_ = false; return true; }
    if (arrow && key == 'A') { if (meta_choice_sel_ > 0) --meta_choice_sel_; return true; }
    if (arrow && key == 'B') { if (meta_choice_sel_ + 1 < n) ++meta_choice_sel_; return true; }
    if (key == '\r' || key == '\n') { meta_choice_pick(); return true; }
    if (key == kKeyDelete || key == 127 || key == 8 || (key == 'd' && !arrow)) { meta_choice_discard(); return true; }
    return true;   // swallow everything else while the overlay is up
}

std::vector<std::string> App::build_meta_choice_panel() const {
    const int W = kMetaChoicePanelWidth;
    const int inner = W - 2;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    const MetaEditEntry* e = meta_entry(meta_hovering_path());
    std::vector<std::string> lines;
    const int f = std::clamp(meta_choice_field_, 0, kMetaFieldCount - 1);
    const int n = e ? static_cast<int>(e->choices[f].size()) : 0;
    lines.push_back(box_top(std::string("CHOOSE ") + meta_field_label(f) + " (" + std::to_string(n) + " found)", W, border));
    auto text_row = [&](const std::string& sgr, const std::string& text) {
        return obar + sgr + pad_right(truncate_str(" " + text, inner), inner) + "\x1b[0m" + obar;
    };
    lines.push_back(text_row(legend_sgr(settings_), "The first one is in the field now -- pick the right one:"));
    const int shown = std::max(1, std::min(n, 8));
    const int first = std::clamp(meta_choice_sel_ - shown / 2, 0, std::max(0, n - shown));
    for (int r = 0; r < shown; ++r) {
        const int k = first + r;
        if (k >= n || !e) { lines.push_back(obar + std::string(inner, ' ') + obar); continue; }
        std::string value = e->choices[f][static_cast<size_t>(k)];
        std::string aux = (static_cast<size_t>(k) < e->choice_aux[f].size()) ? e->choice_aux[f][static_cast<size_t>(k)] : std::string();
        const bool current = (e->value[f] == value);
        const std::string mark = current ? " (o) " : " ( ) ";
        std::string row;
        if (aux.empty()) {
            row = pad_right(truncate_str(mark + value, inner - 1), inner - 1) + " ";
        } else {
            const int aux_w = static_cast<int>(aux.size()) + 2;
            row = pad_right(truncate_str(mark + value, inner - 1 - aux_w), inner - 1 - aux_w) + aux + "  " + " ";
        }
        if (k == meta_choice_sel_) row = "\x1b[7m" + row + "\x1b[0m";
        lines.push_back(obar + row + obar);
    }
    if (n > shown) lines.push_back(text_row(legend_sgr(settings_), "( " + std::to_string(n - shown) + " more -- keep going with the arrow keys )"));
    lines.push_back(box_bottom(W, "[\u2191\u2193] select  [ENTER] choose  [DEL/d] discard all  [ESC] close", border));
    return lines;
}

// A video that is paired with a track (VIDEO OPTION) takes that track's tags instead of being fingerprinted: they describe the
// same song, and a video's own sound track often fingerprints worse. Only fields the video has no value for are filled.
bool App::meta_take_linked_tags(const std::string& video) {
    if (!LocalSource::is_video_file(path_from_utf8(video))) return false;
    for (const std::string& tr : VideoLinks::get().tracks_for(video)) {
        const fs::path tp = path_from_utf8(tr);
        std::error_code ec;
        if (!fs::exists(tp, ec)) continue;
        const RowMeta src = meta_row_meta(tp);
        if (src.artist.empty() && src.title.empty() && src.album.empty() && src.year.empty()) continue;
        const RowMeta own = meta_row_meta(path_from_utf8(video));
        const MetaEditEntry* cur = meta_entry(video);
        auto empty_now = [&](int f, const std::string& disk) {
            if (cur && cur->edited[f]) return cur->value[f].empty();
            return disk.empty();
        };
        const std::string vals[5] = {"", src.artist, src.title, src.album, src.year};
        const std::string disk[5] = {"", own.artist, own.title, own.album, own.year};
        bool any = false;
        for (int f = 1; f <= 4; ++f) {
            if (vals[f].empty() || !empty_now(f, disk[f])) continue;
            MetaEditEntry& e = meta_touch_entry(video);
            e.edited[f] = true;
            e.value[f] = vals[f];
            any = true;
        }
        if (any) { meta_persist(); return true; }
    }
    return false;
}

void App::meta_add_hovering_to_fetch() {
    meta_ensure_session_loaded();
    std::string path = meta_hovering_path();
    if (path.empty()) { meta_status_ = "no title selected"; return; }
    if (meta_take_linked_tags(path)) { meta_status_ = "video: took the tags of the track it is paired with (queue it again to fetch its own)"; return; }
    for (const auto& p : meta_fetch_list_) {
        if (p == path) {
            meta_status_ = "already on the fetch list (" + std::to_string(meta_fetch_list_.size()) + " queued)";
            return;
        }
    }
    meta_fetch_list_.push_back(path);
    meta_fetch_selected_ = static_cast<int>(meta_fetch_list_.size()) - 1;
    meta_persist();
    meta_status_ = "added to fetch list (" + std::to_string(meta_fetch_list_.size()) + " queued)";
}

// SHIFT+f in the library pane: puts every title that pane shows right now -- so the search AND the missing-tag filter
// (x, SHIFT+t/a/l/y) are applied -- on the fetch list. Titles already on it are skipped, the order of the pane is kept.
void App::meta_add_view_to_fetch() {
    meta_ensure_session_loaded();
    if (meta_lib_view_.empty()) { meta_status_ = "nothing to add -- the library pane is empty"; return; }
    std::unordered_set<std::string> queued(meta_fetch_list_.begin(), meta_fetch_list_.end());
    size_t added = 0;
    for (const auto& t : meta_lib_view_) {
        std::string path = path_utf8(t.path);
        if (queued.insert(path).second) { meta_fetch_list_.push_back(std::move(path)); ++added; }
    }
    if (added == 0) {
        meta_status_ = "all " + std::to_string(meta_lib_view_.size()) + " titles are on the fetch list already (" + std::to_string(meta_fetch_list_.size()) + " queued)";
        return;
    }
    meta_persist();
    meta_status_ = "added " + std::to_string(added) + (added == 1 ? " title" : " titles") + " to the fetch list"
                 + (added < meta_lib_view_.size() ? " (" + std::to_string(meta_lib_view_.size() - added) + " already on it)" : std::string())
                 + " -- " + std::to_string(meta_fetch_list_.size()) + " queued";
}

// DEL / 'd' -- same "unqueue" gesture as the main list's removal key.
void App::meta_remove_hovering() {
    meta_ensure_session_loaded();
    std::string path = meta_hovering_path();
    if (path.empty()) return;
    for (size_t i = 0; i < meta_fetch_list_.size(); ++i) {
        if (meta_fetch_list_[i] == path) {
            meta_fetch_list_.erase(meta_fetch_list_.begin() + static_cast<long>(i));
            meta_refresh_lib_view();
            meta_persist();
            meta_status_ = "removed from fetch list (" + std::to_string(meta_fetch_list_.size()) + " queued)";
            return;
        }
    }
    meta_status_ = "not on the fetch list";
}

void App::meta_prompt_single_fetch(const std::string& path, bool all) {
    if (path.empty()) return;
    meta_ensure_session_loaded();
    meta_prompt_ = MetaPrompt::Fetch;
    meta_prompt_opts_ = all ? 2u : 1u;   // SHIFT+b: only the missing tags; SHIFT+n: all tags (the boxes do not apply)
    meta_prompt_paths_.assign(1, path);
    // In the menu the footer renders the disclaimer; in Browse there is no
    // footer, so it goes through the status line instead.
    if (mode_ == Mode::MetaEdit) meta_status_.clear();
    else status_line_ = meta_fetch_disclaimer();
}

void App::meta_prompt_list_fetch() {
    meta_ensure_session_loaded();
    if (meta_fetch_list_.empty()) {
        meta_status_ = "fetch list is empty -- press 'a' on a track to queue it";
        return;
    }
    meta_prompt_ = MetaPrompt::Fetch;
    meta_prompt_opts_ = meta_fetch_opts_;
    meta_prompt_paths_ = meta_fetch_list_;
}

// The tag fields (1..4) a fetch with the ticked FETCH options `opts` may fill in for `path`. "missing only" asks for
// exactly the tags that are empty right now -- a pending edit counts as the value the file will have -- while "all" and
// the single tags ask regardless of what is there (those are the only ways an existing tag gets overwritten).
std::array<bool, kMetaFieldCount> App::meta_want_for(const std::string& path, unsigned opts) {
    std::array<bool, kMetaFieldCount> want{};
    const MetaEditEntry* e = meta_entry(path);
    for (int i = 1; i < kMetaFieldCount; ++i) {
        if (opts & 2u) want[i] = true;                                        // all
        else if (opts & (1u << (i + 1))) want[i] = true;                      // artist .. year ticked (bits 2..5)
        else if ((opts & 1u) && !(opts & 0x3Cu)) want[i] = meta_display_value(path, i, e).empty();   // missing only
    }
    return want;
}

// Ticks / unticks FETCH option i (the rules live in meta_fetch_opt_toggle()).
void App::meta_toggle_fetch_opt(int i) {
    const unsigned before = meta_fetch_opts_;
    meta_fetch_opts_ = meta_fetch_opt_toggle(meta_fetch_opts_, i);
    meta_persist();
    if (meta_fetch_opts_ == before) { meta_status_ = "at least one fetch option has to stay ticked"; return; }
    const bool overwrites = !(meta_fetch_opts_ & 1u);
    meta_status_ = "fetches: " + meta_fetch_opt_summary(meta_fetch_opts_)
                 + (overwrites ? " (existing tags of those kinds are overwritten)" : " (existing tags stay as they are)");
}


// SHIFT+c on the Fetch List tab. The helper is killed; the answers that had already arrived stay in the session as
// pending edits, everything not fetched yet stays on the fetch list (see poll_pending_meta_fetch()).
void App::meta_cancel_fetch() {
    if (!meta_fetch_running_.load()) { meta_status_ = "no fetch is running"; return; }
    meta_fetch_cancel_.request();
    meta_status_ = "AcoustID: cancelling ...";
}

void App::meta_start_fetch() {
    meta_ensure_session_loaded();
    if (meta_prompt_paths_.empty()) return;
    if (meta_fetch_running_.exchange(true)) {
        (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = "an AcoustID fetch is already running";
        return;
    }

    std::vector<MetaFetchRequest> reqs;
    reqs.reserve(meta_prompt_paths_.size());
    meta_fetch_want_.clear();
    int skipped = 0;
    for (size_t pi = 0; pi < meta_prompt_paths_.size(); ++pi) {
        const std::string& path = meta_prompt_paths_[pi];
        MetaFetchRequest r;
        r.path = path;
        r.want = meta_want_for(path, meta_prompt_opts_);
        bool any_wanted = false;
        for (int i = 1; i < kMetaFieldCount; ++i) any_wanted = any_wanted || r.want[i];
        if (!any_wanted) { ++skipped; continue; }   // missing only, and nothing is missing
        meta_fetch_want_[path] = r.want;
        const MetaEditEntry* e = meta_entry(path);
        RowMeta rm = meta_row_meta(path_from_utf8(path));
        r.title = (e && e->edited[static_cast<int>(MetaField::Title)])
                      ? e->value[static_cast<int>(MetaField::Title)]
                      : rm.title;
        if (r.title.empty()) r.title = path_utf8(path_from_utf8(path).stem()); // last resort: the file name
        r.artist = (e && e->edited[static_cast<int>(MetaField::Artist)])
                       ? e->value[static_cast<int>(MetaField::Artist)]
                       : rm.artist;
        reqs.push_back(std::move(r));
    }
    meta_prompt_paths_.clear();
    meta_fetch_ok_paths_.clear();
    if (reqs.empty()) {
        meta_fetch_running_ = false;
        (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) =
            "nothing to fetch -- no tag is missing ([SHIFT+n] fetches all tags; or tick tags on the FETCH LIST tab)";
        return;
    }
    meta_fetch_skipped_ = skipped;
    meta_fetch_cancel_.reset();
    {
        std::lock_guard<std::mutex> lk(meta_fetch_mutex_);
        meta_fetch_results_.clear();
        meta_fetch_progress_ = "0/" + std::to_string(reqs.size());
        meta_fetch_done_ = false;
        meta_fetch_outcome_ = MetaFetchOutcome{};
    }
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) =
        "AcoustID: fetching " + std::to_string(reqs.size()) + " title" + (reqs.size() == 1 ? "" : "s") + " ...";

    if (meta_fetch_thread_.joinable()) meta_fetch_thread_.join(); // previous batch already finished (running_ was false)
    meta_fetch_thread_ = std::thread([this, reqs]() {
        run_guarded("acoustid fetch", [&] {
            int total = static_cast<int>(reqs.size());
            int done = 0;
            // One python process fingerprints the whole batch (fpcalc) and
            // paces itself to AcoustID's 3-requests-per-second limit, printing
            // a JSON line per lookup, so results are applied to the session as
            // they land instead of only after the last one.
            MetaFetchOutcome out = run_acoustid_fetch(reqs, meta_script_,
                [this, total, &done](const MetaFetchResult& r) {
                    std::lock_guard<std::mutex> lk(meta_fetch_mutex_);
                    meta_fetch_results_.push_back(r);
                    ++done;
                    meta_fetch_progress_ = std::to_string(done) + "/" + std::to_string(total);
                }, &meta_fetch_cancel_);
            std::lock_guard<std::mutex> lk(meta_fetch_mutex_);
            meta_fetch_outcome_ = out;
            meta_fetch_done_ = true;
        });
    });
}

// Called from the render loop: drains whatever the batch produced since the
// last frame (each result becomes pending edits, highlighted exactly like
// typed ones) and finalises it once the worker reports done.
void App::poll_pending_meta_fetch() {
    std::vector<MetaFetchResult> incoming;
    std::string progress;
    bool done = false;
    MetaFetchOutcome outcome;
    {
        std::lock_guard<std::mutex> lk(meta_fetch_mutex_);
        // All pushes happen under this same lock, so reading `done` in the
        // same critical section as the swap means a true `done` implies no
        // further results are coming.
        incoming.swap(meta_fetch_results_);
        progress = meta_fetch_progress_;
        done = meta_fetch_done_;
        if (done) { outcome = meta_fetch_outcome_; meta_fetch_done_ = false; }
    }

    if (!incoming.empty()) {
        int applied = 0;
        for (const auto& r : incoming) {
            if (!r.ok) continue;
            // Only the tags this entry was allowed to fetch are written; the rest of the answer is ignored.
            std::array<bool, kMetaFieldCount> want{};
            want.fill(true);
            auto wit = meta_fetch_want_.find(r.path);
            if (wit != meta_fetch_want_.end()) want = wit->second;
            MetaEditEntry& e = meta_touch_entry(r.path);
            bool any = false;
            for (int i = 1; i < kMetaFieldCount; ++i) { // 0 = file name: AcoustID never renames anything
                if (!r.set[i] || !want[i]) continue;
                e.edited[i] = true;
                e.fetched[i] = true;                    // a fetched suggestion (several candidates -> shown in the legend colour)
                e.value[i] = r.value[i];
                e.choices[i] = r.choices[i];
                e.choice_aux[i] = r.choice_aux[i];
                if (!e.choices[i].empty()) e.value[i] = e.choices[i].front();   // the shown value is the first candidate
                any = true;
            }
            if (!r.note.empty() && meta_fetch_note_.empty()) meta_fetch_note_ = r.note;
            if (any) { ++applied; meta_fetch_ok_paths_.push_back(r.path); }
        }
        if (applied) { meta_persist(); meta_refresh_lib_view(); }
        if (!progress.empty()) {
            (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = "AcoustID: " + progress + " ...";
        }
    }
    if (!done) return;

    if (meta_fetch_thread_.joinable()) meta_fetch_thread_.join();
    meta_fetch_running_ = false;

    // Everything that came back is now in the session, so drop it from the
    // fetch list -- what remains is exactly what still needs another try.
    if (!meta_fetch_ok_paths_.empty() && !meta_fetch_list_.empty()) {
        std::vector<std::string> keep;
        for (const auto& p : meta_fetch_list_) {
            if (std::find(meta_fetch_ok_paths_.begin(), meta_fetch_ok_paths_.end(), p) == meta_fetch_ok_paths_.end()) {
                keep.push_back(p);
            }
        }
        meta_fetch_list_ = std::move(keep);
        meta_refresh_lib_view();
    }
    meta_fetch_ok_paths_.clear();
    int need_choice = 0;
    for (const auto& en : meta_session_) for (int i = 0; i < kMetaFieldCount; ++i) if (en.needs_choice(i)) ++need_choice;

    std::string msg;
    if (outcome.cancelled) {
        msg = "AcoustID: cancelled after " + std::to_string(outcome.ok_count) + " ok";
        if (outcome.fail_count > 0) msg += ", " + std::to_string(outcome.fail_count) + " failed";
        msg += " -- the titles not fetched yet stay on the fetch list";
    } else if (outcome.python_missing || outcome.script_missing) {
        msg = "AcoustID: " + outcome.error;
    } else if (outcome.ok_count > 0 || outcome.fail_count > 0) {
        msg = "AcoustID: " + std::to_string(outcome.ok_count) + " ok";
        if (outcome.fail_count > 0) {
            msg += ", " + std::to_string(outcome.fail_count) + " failed";
            if (!outcome.error.empty()) msg += " (" + outcome.error + ")";
        }
    } else {
        msg = "AcoustID: " + (outcome.error.empty() ? std::string("no result") : outcome.error);
    }
    if (meta_fetch_skipped_ > 0) msg += ", " + std::to_string(meta_fetch_skipped_) + " skipped (nothing missing)";
    if (!meta_fetch_note_.empty()) msg += " [" + meta_fetch_note_ + "]";
    if (need_choice > 0) msg += " -- " + std::to_string(need_choice) + " field" + (need_choice == 1 ? " needs" : "s need") + " a choice (ENTER on the tag)";
    meta_fetch_skipped_ = 0;
    meta_fetch_note_.clear();
    meta_fetch_want_.clear();
    meta_persist();
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = msg;
    log_event(msg);
}

// Ctrl+Shift+S -> Y. Writes every pending edit to disk, keeps the failures
// in the session so they can be retried, and only clears the backup when
// nothing is left over.
void App::meta_apply_session() {
    meta_ensure_session_loaded();
    if (meta_save_.active) return;
    if (meta_session_.empty()) {
        (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = "nothing to save -- no pending edits";
        return;
    }
    meta_save_ = MetaSaveJob{};
    meta_save_.active = true;
    meta_save_.todo = meta_session_;   // the working copy; meta_session_ itself shrinks as files are written
    meta_save_.started = meta_save_.last_persist = std::chrono::steady_clock::now();
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = "saving " + std::to_string(meta_save_.todo.size()) + " file" + (meta_save_.todo.size() == 1 ? "" : "s") + " ...";
    poll_meta_save();   // a few files are done before the next frame
}

// One slice of the save: files are written for about 0.12 s, then the render loop gets control back (it draws the progress
// panel and reads ESC). Called every frame; does nothing without a job.
void App::poll_meta_save() {
    if (!meta_save_.active) return;
    MetaSaveJob& job = meta_save_;
    const auto t0 = std::chrono::steady_clock::now();
    while (job.next < job.todo.size() && !job.cancel &&
           std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(120)) {
        const MetaEditEntry& e = job.todo[job.next++];
        std::string err, new_path;
        if (apply_meta_entry(e, &err, &new_path)) {
            ++job.ok;
            {
                // The tags on disk just changed, but row_meta_cache_ is
                // keyed by path and only ever filled in once per path (see
                // meta_row_meta() and rescan_library()'s background sweep,
                // which explicitly SKIPS any path already cached). Left
                // alone, a title-only edit -- same path, so rescan_library()
                // never re-resolves it -- would keep showing the pre-edit
                // tags (including "missing title") forever, even though the
                // file itself now has the new title. Dropping the cache
                // entry here (old path, and the new one if renamed) is what
                // makes the next read actually hit the file again.
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                row_meta_cache_.erase(e.path);
                search_idx_.clear();   // the tags on disk changed: the index takes them from the cache again
                if (!new_path.empty() && new_path != e.path) row_meta_cache_.erase(new_path);
            }
            // The playing file now carries these values: make them what the
            // main screen's block is loaded with (it is read once per track),
            // so it keeps showing them after the pending edit is gone.
            if (has_track_ && current_is_local_ && path_utf8(current_path_) == e.path) {
                overlay_meta_entry(metadata_, e, current_path_);
            }
            if (!new_path.empty() && new_path != e.path) {
                // The file was renamed: keep every in-app reference pointed
                // at the new name so playback/queue/lyrics don't break.
                if (path_utf8(current_path_) == e.path) current_path_ = path_from_utf8(new_path);   // (metadata_ was already updated above)
                for (auto& q : queue_) {
                    if (q.is_local && path_utf8(q.local_path) == e.path) q.local_path = path_from_utf8(new_path);
                }
                for (auto& p : meta_fetch_list_) if (p == e.path) p = new_path;
                for (auto& t : playlist_edit_tracks_) {
                    if (path_utf8(t.path) == e.path) {
                        t.path = path_from_utf8(new_path);
                        t.title = path_utf8(t.path.stem());
                        t.missing = false;
                    }
                }
                job.renamed.emplace_back(e.path, new_path);   // saved playlists follow in meta_save_finish()
            }
        } else {
            if (job.first_error.empty()) job.first_error = err;
            job.failed.push_back(e);
        }
    }
    // The session on screen (and in the backup file) is always: failures so far + what is still to do.
    meta_session_ = job.failed;
    meta_session_.insert(meta_session_.end(), job.todo.begin() + static_cast<long>(job.next), job.todo.end());
    if (job.next >= job.todo.size() || job.cancel) { meta_save_finish(); return; }
    if (std::chrono::steady_clock::now() - job.last_persist > std::chrono::seconds(2)) {   // survives a crash half-way
        meta_persist();
        job.last_persist = std::chrono::steady_clock::now();
    }
    const std::string msg = "saving " + std::to_string(job.next) + " / " + std::to_string(job.todo.size()) + " ...";
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = msg;
}

void App::meta_save_finish() {
    MetaSaveJob job = std::move(meta_save_);
    meta_save_ = MetaSaveJob{};   // inactive
    const int failed = static_cast<int>(job.failed.size());
    const int not_done = static_cast<int>(job.todo.size() - job.next);
    meta_persist();
    if (!job.renamed.empty()) {
        // Saved playlists hold file paths: let them follow the renamed files.
        for (const fs::path& dir : playlist_dirs()) PlaylistManager::rename_paths(dir, job.renamed);
        VideoLinks::get().rename_paths(job.renamed);   // the track <-> video pairs follow the files too
        video_for_track_.clear(); ++video_rev_;
    }
    if (job.ok > 0) {
        // Names and tags changed -- rebuild the lists. rescan_library() only
        // refreshes the app's own views, so the meta menu's LIBRARY panel has
        // to be rebuilt as well: after a rename it would otherwise keep
        // showing (and keep handing out, via meta_hovering_path()) the old
        // path, and every edit made against it from there on would be filed
        // under a file that no longer exists.
        rescan_library();
        meta_refresh_lib_view();
        resolve_playlist_tracks_async();   // an open playlist editor shows the new tags
    }
    std::string msg;
    if (failed == 0) {
        msg = "applied " + std::to_string(job.ok) + " pending edit" + (job.ok == 1 ? "" : "s") + " to disk";
    } else {
        msg = "applied " + std::to_string(job.ok) + ", " + std::to_string(failed) + " failed: " + job.first_error;
    }
    if (not_done > 0) msg += " -- stopped, " + std::to_string(not_done) + " not saved yet (still pending)";
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = msg;
    log_event(msg);
}

std::vector<std::string> App::build_meta_save_panel() const {
    const int W = kMetaSavePanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border + settings_.box_vertical + "\x1b[0m";
    auto centred = [&](const std::string& plain) {
        const int fill = std::max(0, inner - display_width(plain));
        const int left = fill / 2;
        return bar + " " + std::string(static_cast<size_t>(left), ' ') + plain + std::string(static_cast<size_t>(fill - left), ' ') + " " + bar;
    };
    const size_t total = std::max<size_t>(1, meta_save_.todo.size());
    const size_t done = std::min(meta_save_.next, total);
    const int bar_w = inner - 2;
    const int filled = static_cast<int>(static_cast<double>(bar_w) * static_cast<double>(done) / static_cast<double>(total));
    std::string progress = "[";
    for (int i = 0; i < bar_w - 2; ++i) progress += i < filled ? "\u2588" : "\u2591";
    progress += "]";
    std::vector<std::string> lines;
    lines.push_back(box_top("SAVING", W, border));
    lines.push_back(centred(""));
    lines.push_back(centred("Please wait, this may take a few seconds"));
    lines.push_back(centred(std::to_string(done) + " / " + std::to_string(meta_save_.todo.size()) + " files"));
    lines.push_back(centred(progress));
    lines.push_back(centred(""));
    lines.push_back(box_bottom(W, meta_save_.cancel ? "stopping ..." : "[ESC] stop after the current file", border_bottom));
    return lines;
}

// Ctrl+Shift+X -> Y. The audio files were never written to by an edit, so
// discarding only means throwing the pending values (and their backup file)
// away.
void App::meta_discard_session() {
    meta_ensure_session_loaded();
    int had = static_cast<int>(meta_session_.size());
    meta_session_.clear();
    meta_fetch_list_.clear();
    meta_fetch_ok_paths_.clear();
    meta_persist(); // empty -> the autosave backup file goes away
    std::string msg = "editing session discarded";
    if (had > 0) msg += " (" + std::to_string(had) + " file" + (had == 1 ? "" : "s") + " had pending edits)";
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = msg;
    log_event(msg);
}

// Shared Y/N/ESC handler for the three confirmations. Returns true whenever
// it consumed the key -- including the "a prompt is up, swallow everything
// else" case -- and false only when there is no prompt at all, which is what
// lets handle_key() call it unconditionally before its mode dispatch.
bool App::handle_meta_prompt_key(int key) {
    if (meta_prompt_ == MetaPrompt::None) return false;
    auto say = [this](const std::string& s) {
        (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = s;
    };
    if (key == 'y' || key == 'Y') {
        MetaPrompt p = meta_prompt_;
        meta_prompt_ = MetaPrompt::None;
        // Careful with meta_prompt_paths_: the fetch batch is built FROM it,
        // so it may only be cleared by the actions that don't read it -- an
        // eager clear here made meta_start_fetch() see an empty list and
        // return without fetching anything (silently: the confirmation had
        // already been dismissed).
        if (p == MetaPrompt::Save) {
            meta_prompt_paths_.clear();
            meta_apply_session();
        } else if (p == MetaPrompt::Discard) {
            meta_prompt_paths_.clear();
            meta_discard_session();
        } else if (p == MetaPrompt::ClearFetch) {
            // only the list: pending edits (fetched tags included) stay until CTRL+s or CTRL+SHIFT+x
            meta_prompt_paths_.clear();
            const size_t n = meta_fetch_list_.size();
            meta_fetch_list_.clear();
            meta_fetch_ok_paths_.clear();
            meta_fetch_selected_ = 0;
            meta_persist();
            say("fetch list emptied (" + std::to_string(n) + " title" + (n == 1 ? "" : "s") + "); pending edits are kept");
        } else {
            meta_start_fetch(); // clears meta_prompt_paths_ itself
        }
        return true;
    }
    if (key == 'n' || key == 'N' || key == 27) {
        meta_prompt_ = MetaPrompt::None;
        meta_prompt_paths_.clear();
        say("cancelled");
        return true;
    }
    return true; // swallow everything else while a confirmation is up
}

} // namespace muisc
