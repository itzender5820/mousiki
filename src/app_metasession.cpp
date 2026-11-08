// app_metasession.cpp
// Part of class App (src/app.h): The meta editor session: filters, the edit list, fetching tags, saving.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "playlist_state.h"
#include "meta_state.h"
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
//     fetched goes into meta_->session and is written to an autosave backup
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
    if (meta_->prompt_opts & 1u) return head + "Only empty tags are filled in; existing tags stay as they are. Continue?";
    return head + "Existing " + (meta_->prompt_opts & 2u ? std::string("tags") : meta_fetch_opt_summary(meta_->prompt_opts) + " tags")
         + " are overwritten. Continue?";
}

void App::meta_open() {
    meta_->status.clear();          // before the loader below, which may set it
    meta_->prompt = MetaPrompt::None;
    meta_->prompt_paths.clear();
    meta_ensure_session_loaded();  // reopening never loses what's already pending
    mode_ = Mode::MetaEdit;
    text_disengage();
    meta_->tab = 0;
    meta_->focus = 0;
    meta_->query.clear();
    meta_->filter = 0; // a fresh open shows the whole library again
    meta_->lib_selected = 0;
    meta_->field = 0;
    meta_->field_editing = false;
    meta_->fetch_selected = 0;
    meta_refresh_lib_view();
    // The song that plays now is where the list starts (the view is unfiltered here, whatever was typed or filtered before).
    if (has_track_ && current_is_local_) {
        for (size_t i = 0; i < meta_->lib_view.size(); ++i)
            if (meta_->lib_view[i].path == current_path_) { meta_->lib_selected = static_cast<int>(i); break; }
    }
}

// Loads the autosaved backup exactly once per run. Called from meta_open()
// AND from every session-touching action, because Shift+B reaches the same
// session from Browse mode without ever opening the menu.
void App::meta_ensure_session_loaded() {
    if (meta_->session_loaded) return;
    meta_->session_loaded = true;
    std::vector<MetaEditEntry> entries;
    std::vector<std::string> fetch_list;
    unsigned fetch_opts = meta_->fetch_opts;
    if (load_meta_session(entries, fetch_list, fetch_opts)) {
        meta_->session = std::move(entries);
        meta_->fetch_list = std::move(fetch_list);
        meta_->fetch_opts = fetch_opts;
        meta_->status = "restored autosaved session: " + std::to_string(meta_->session.size())
                     + " file" + (meta_->session.size() == 1 ? "" : "s") + " with pending edits";
        log_event("restored meta edit session (" + std::to_string(meta_->session.size()) + " files)");
    }
    meta_refresh_lib_view();
}

void App::meta_refresh_lib_view() {
    meta_->folder_hover.clear();   // (the folder view's header cursor: the rows are rebuilt)
    // With the 'r' resort on, this rebuild re-orders the pane underneath the
    // cursor, so remember the hovered row by PATH first: clamping the old
    // INDEX would silently point the picker -- and with it the fields panel,
    // which is what the user is editing -- at a different track.
    std::string keep;
    if ((meta_->resort_edited || meta_->sort_added) && !meta_->lib_view.empty()) keep = meta_hovering_path();

    meta_->lib_view = filter_local_or_folders(meta_->query, meta_->media);   // (SHIFT+v: audio / video + audio / video; the search, folder view and the missing-tag filters below still apply)

    // Missing-tag filter ('x' / Shift+T/A/Y). meta_row_meta() is a native,
    // in-process tag read cached in row_meta_cache_, so the first pass costs
    // one header parse per file and everything after that is a lookup --
    // no ffprobe, no subprocess. A pending session edit counts as the value
    // the file WOULD have, so filtering right after typing (or deleting) a
    // title reflects what is on screen instead of what is on disk.
    if (meta_->filter != 0) {
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
            if (meta_->filter == 1) { // "no meta data" = every tag field empty
                for (int f = 1; f < kMetaFieldCount; ++f)
                    if (!value_of(t, f).empty()) return false;
                return true;
            }
            const int field = (meta_->filter == 3) ? 1   // ARTIST
                            : (meta_->filter == 2) ? 2   // TITLE
                            : (meta_->filter == 5) ? 3   // ALBUM
                            : 4;                        // YEAR
            return value_of(t, field).empty();
        };
        meta_->lib_view.erase(std::remove_if(meta_->lib_view.begin(), meta_->lib_view.end(),
                                            [&](const LocalTrack& t) { return !matches(t); }),
                             meta_->lib_view.end());
    }

    if (meta_->sort_added) {   // 'n': newest arrived first; 'r' (below) still lifts the edited files above this order
        std::vector<std::pair<long long, size_t>> keyed;
        keyed.reserve(meta_->lib_view.size());
        for (size_t i = 0; i < meta_->lib_view.size(); ++i) keyed.emplace_back(added_time_of(meta_->lib_view[i].path), i);
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<LocalTrack> sorted;
        sorted.reserve(keyed.size());
        for (const auto& k : keyed) sorted.push_back(std::move(meta_->lib_view[k.second]));
        meta_->lib_view = std::move(sorted);
    }

    if (meta_->resort_edited) {
        // Strict "edited before not edited" on a stable sort is a stable
        // partition: every file with a pending edit moves into one block at
        // the top, and both blocks keep the exact order the pane already had
        // -- the normal alphabetical/scan order, or the fuzzy ranking while
        // a search is active (so a query's relevance order is never thrown
        // away). Toggling 'r' off simply rebuilds without this step, i.e.
        // straight back to that order.
        std::stable_sort(meta_->lib_view.begin(), meta_->lib_view.end(),
                         [this](const LocalTrack& a, const LocalTrack& b) {
                             auto edited = [this](const LocalTrack& t) {
                                 const MetaEditEntry* e = meta_entry(path_utf8(t.path));
                                 return e && e->any_edited();
                             };
                             return edited(a) && !edited(b);
                         });
    }
    if (!keep.empty()) {
        for (size_t i = 0; i < meta_->lib_view.size(); ++i) {
            if (path_utf8(meta_->lib_view[i].path) == keep) {
                meta_->lib_selected = static_cast<int>(i);
                break;
            }
        }
    }

    meta_->lib_selected = std::clamp(meta_->lib_selected, 0,
        std::max(0, static_cast<int>(meta_->lib_view.size()) - 1));
    meta_->fetch_selected = std::clamp(meta_->fetch_selected, 0,
        std::max(0, static_cast<int>(meta_->fetch_list.size()) - 1));
}

// 'r': flip the library pane between its normal order and "every file with
// pending edits on top". Nothing else about the list changes -- same rows,
// same filter, same selection (meta_refresh_lib_view() follows it by path).
void App::meta_toggle_resort() {
    meta_->resort_edited = !meta_->resort_edited;
    meta_refresh_lib_view();
    meta_->lib_selected = 0;   // the view starts at the top again
    if (meta_->resort_edited) {
        int n = 0;
        for (const auto& e : meta_->session) if (e.any_edited()) ++n;
        meta_->status = "library resorted: " + std::to_string(n) + " edited file"
                     + (n == 1 ? "" : "s") + " on top" + meta_filter_note();
    } else {
        meta_->status = "library order: alphabetical again" + meta_filter_note();
    }
}

// The reminder added to the status line of 'r' / 'n': a missing-tag filter (x / T / A / L / Y) that is still on hides every file
// that is not missing that tag, which looks like "the resort found nothing".
std::string App::meta_filter_note() const {
    if (meta_->filter == 0) return "";
    return std::string("  NOTE: [") + meta_filter_label() + "] filter active, hit [ESC] to clear filter";
}

// 'n': flip the library pane between its normal order and "the files that arrived last first" (see added_time_of()).
void App::meta_toggle_sort_added() {
    meta_->sort_added = !meta_->sort_added;
    if (meta_->sort_added) { std::lock_guard<std::mutex> lk(added_time_mutex_); added_time_cache_.clear(); }   // fresh times
    meta_refresh_lib_view();
    meta_->status = std::string(meta_->sort_added ? "library order: recently added first" : "library order: alphabetical again") + meta_filter_note();
}

const char* App::meta_filter_label() const {
    switch (meta_->filter) {
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
    if (!has_track_ || !current_is_local_) { meta_->status = "no local song plays right now"; return; }
    meta_->query.clear();
    meta_->filter = 0;
    meta_->folder_hover.clear();
    meta_refresh_lib_view();
    for (size_t i = 0; i < meta_->lib_view.size(); ++i)
        if (meta_->lib_view[i].path == current_path_) { meta_->lib_selected = static_cast<int>(i); meta_->status = "the song that plays now (search and filters removed)"; return; }
    meta_->status = "the song that plays now is not in the library list";
}

// 'x' / Shift+T / Shift+A / Shift+Y: narrow the library pane to files a
// given tag is (or, for 'x', every tag is) missing. The same key clears it,
// which is what keeps four keys enough for five states, and the status line
// always says which filter is up and how much of the library survived it --
// the pane's title carries the label too, so an active filter is never
// something you have to remember.
void App::meta_toggle_filter(int filter) {
    meta_->filter = (meta_->filter == filter) ? 0 : filter;
    meta_refresh_lib_view();
    meta_->lib_selected = 0;   // the view starts at the top again
    meta_->folder_hover.clear();
    const std::string shown = std::to_string(meta_->lib_view.size());
    if (meta_->filter == 0) meta_->status = "filter cleared - " + shown + " files";
    else meta_->status = std::string("filter: ") + meta_filter_label() + " - " + shown + " files (press the key again or [ESC] to clear)";
}

// The autosave: save whenever there is something to restore, remove the file
// when there isn't. Deliberately NOT tied to any "was it applied" flag --
// leaving the app always ends with whatever the user last edited still on
// disk as a backup, which is the whole point of the session file.
void App::meta_persist() {
    if (!meta_->session_loaded) return;
    if (meta_->session.empty() && meta_->fetch_list.empty()) delete_meta_session();
    else save_meta_session(meta_->session, meta_->fetch_list, meta_->fetch_opts);
}

const MetaEditEntry* App::meta_entry(const std::string& path) const {
    for (const auto& e : meta_->session) if (e.path == path) return &e;
    return nullptr;
}

MetaEditEntry& App::meta_touch_entry(const std::string& path) {
    for (auto& e : meta_->session) if (e.path == path) return e;
    MetaEditEntry e;
    e.path = path;
    meta_->session.push_back(std::move(e));
    return meta_->session.back();
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
    if (p.empty()) { meta_->status = "no track selected"; return; }
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
    meta_->status = "playing: " + t.title;
}

std::string App::meta_hovering_path() const {
    if (meta_->tab == 1) { // the fetch list's rows ARE the titles
        if (meta_->fetch_list.empty()) return {};
        int i = std::clamp(meta_->fetch_selected, 0, static_cast<int>(meta_->fetch_list.size()) - 1);
        return meta_->fetch_list[i];
    }
    if (meta_->lib_view.empty()) return {};
    int i = std::clamp(meta_->lib_selected, 0, static_cast<int>(meta_->lib_view.size()) - 1);
    return path_utf8(meta_->lib_view[i].path);
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
    meta_->hover_values.fill(std::string());
    meta_->hover_resolved = false;
    if (mode_ != Mode::MetaEdit) return;
    std::string path = meta_hovering_path();
    if (path.empty()) return;
    const MetaEditEntry* e = meta_entry(path);
    meta_->hover_resolved = meta_row_meta(path_from_utf8(path)).tags_resolved;
    for (int i = 0; i < kMetaFieldCount; ++i) meta_->hover_values[i] = meta_display_value(path, i, e);
}

// SHIFT+r in the main UI and in the meta data editor.
void App::rescan_now() {
    const size_t before = all_local_tracks_.size();
    rescan_library();
    if (mode_ == Mode::MetaEdit) meta_refresh_lib_view();
    const long d = static_cast<long>(all_local_tracks_.size()) - static_cast<long>(before);
    std::string msg = "library rescanned: " + std::to_string(all_local_tracks_.size()) + " tracks";
    if (d > 0) msg += " (+" + std::to_string(d) + ")"; else if (d < 0) msg += " (" + std::to_string(d) + ")";
    if (mode_ == Mode::MetaEdit) meta_->status = msg; else status_line_ = msg;
}

// --- the choice overlay -----------------------------------------------------------------------------------------
void App::meta_choice_open() {
    const std::string path = meta_hovering_path();
    const MetaEditEntry* e = meta_entry(path);
    if (!e || meta_->field < 0 || meta_->field >= kMetaFieldCount || !e->needs_choice(meta_->field)) return;
    meta_->choice_open = true;
    meta_->choice_field = meta_->field;
    meta_->choice_sel = 0;
    for (size_t k = 0; k < e->choices[meta_->field].size(); ++k) {
        if (e->choices[meta_->field][k] == e->value[meta_->field]) { meta_->choice_sel = static_cast<int>(k); break; }
    }
}

void App::meta_choice_pick() {
    const std::string path = meta_hovering_path();
    const MetaEditEntry* ce = meta_entry(path);
    const int f = meta_->choice_field;
    if (!ce || f < 1 || f >= kMetaFieldCount || meta_->choice_sel < 0 ||
        meta_->choice_sel >= static_cast<int>(ce->choices[f].size())) { meta_->choice_open = false; return; }
    MetaEditEntry& e = meta_touch_entry(path);
    const size_t k = static_cast<size_t>(meta_->choice_sel);
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
    meta_->choice_open = false;
    meta_persist();
    meta_refresh_lib_view();
    meta_->status = std::string(meta_field_label(f)) + ": " + e.value[f];
}

void App::meta_choice_discard() {
    const std::string path = meta_hovering_path();
    const int f = meta_->choice_field;
    meta_->choice_open = false;
    if (f < 1 || f >= kMetaFieldCount) return;
    for (size_t i = 0; i < meta_->session.size(); ++i) {
        if (meta_->session[i].path != path) continue;
        meta_->session[i].clear_field(f);      // back to what the file has
        if (!meta_->session[i].any_edited()) meta_->session.erase(meta_->session.begin() + static_cast<long>(i));
        break;
    }
    meta_persist();
    meta_refresh_lib_view();
    meta_->status = std::string("discarded the fetched ") + meta_field_label(f) + " and its options";
}

bool App::handle_meta_choice_key(int key) {
    if (!meta_->choice_open) return false;
    const MetaEditEntry* e = meta_entry(meta_hovering_path());
    const int n = (e && meta_->choice_field >= 0 && meta_->choice_field < kMetaFieldCount)
                      ? static_cast<int>(e->choices[meta_->choice_field].size()) : 0;
    if (n == 0) { meta_->choice_open = false; return true; }
    const bool arrow = last_key_was_arrow();
    if (key == 27) { meta_->choice_open = false; return true; }
    if (arrow && key == 'A') { if (meta_->choice_sel > 0) --meta_->choice_sel; return true; }
    if (arrow && key == 'B') { if (meta_->choice_sel + 1 < n) ++meta_->choice_sel; return true; }
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
    const int f = std::clamp(meta_->choice_field, 0, kMetaFieldCount - 1);
    const int n = e ? static_cast<int>(e->choices[f].size()) : 0;
    lines.push_back(box_top(std::string("CHOOSE ") + meta_field_label(f) + " (" + std::to_string(n) + " found)", W, border));
    auto text_row = [&](const std::string& sgr, const std::string& text) {
        return obar + sgr + pad_right(truncate_str(" " + text, inner), inner) + "\x1b[0m" + obar;
    };
    lines.push_back(text_row(legend_sgr(settings_), "The first one is in the field now -- pick the right one:"));
    const int shown = std::max(1, std::min(n, 8));
    const int first = std::clamp(meta_->choice_sel - shown / 2, 0, std::max(0, n - shown));
    for (int r = 0; r < shown; ++r) {
        const int k = first + r;
        if (k >= n || !e) { lines.push_back(obar + std::string(inner, ' ') + obar); continue; }
        std::string value = e->choices[f][static_cast<size_t>(k)];
        std::string aux = (static_cast<size_t>(k) < e->choice_aux[f].size()) ? e->choice_aux[f][static_cast<size_t>(k)] : std::string();
        const bool current = (e->value[f] == value);
        const std::string mark = current ? " (o) " : " ( ) ";
        std::string row;
        if (aux.empty()) {
            row = pad_right(truncate_str(mark + apply_font_map(value, settings_.font_map), inner - 1), inner - 1) + " ";
        } else {
            const int aux_w = static_cast<int>(aux.size()) + 2;
            row = pad_right(truncate_str(mark + apply_font_map(value, settings_.font_map), inner - 1 - aux_w), inner - 1 - aux_w) + aux + "  " + " ";
        }
        if (k == meta_->choice_sel) row = "\x1b[7m" + row + "\x1b[0m";
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
    if (path.empty()) { meta_->status = "no title selected"; return; }
    if (meta_take_linked_tags(path)) { meta_->status = "video: took the tags of the track it is paired with (queue it again to fetch its own)"; return; }
    for (const auto& p : meta_->fetch_list) {
        if (p == path) {
            meta_->status = "already on the fetch list (" + std::to_string(meta_->fetch_list.size()) + " queued)";
            return;
        }
    }
    meta_->fetch_list.push_back(path);
    meta_->fetch_selected = static_cast<int>(meta_->fetch_list.size()) - 1;
    meta_persist();
    meta_->status = "added to fetch list (" + std::to_string(meta_->fetch_list.size()) + " queued)";
}

// SHIFT+f in the library pane: puts every title that pane shows right now -- so the search AND the missing-tag filter
// (x, SHIFT+t/a/l/y) are applied -- on the fetch list. Titles already on it are skipped, the order of the pane is kept.
void App::meta_add_view_to_fetch() {
    meta_ensure_session_loaded();
    if (meta_->lib_view.empty()) { meta_->status = "nothing to add -- the library pane is empty"; return; }
    std::unordered_set<std::string> queued(meta_->fetch_list.begin(), meta_->fetch_list.end());
    size_t added = 0;
    for (const auto& t : meta_->lib_view) {
        std::string path = path_utf8(t.path);
        if (queued.insert(path).second) { meta_->fetch_list.push_back(std::move(path)); ++added; }
    }
    if (added == 0) {
        meta_->status = "all " + std::to_string(meta_->lib_view.size()) + " titles are on the fetch list already (" + std::to_string(meta_->fetch_list.size()) + " queued)";
        return;
    }
    meta_persist();
    meta_->status = "added " + std::to_string(added) + (added == 1 ? " title" : " titles") + " to the fetch list"
                 + (added < meta_->lib_view.size() ? " (" + std::to_string(meta_->lib_view.size() - added) + " already on it)" : std::string())
                 + " -- " + std::to_string(meta_->fetch_list.size()) + " queued";
}

// DEL / 'd' -- same "unqueue" gesture as the main list's removal key.
void App::meta_remove_hovering() {
    meta_ensure_session_loaded();
    std::string path = meta_hovering_path();
    if (path.empty()) return;
    for (size_t i = 0; i < meta_->fetch_list.size(); ++i) {
        if (meta_->fetch_list[i] == path) {
            meta_->fetch_list.erase(meta_->fetch_list.begin() + static_cast<long>(i));
            meta_refresh_lib_view();
            meta_persist();
            meta_->status = "removed from fetch list (" + std::to_string(meta_->fetch_list.size()) + " queued)";
            return;
        }
    }
    meta_->status = "not on the fetch list";
}

void App::meta_prompt_single_fetch(const std::string& path, bool all) {
    if (path.empty()) return;
    meta_ensure_session_loaded();
    meta_->prompt = MetaPrompt::Fetch;
    meta_->prompt_opts = all ? 2u : 1u;   // SHIFT+b: only the missing tags; SHIFT+n: all tags (the boxes do not apply)
    meta_->prompt_paths.assign(1, path);
    // In the menu the footer renders the disclaimer; in Browse there is no
    // footer, so it goes through the status line instead.
    if (mode_ == Mode::MetaEdit) meta_->status.clear();
    else status_line_ = meta_fetch_disclaimer();
}

void App::meta_prompt_list_fetch() {
    meta_ensure_session_loaded();
    if (meta_->fetch_list.empty()) {
        meta_->status = "fetch list is empty -- press 'a' on a track to queue it";
        return;
    }
    meta_->prompt = MetaPrompt::Fetch;
    meta_->prompt_opts = meta_->fetch_opts;
    meta_->prompt_paths = meta_->fetch_list;
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
    const unsigned before = meta_->fetch_opts;
    meta_->fetch_opts = meta_fetch_opt_toggle(meta_->fetch_opts, i);
    meta_persist();
    if (meta_->fetch_opts == before) { meta_->status = "at least one fetch option has to stay ticked"; return; }
    const bool overwrites = !(meta_->fetch_opts & 1u);
    meta_->status = "fetches: " + meta_fetch_opt_summary(meta_->fetch_opts)
                 + (overwrites ? " (existing tags of those kinds are overwritten)" : " (existing tags stay as they are)");
}


// SHIFT+c on the Fetch List tab. The helper is killed; the answers that had already arrived stay in the session as
// pending edits, everything not fetched yet stays on the fetch list (see poll_pending_meta_fetch()).
void App::meta_cancel_fetch() {
    if (!meta_->fetch_running.load()) { meta_->status = "no fetch is running"; return; }
    meta_->fetch_cancel.request();
    meta_->status = "AcoustID: cancelling ...";
}

void App::meta_start_fetch() {
    meta_ensure_session_loaded();
    if (meta_->prompt_paths.empty()) return;
    if (meta_->fetch_running.exchange(true)) {
        (mode_ == Mode::MetaEdit ? meta_->status : status_line_) = "an AcoustID fetch is already running";
        return;
    }

    std::vector<MetaFetchRequest> reqs;
    reqs.reserve(meta_->prompt_paths.size());
    meta_->fetch_want.clear();
    int skipped = 0;
    for (size_t pi = 0; pi < meta_->prompt_paths.size(); ++pi) {
        const std::string& path = meta_->prompt_paths[pi];
        MetaFetchRequest r;
        r.path = path;
        r.want = meta_want_for(path, meta_->prompt_opts);
        bool any_wanted = false;
        for (int i = 1; i < kMetaFieldCount; ++i) any_wanted = any_wanted || r.want[i];
        if (!any_wanted) { ++skipped; continue; }   // missing only, and nothing is missing
        meta_->fetch_want[path] = r.want;
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
    meta_->prompt_paths.clear();
    meta_->fetch_ok_paths.clear();
    if (reqs.empty()) {
        meta_->fetch_running = false;
        (mode_ == Mode::MetaEdit ? meta_->status : status_line_) =
            "nothing to fetch -- no tag is missing ([SHIFT+n] fetches all tags; or tick tags on the FETCH LIST tab)";
        return;
    }
    meta_->fetch_skipped = skipped;
    meta_->fetch_cancel.reset();
    {
        std::lock_guard<std::mutex> lk(meta_->fetch_mutex);
        meta_->fetch_results.clear();
        meta_->fetch_progress = "0/" + std::to_string(reqs.size());
        meta_->fetch_done = false;
        meta_->fetch_outcome = MetaFetchOutcome{};
    }
    (mode_ == Mode::MetaEdit ? meta_->status : status_line_) =
        "AcoustID: fetching " + std::to_string(reqs.size()) + " title" + (reqs.size() == 1 ? "" : "s") + " ...";

    if (meta_->fetch_thread.joinable()) meta_->fetch_thread.join(); // previous batch already finished (running_ was false)
    meta_->fetch_thread = std::thread([this, reqs]() {
        run_guarded("acoustid fetch", [&] {
            int total = static_cast<int>(reqs.size());
            int done = 0;
            // One python process fingerprints the whole batch (fpcalc) and
            // paces itself to AcoustID's 3-requests-per-second limit, printing
            // a JSON line per lookup, so results are applied to the session as
            // they land instead of only after the last one.
            MetaFetchOutcome out = run_acoustid_fetch(reqs, meta_->script,
                [this, total, &done](const MetaFetchResult& r) {
                    std::lock_guard<std::mutex> lk(meta_->fetch_mutex);
                    meta_->fetch_results.push_back(r);
                    ++done;
                    meta_->fetch_progress = std::to_string(done) + "/" + std::to_string(total);
                }, &meta_->fetch_cancel);
            std::lock_guard<std::mutex> lk(meta_->fetch_mutex);
            meta_->fetch_outcome = out;
            meta_->fetch_done = true;
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
        std::lock_guard<std::mutex> lk(meta_->fetch_mutex);
        // All pushes happen under this same lock, so reading `done` in the
        // same critical section as the swap means a true `done` implies no
        // further results are coming.
        incoming.swap(meta_->fetch_results);
        progress = meta_->fetch_progress;
        done = meta_->fetch_done;
        if (done) { outcome = meta_->fetch_outcome; meta_->fetch_done = false; }
    }

    if (!incoming.empty()) {
        int applied = 0;
        for (const auto& r : incoming) {
            if (!r.ok) continue;
            // Only the tags this entry was allowed to fetch are written; the rest of the answer is ignored.
            std::array<bool, kMetaFieldCount> want{};
            want.fill(true);
            auto wit = meta_->fetch_want.find(r.path);
            if (wit != meta_->fetch_want.end()) want = wit->second;
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
            if (!r.note.empty() && meta_->fetch_note.empty()) meta_->fetch_note = r.note;
            if (any) { ++applied; meta_->fetch_ok_paths.push_back(r.path); }
        }
        if (applied) { meta_persist(); meta_refresh_lib_view(); }
        if (!progress.empty()) {
            (mode_ == Mode::MetaEdit ? meta_->status : status_line_) = "AcoustID: " + progress + " ...";
        }
    }
    if (!done) return;

    if (meta_->fetch_thread.joinable()) meta_->fetch_thread.join();
    meta_->fetch_running = false;

    // Everything that came back is now in the session, so drop it from the
    // fetch list -- what remains is exactly what still needs another try.
    if (!meta_->fetch_ok_paths.empty() && !meta_->fetch_list.empty()) {
        std::vector<std::string> keep;
        for (const auto& p : meta_->fetch_list) {
            if (std::find(meta_->fetch_ok_paths.begin(), meta_->fetch_ok_paths.end(), p) == meta_->fetch_ok_paths.end()) {
                keep.push_back(p);
            }
        }
        meta_->fetch_list = std::move(keep);
        meta_refresh_lib_view();
    }
    meta_->fetch_ok_paths.clear();
    int need_choice = 0;
    for (const auto& en : meta_->session) for (int i = 0; i < kMetaFieldCount; ++i) if (en.needs_choice(i)) ++need_choice;

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
    if (meta_->fetch_skipped > 0) msg += ", " + std::to_string(meta_->fetch_skipped) + " skipped (nothing missing)";
    if (!meta_->fetch_note.empty()) msg += " [" + meta_->fetch_note + "]";
    if (need_choice > 0) msg += " -- " + std::to_string(need_choice) + " field" + (need_choice == 1 ? " needs" : "s need") + " a choice (ENTER on the tag)";
    meta_->fetch_skipped = 0;
    meta_->fetch_note.clear();
    meta_->fetch_want.clear();
    meta_persist();
    (mode_ == Mode::MetaEdit ? meta_->status : status_line_) = msg;
    log_event(msg);
}

// Ctrl+Shift+S -> Y. Writes every pending edit to disk, keeps the failures
// in the session so they can be retried, and only clears the backup when
// nothing is left over.
void App::meta_apply_session() {
    meta_ensure_session_loaded();
    if (meta_->save.active) return;
    if (meta_->session.empty()) {
        (mode_ == Mode::MetaEdit ? meta_->status : status_line_) = "nothing to save -- no pending edits";
        return;
    }
    meta_->save = MetaSaveJob{};
    meta_->save.active = true;
    meta_->save.todo = meta_->session;   // the working copy; meta_->session itself shrinks as files are written
    meta_->save.started = meta_->save.last_persist = std::chrono::steady_clock::now();
    (mode_ == Mode::MetaEdit ? meta_->status : status_line_) = "saving " + std::to_string(meta_->save.todo.size()) + " file" + (meta_->save.todo.size() == 1 ? "" : "s") + " ...";
    poll_meta_save();   // a few files are done before the next frame
}

// One slice of the save: files are written for about 0.12 s, then the render loop gets control back (it draws the progress
// panel and reads ESC). Called every frame; does nothing without a job.
void App::poll_meta_save() {
    if (!meta_->save.active) return;
    MetaSaveJob& job = meta_->save;
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
                for (auto& p : meta_->fetch_list) if (p == e.path) p = new_path;
                for (auto& t : playlist_->edit_tracks) {
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
    meta_->session = job.failed;
    meta_->session.insert(meta_->session.end(), job.todo.begin() + static_cast<long>(job.next), job.todo.end());
    if (job.next >= job.todo.size() || job.cancel) { meta_save_finish(); return; }
    if (std::chrono::steady_clock::now() - job.last_persist > std::chrono::seconds(2)) {   // survives a crash half-way
        meta_persist();
        job.last_persist = std::chrono::steady_clock::now();
    }
    const std::string msg = "saving " + std::to_string(job.next) + " / " + std::to_string(job.todo.size()) + " ...";
    (mode_ == Mode::MetaEdit ? meta_->status : status_line_) = msg;
}

void App::meta_save_finish() {
    MetaSaveJob job = std::move(meta_->save);
    meta_->save = MetaSaveJob{};   // inactive
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
    (mode_ == Mode::MetaEdit ? meta_->status : status_line_) = msg;
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
    const size_t total = std::max<size_t>(1, meta_->save.todo.size());
    const size_t done = std::min(meta_->save.next, total);
    const int bar_w = inner - 2;
    const int filled = static_cast<int>(static_cast<double>(bar_w) * static_cast<double>(done) / static_cast<double>(total));
    std::string progress = "[";
    for (int i = 0; i < bar_w - 2; ++i) progress += i < filled ? "\u2588" : "\u2591";
    progress += "]";
    std::vector<std::string> lines;
    lines.push_back(box_top("SAVING", W, border));
    lines.push_back(centred(""));
    lines.push_back(centred("Please wait, this may take a few seconds"));
    lines.push_back(centred(std::to_string(done) + " / " + std::to_string(meta_->save.todo.size()) + " files"));
    lines.push_back(centred(progress));
    lines.push_back(centred(""));
    lines.push_back(box_bottom(W, meta_->save.cancel ? "stopping ..." : "[ESC] stop after the current file", border_bottom));
    return lines;
}

// Ctrl+Shift+X -> Y. The audio files were never written to by an edit, so
// discarding only means throwing the pending values (and their backup file)
// away.
void App::meta_discard_session() {
    meta_ensure_session_loaded();
    int had = static_cast<int>(meta_->session.size());
    meta_->session.clear();
    meta_->fetch_list.clear();
    meta_->fetch_ok_paths.clear();
    meta_persist(); // empty -> the autosave backup file goes away
    std::string msg = "editing session discarded";
    if (had > 0) msg += " (" + std::to_string(had) + " file" + (had == 1 ? "" : "s") + " had pending edits)";
    (mode_ == Mode::MetaEdit ? meta_->status : status_line_) = msg;
    log_event(msg);
}

// Shared Y/N/ESC handler for the three confirmations. Returns true whenever
// it consumed the key -- including the "a prompt is up, swallow everything
// else" case -- and false only when there is no prompt at all, which is what
// lets handle_key() call it unconditionally before its mode dispatch.
bool App::handle_meta_prompt_key(int key) {
    if (meta_->prompt == MetaPrompt::None) return false;
    auto say = [this](const std::string& s) {
        (mode_ == Mode::MetaEdit ? meta_->status : status_line_) = s;
    };
    if (key == 'y' || key == 'Y') {
        MetaPrompt p = meta_->prompt;
        meta_->prompt = MetaPrompt::None;
        // Careful with meta_->prompt_paths: the fetch batch is built FROM it,
        // so it may only be cleared by the actions that don't read it -- an
        // eager clear here made meta_start_fetch() see an empty list and
        // return without fetching anything (silently: the confirmation had
        // already been dismissed).
        if (p == MetaPrompt::Save) {
            meta_->prompt_paths.clear();
            meta_apply_session();
        } else if (p == MetaPrompt::Discard) {
            meta_->prompt_paths.clear();
            meta_discard_session();
        } else if (p == MetaPrompt::ClearFetch) {
            // only the list: pending edits (fetched tags included) stay until CTRL+s or CTRL+SHIFT+x
            meta_->prompt_paths.clear();
            const size_t n = meta_->fetch_list.size();
            meta_->fetch_list.clear();
            meta_->fetch_ok_paths.clear();
            meta_->fetch_selected = 0;
            meta_persist();
            say("fetch list emptied (" + std::to_string(n) + " title" + (n == 1 ? "" : "s") + "); pending edits are kept");
        } else {
            meta_start_fetch(); // clears meta_->prompt_paths itself
        }
        return true;
    }
    if (key == 'n' || key == 'N' || key == 27) {
        meta_->prompt = MetaPrompt::None;
        meta_->prompt_paths.clear();
        say("cancelled");
        return true;
    }
    return true; // swallow everything else while a confirmation is up
}

} // namespace muisc
