// Meta data editor (the META overlay): key handling and the screen. Split out of app.cpp.
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

int App::meta_nav(int selected, int total, bool up) const {
    if (total <= 0) return 0;
    int to = selected;
    if (last_key_was_ctrl()) to = up ? 0 : total - 1;
    else if (last_key_was_shifted()) to = selected + (up ? -1 : 1) * std::max(1, meta_page_rows_);
    else to = selected + (up ? -1 : 1);
    return std::clamp(to, 0, total - 1);
}

// The rows of the folder view: per folder (in the order they first appear in the library pane) a header, then its files numbered from 1.
std::vector<App::MetaFolderRow> App::meta_folder_rows() const {
    std::vector<std::string> order;
    std::unordered_map<std::string, std::vector<int>> members;
    for (size_t i = 0; i < meta_lib_view_.size(); ++i) {
        const std::string dir = path_utf8(meta_lib_view_[i].path.parent_path());
        auto it = members.find(dir);
        if (it == members.end()) { order.push_back(dir); it = members.emplace(dir, std::vector<int>()).first; }
        it->second.push_back(static_cast<int>(i));
    }
    std::vector<MetaFolderRow> rows;
    for (const std::string& dir : order) {
        const std::vector<int>& m = members[dir];
        MetaFolderRow h;
        h.header = true;
        h.lib = m.front();
        h.folder = dir;
        h.name = path_utf8(path_from_utf8(dir).filename());
        if (h.name.empty()) h.name = dir;
        h.count = static_cast<int>(m.size());
        h.closed = meta_folder_closed_.count(dir) > 0;
        rows.push_back(h);
        if (h.closed) continue;
        int n = 0;
        for (int li : m) {
            MetaFolderRow f;
            f.lib = li;
            f.folder = dir;
            f.num = ++n;
            rows.push_back(f);
        }
    }
    return rows;
}

// The row of the cursor: the hovered folder header, else the row of the selected file (a file in a closed folder: its header).
int App::meta_folder_cursor(const std::vector<MetaFolderRow>& rows) const {
    if (!meta_folder_hover_.empty())
        for (size_t i = 0; i < rows.size(); ++i) if (rows[i].header && rows[i].folder == meta_folder_hover_) return static_cast<int>(i);
    for (size_t i = 0; i < rows.size(); ++i) if (!rows[i].header && rows[i].lib == meta_lib_selected_) return static_cast<int>(i);
    if (meta_lib_selected_ >= 0 && meta_lib_selected_ < static_cast<int>(meta_lib_view_.size())) {
        const std::string dir = path_utf8(meta_lib_view_[static_cast<size_t>(meta_lib_selected_)].path.parent_path());
        for (size_t i = 0; i < rows.size(); ++i) if (rows[i].header && rows[i].folder == dir) return static_cast<int>(i);
    }
    return 0;
}

// Up / Down in the library pane: file to file, or (folder view) row to row, headers included.
void App::meta_lib_move(bool up) {
    if (!meta_folder_view_) { meta_lib_selected_ = meta_nav(meta_lib_selected_, static_cast<int>(meta_lib_view_.size()), up); return; }
    const std::vector<MetaFolderRow> rows = meta_folder_rows();
    if (rows.empty()) return;
    const int to = meta_nav(meta_folder_cursor(rows), static_cast<int>(rows.size()), up);
    const MetaFolderRow& r = rows[static_cast<size_t>(to)];
    meta_folder_hover_ = r.header ? r.folder : std::string();
    meta_lib_selected_ = r.lib;   // (on a header: the folder's first file, so the panels beside show something)
}

void App::meta_add_folder_to_fetch(const std::string& folder) {
    meta_ensure_session_loaded();
    std::unordered_set<std::string> queued(meta_fetch_list_.begin(), meta_fetch_list_.end());
    size_t total = 0, added = 0;
    for (const auto& t : meta_lib_view_) {
        if (path_utf8(t.path.parent_path()) != folder) continue;
        ++total;
        std::string path = path_utf8(t.path);
        if (queued.insert(path).second) { meta_fetch_list_.push_back(std::move(path)); ++added; }
    }
    if (total == 0) { meta_status_ = "nothing to add -- no titles in this folder"; return; }
    if (added == 0) { meta_status_ = "all " + std::to_string(total) + " titles of the folder are on the fetch list already (" + std::to_string(meta_fetch_list_.size()) + " queued)"; return; }
    meta_persist();
    meta_status_ = "added " + std::to_string(added) + (added == 1 ? " title" : " titles") + " of the folder to the fetch list"
                 + (added < total ? " (" + std::to_string(total - added) + " already on it)" : std::string()) + " -- " + std::to_string(meta_fetch_list_.size()) + " queued";
}

void App::handle_meta_key(int key) {
    if (handle_meta_prompt_key(key)) return;
    if (handle_meta_choice_key(key)) return;
    if (handle_meta_cov_key(key)) return;
    if (handle_meta_sty_key(key)) return;

    if (key == 27) { // ESC
        // Inside a text box ESC first steps out of it: the field editor goes
        // back to the library (like ENTER), a search box that was typed into
        // hands Left/Right back to the tab switch. Only an ESC outside a box
        // leaves the editor -- which always just keeps the autosave backup.
        if (meta_tab_ == 0 && meta_focus_ == 2 && meta_field_editing_) {
            // Actively typing into a field: ESC first just puts the caret
            // away again (back to "highlighted, not editing" -- see
            // meta_field_editing_), same step ENTER takes. A second ESC,
            // below, is what leaves the field for the library.
            meta_field_editing_ = false;
            text_disengage();
            return;
        }
        if (meta_tab_ == 0 && meta_focus_ == 2) {
            meta_focus_ = 1;
            text_disengage();
            if (meta_resort_edited_ && !meta_lib_view_.empty()) meta_refresh_lib_view();
            return;
        }
        // One more step back from the library picker: the search bar,
        // rather than leaving the editor outright. A second ESC from there
        // (below, since the box isn't engaged any more) is what actually
        // exits now.
        // With a missing-tag filter (x / T / A / L / Y) on, the first ESC only takes the filter off ...
        if (meta_tab_ == 0 && meta_focus_ == 1 && meta_filter_ != 0) { meta_toggle_filter(meta_filter_); return; }
        // ... the next ESC takes the sorts off ('r' edited first, 'n' added first), one step per ESC like the filter.
        if (meta_tab_ == 0 && meta_focus_ == 1 && (meta_resort_edited_ || meta_sort_added_)) {
            meta_resort_edited_ = false;
            meta_sort_added_ = false;
            meta_refresh_lib_view();
            meta_status_ = "library order: alphabetical again";
            return;
        }
        if (meta_tab_ == 0 && meta_focus_ == 1) { meta_focus_ = 0; return; }
        if (meta_tab_ == 0 && meta_focus_ == 0 && !meta_query_.empty()) {   // the first ESC in the search field clears the query ...
            meta_query_.clear();
            edit_caret_ = edit_anchor_ = 0;
            text_disengage();
            meta_lib_selected_ = 0;
            meta_refresh_lib_view();
            return;
        }
        if (meta_tab_ == 0 && meta_focus_ == 0 && text_engaged("meta-search")) { text_disengage(); return; }   // ... the next one closes the editor
        mode_ = Mode::Browse;
        return;
    }
    if (key == kKeyCtrlS) {
        if (meta_session_.empty()) { meta_status_ = "nothing to save -- no pending edits"; return; }
        meta_prompt_ = MetaPrompt::Save;
        return;
    }
    if (key == kKeyCtrlShiftX) {
        if (meta_session_.empty() && meta_fetch_list_.empty()) { meta_status_ = "nothing to discard"; return; }
        meta_prompt_ = MetaPrompt::Discard;
        return;
    }
    // SHIFT+b (HKeyFetchTags) for the hovered title -- raw 'B' with no arrow behind it. Only while the focus has no caret of its own to
    // type into: the library picker (focus 1) and the whole of tab 1's fetch list. In the search box and the field editor a capital B
    // must stay typable.
    if (key_is("HKeyFetchTags", key) && (key > 255 || meta_tab_ == 1 || meta_focus_ == 1)) {
        std::string path = meta_hovering_path();
        if (path.empty()) meta_status_ = "no title selected";
        else meta_prompt_single_fetch(path, false);
        return;
    }
    // SHIFT+c on the EDIT tab (library list): jump to the song that plays now, whatever was typed or filtered (all removed).
    if (key == 'C' && !last_key_was_arrow() && meta_tab_ == 0 && meta_focus_ == 1) { meta_jump_to_current(); return; }
    // SHIFT+c on the Fetch List tab: stop the running fetch.
    if (key == 'C' && !last_key_was_arrow() && meta_tab_ == 1) { meta_cancel_fetch(); return; }

    // SHIFT+n: the same for ALL tags (overwrites the existing ones); same focus rule as SHIFT+b.
    if (key == 'N' && !last_key_was_arrow() && (meta_tab_ == 1 || meta_focus_ == 1)) {
        std::string path = meta_hovering_path();
        if (path.empty()) meta_status_ = "no title selected";
        else meta_prompt_single_fetch(path, true);
        return;
    }

    // SHIFT+r: rescan the library (same rule as SHIFT+b: not while a text field has the caret).
    if (key_is("HKeyRescan", key) && (key > 255 || meta_tab_ == 1 || meta_focus_ == 1)) { rescan_now(); return; }

    // Arrows collapse to 'A'..'D' app-wide; last_key_was_arrow() is what
    // tells a real arrow from a typed capital. Unlike the playlist editor
    // (where the name field only ever sees casual typing) these fields hold
    // real words, so capitals MUST stay typable here -- which is also why
    // the tab switch below only fires for the arrow itself. It is skipped
    // entirely while a field is being edited: Left/Right are the caret keys
    // there (see the field editor at the bottom), and bouncing to the fetch
    // list on every arrow press is exactly what made marking text
    // impossible.
    bool arrow = last_key_was_arrow();

    // The search box follows the playlist editor's rule: Left/Right switch
    // tabs until something is typed into it (text_engage()), then they move
    // the caret until ESC / TAB / ENTER. In the field editor they are always
    // the caret keys -- it is only ever entered on purpose (ENTER / TAB).
    // Alt+Left/Right switch tabs from everywhere.
    // The tab switch itself stays blocked for the whole time focus is in the
    // field editor, not just while a field is actively being typed into --
    // a merely-highlighted field still uses Left/Right to move the caret
    // once ENTER activates it, so bouncing to the other tab on a stray
    // arrow there would be surprising either way.
    const bool block_tab_switch = (meta_tab_ == 0 && (meta_focus_ == 2 || (meta_focus_ == 0 && text_engaged("meta-search"))))
                               || (meta_tab_ == 1 && meta_fetch_focus_ == 1);   // the option line uses left/right itself
    if (key == kKeyAltLeft || key == kKeyAltRight || (arrow && (key == 'C' || key == 'D') && !block_tab_switch)) { // the only 2 tabs
        meta_tab_ = (meta_tab_ + 1) % 2;
        meta_focus_ = 0;
        meta_fetch_focus_ = 0;
        meta_field_editing_ = false;
        text_disengage();
        meta_refresh_lib_view();
        return;
    }
    if (key == 9) { // Tab: search field -> library picker -> field editor -> ...
        if (meta_tab_ == 0) meta_focus_ = (meta_focus_ + 1) % 3;
        else meta_fetch_focus_ = 1 - meta_fetch_focus_;   // FETCH LIST: the list <-> the option line in the header
        meta_field_editing_ = false;
        text_disengage();
        return;
    }

    if (meta_tab_ == 1) { // --- fetch list ---
        int total = static_cast<int>(meta_fetch_list_.size());
        if (arrow && (key == 'A' || key == 'B')) { meta_fetch_selected_ = meta_nav(meta_fetch_selected_, total, key == 'A'); return; }
        if (meta_fetch_focus_ == 1) {   // the option line: left / right move the cursor, SPACE ticks the box under it
            if (arrow && key == 'C') { meta_opt_cursor_ = (meta_opt_cursor_ + 1) % kMetaFetchOptCount; return; }
            if (arrow && key == 'D') { meta_opt_cursor_ = (meta_opt_cursor_ + kMetaFetchOptCount - 1) % kMetaFetchOptCount; return; }
            if (key == ' ') { meta_toggle_fetch_opt(meta_opt_cursor_); return; }
        }
        if (key == '\r' || key == '\n') { meta_prompt_list_fetch(); return; } // "fetch metadata" over the whole list
        if (key == 'P' && !arrow) { meta_play_hovering(); return; } // SHIFT+p: start the hovered track
        // SHIFT+d / SHIFT+DEL: empty the whole list (asks first); DEL / d: only the hovered title
        if ((key == kKeyDelete && last_key_was_shifted()) || (key == 'D' && !arrow)) {
            if (meta_fetch_list_.empty()) { meta_status_ = "the fetch list is empty"; return; }
            if (meta_fetch_running_.load()) { meta_status_ = "a fetch is running: stop it first with SHIFT+c"; return; }
            meta_prompt_ = MetaPrompt::ClearFetch;
            return;
        }
        if (key == kKeyDelete || key == 127 || key == 'd') { meta_remove_hovering(); return; }
        return;
    }

    if (meta_focus_ == 0) { // --- search field ---
        // Up/Down still walk the library while the box has focus (same
        // convention as the main UI's "/" search); Left/Right are the caret
        // now, which is what makes marking -- and therefore copy/paste --
        // possible in here at all.
        if (arrow && (key == 'A' || key == 'B')) { meta_lib_move(key == 'A'); return; }
        if (key == '\r' || key == '\n') {
            // The hovered track (Up/Down already moved meta_lib_selected_
            // while this box had focus) goes straight to its fields --
            // no need to land on the library picker first just to press
            // ENTER a second time.
            text_disengage();
            if (meta_lib_view_.empty()) { meta_status_ = "no track selected"; meta_focus_ = 1; return; }
            meta_focus_ = 2;
            meta_field_editing_ = false; // lands highlighted, not yet armed for typing
            return;
        }
        edit_focus("meta-search", meta_query_);
        text_engage("meta-search");
        if (edit_text_key(meta_query_, edit_caret_, edit_anchor_, key, 120, &meta_status_))
            meta_refresh_lib_view();
        return;
    }

    if (meta_focus_ == 1) { // --- library picker ---
        int total = static_cast<int>(meta_lib_view_.size());
        if (arrow && (key == 'A' || key == 'B')) { meta_lib_move(key == 'A'); return; }
        if (meta_folder_view_ && !meta_folder_hover_.empty() && !arrow) {   // a folder header is hovered
            if (key == '\r' || key == '\n') {   // open / close the folder
                if (!meta_folder_closed_.erase(meta_folder_hover_)) meta_folder_closed_.insert(meta_folder_hover_);
                return;
            }
            if (key == 'a') { meta_add_folder_to_fetch(meta_folder_hover_); return; }
            if (key == kKeyDelete || key == 127 || key == 'd' || key == 'P') { meta_status_ = "a folder is hovered -- move onto a title first"; return; }
        }
        if (key == 'f' && !arrow) {   // folder view on / off
            meta_folder_view_ = !meta_folder_view_;
            meta_folder_hover_.clear();
            meta_lib_selected_ = 0;   // the view starts at the top again
            meta_status_ = meta_folder_view_ ? "folder view: on -- [a] on a folder queues all its titles, [ENTER] opens / closes it" : "folder view: off";
            return;
        }
        if (key == '\r' || key == '\n') {
            if (total == 0) { meta_status_ = "no track selected"; return; }
            meta_focus_ = 2; // land on the hovered title's fields, highlighted
            meta_field_editing_ = false; // not yet armed for typing -- ENTER arms it
            return;
        }
        if (key == 'a') { meta_add_hovering_to_fetch(); return; } // queue it for AcoustID, like the main queue's 'a'
        if (key == 'F' && !arrow) { meta_add_view_to_fetch(); return; } // SHIFT+f: the whole (filtered) pane
        if (key == 'V' && !arrow) {   // SHIFT+v: the pane lists audio / video + audio / video
            meta_media_ = (meta_media_ < 0 ? (settings_.video_mode ? 1 : 0) : meta_media_) + 1;
            if (meta_media_ > 2) meta_media_ = 0;
            meta_lib_selected_ = 0;
            meta_refresh_lib_view();
            meta_status_ = std::string("library pane: ") + media_view_name(meta_media_) + " -- " + std::to_string(meta_lib_view_.size()) + " files";
            return;
        }
        if (key == 'r') { meta_toggle_resort(); return; } // toggle: edited files on top vs. alphabetical order
        if (key == 'n' && !arrow) { meta_toggle_sort_added(); return; } // toggle: recently added files first
        // Missing-tag filters. 'T'/'Y' have no other meaning here; 'A' is
        // only the Up ARROW together with last_key_was_arrow(), which the
        // two navigation keys above already claimed -- so a plain Shift+A
        // typed here reaches this line.
        if (key == 'x') { meta_toggle_filter(1); return; } // only files with no metadata at all
        if (key == 'T') { meta_toggle_filter(2); return; } // SHIFT+t: missing title
        if (key == 'A') { meta_toggle_filter(3); return; } // SHIFT+a: missing artist
        if (key == 'Y') { meta_toggle_filter(4); return; } // SHIFT+y: missing year
        if (key == 'L') { meta_toggle_filter(5); return; } // SHIFT+l: missing album
        if (key == 'P') { meta_play_hovering(); return; } // SHIFT+p: start the hovered track
        if (key == 't') { meta_toggle_cover_res(); return; } // the cover picture: ASCII / full resolution
        if (key == kKeyDelete || key == 127 || key == 'd') { meta_remove_hovering(); return; }
        return; // every other printable key would be search input, and search lives in focus 0
    }

    // --- field editor: highlight first, ENTER arms the caret to type ---
    // Same two-stage model the PATHS tab uses for its folder fields
    // (mode_ == Mode::ColorEdit there; meta_field_editing_ here, since the
    // meta editor isn't a Settings mode): a hovered tag is merely
    // highlighted -- Up/Down move between tags -- until ENTER shows the
    // caret, at which point Up/Down stop navigating (so they don't get
    // typed as literal letters below) and a second ENTER puts the caret
    // away again without leaving the field.
    {
        std::string path = meta_hovering_path();
        if (path.empty()) { meta_focus_ = 1; meta_field_editing_ = false; return; }
        if (meta_field_ >= kMetaFieldCount) {   // the "Cover Options" button below the fields
            meta_field_editing_ = false;
            if (arrow && (key == 'A' || key == 'B')) { meta_field_ = meta_nav(meta_field_, kMetaFieldCount + 3, key == 'A'); return; }
            if (arrow && (key == 'C' || key == 'D')) { meta_field_ = kMetaFieldCount + ((meta_field_ - kMetaFieldCount) + (key == 'C' ? 1 : 2)) % 3; return; }   // the three buttons side by side   // the two buttons side by side
            if (key == '\r' || key == '\n') { if (meta_field_ == kMetaFieldCount) meta_cov_open(); else if (meta_field_ == kMetaFieldCount + 1) meta_sty_open(); else video_link_open(path_from_utf8(path), Mode::MetaEdit); }
            return;
        }

        if (key == 't' && !arrow && !meta_field_editing_) { meta_toggle_cover_res(); return; }   // (a field only takes letters once ENTER armed it)

        // The caret belongs to one specific (file, field) pair. If either
        // changed since the last keypress -- a field move, another row, the
        // 'r' resort reordering the list -- the offsets now point at a
        // different string, so retarget them to the end of this one instead
        // of letting them be clamped against the wrong text.
        const MetaEditEntry* e = meta_entry(path);
        std::string cur = meta_display_value(path, meta_field_, e);
        const std::string owner = path + "#" + std::to_string(meta_field_);
        if (edit_owner_ != owner) {
            edit_owner_ = owner;
            edit_caret_ = edit_anchor_ = cur.size();
        }
        le_clamp(cur, edit_caret_, edit_anchor_);
        // meta_set_field() marks the file as edited and rewrites the
        // autosave unconditionally, so it is only ever reached when the
        // bytes really changed -- moving the caret must not make a file
        // count as edited.
        const std::string orig = cur;
        auto commit = [&]() { if (cur != orig) meta_set_field(path, meta_field_, cur); };
        const size_t limit = (meta_field_ == static_cast<int>(MetaField::FileName)) ? 200 : 160;

        if (!meta_field_editing_) {
            // Highlighted, not yet armed: Up/Down move between tags, ENTER
            // arms this one (caret starts at the end, same as a fresh
            // PATHS-row edit). Everything else is a no-op -- typing on a
            // merely-highlighted field would be surprising.
            if (arrow && (key == 'A' || key == 'B')) { meta_field_ = meta_nav(meta_field_, kMetaFieldCount + 3, key == 'A'); return; }
            if (key == '\r' || key == '\n') {
                if (e && e->needs_choice(meta_field_)) { meta_choice_open(); return; }   // several candidates: pick one first
                meta_field_editing_ = true;
                edit_owner_ = owner;
                edit_caret_ = edit_anchor_ = cur.size();
            }
            return;
        }

        // --- armed: caret, selection, clipboard, typing -------------------
        // Up/Down only navigate fields while merely highlighted (above);
        // here they must be dropped rather than falling through to
        // is_text_key() below, where a collapsed arrow would otherwise be
        // typed as a literal 'A'/'B' -- the same guard Mode::ColorEdit uses.
        if (arrow && (key == 'A' || key == 'B')) return;
        if (key == '\r' || key == '\n') {
            meta_field_editing_ = false; // done typing -- back to highlighted, same tag
            // With the 'r' resort on, the file just edited has (re)joined the
            // edited block: move it up now, while the cursor is still on it --
            // meta_refresh_lib_view() follows the row by path.
            if (meta_resort_edited_ && !meta_lib_view_.empty()) meta_refresh_lib_view();
            return;
        }
        // --- caret, selection, clipboard ---------------------------------
        // Shift+arrows mark, Ctrl+C/X/V copy/cut/paste, Home/End jump. They
        // arrive as their own sentinel values (terminal_ui.h) because the
        // bare ones are already taken here: a plain arrow press collapses to
        // the letter it would otherwise type, and a typed 'c' has to stay a
        // typed 'c' inside a word.
        {   // CTRL+z / CTRL+y: undo (the history follows the field that is being edited)
            bool undone = false;
            if (le_undo_key(&edit_owner_, cur, edit_caret_, edit_anchor_, key, &meta_status_, &undone)) { if (undone) commit(); return; }
        }
        LeUndoScope undo_scope(&edit_owner_, cur, edit_caret_, key);
        if (key == kKeyHome) { edit_caret_ = edit_anchor_ = 0; return; }
        if (key == kKeyEnd) { edit_caret_ = edit_anchor_ = cur.size(); return; }
        if (key == kKeyShiftLeft) { le_move(cur, edit_caret_, edit_anchor_, -1, true); return; }
        if (key == kKeyShiftRight) { le_move(cur, edit_caret_, edit_anchor_, +1, true); return; }
        if (key == kKeyCtrlC) {
            size_t a = 0, b = 0;
            le_range(edit_caret_, edit_anchor_, a, b);
            std::string t = (a == b) ? cur : cur.substr(a, b - a);
            clipboard_set(t);
            meta_status_ = t.empty() ? "nothing selected" : "COPIED";
            return;
        }
        if (key == kKeyCtrlX) {
            size_t a = 0, b = 0;
            le_range(edit_caret_, edit_anchor_, a, b);
            if (a == b) return; // never empty the field by accident
            clipboard_set(cur.substr(a, b - a));
            le_erase_selection(cur, edit_caret_, edit_anchor_);
            commit();
            meta_status_ = "CUT";
            return;
        }
        if (key == kKeyCtrlV) {
            le_paste(cur, edit_caret_, edit_anchor_, clipboard_get(), limit);
            commit();
            meta_status_ = "PASTED";
            return;
        }
        if (key == kKeyDelete) { le_delete_forward(cur, edit_caret_, edit_anchor_); commit(); return; }
        if (key == 127 || key == 8) { le_backspace(cur, edit_caret_, edit_anchor_); commit(); return; }
        if (arrow && (key == 'C' || key == 'D')) { // plain Left/Right: move the caret, leave the tab alone
            le_move(cur, edit_caret_, edit_anchor_, key == 'D' ? -1 : +1, false);
            return;
        }
        if (is_text_key(key)) {
            le_insert(cur, edit_caret_, edit_anchor_, static_cast<char>(key), limit);
            commit();
        }
        return;
    }
}

std::vector<std::string> App::build_meta_library_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    // The pane title carries the active missing-tag filter, so a list
    // narrowed by 'x' or Shift+T/A/Y is never mistaken for the whole
    // library (the status line repeats it with a count).
    std::string lib_label = "FILE LIBRARY";
    {   // what is active: the sorts ('r' edited first, 'n' added first) and the filter, joined with " + "
        std::string active;
        auto add = [&](const std::string& part) { active += (active.empty() ? "" : " + ") + part; };
        if (meta_resort_edited_) add("EDITED FIRST");
        if (meta_sort_added_) add("ADDED FIRST");
        if (meta_filter_ != 0) add(meta_filter_label());
        if (meta_folder_view_) add("FOLDERS");
        // the filters are cut with "..." so that title, filters and the focus indicator always fit the border (" ◀" is 2 columns)
        const int room = total_width - 5 - display_width(lib_label) - 2;
        if (!active.empty() && room >= 4) lib_label += truncate_str(" [" + active + "]", room);
    }
    if (meta_focus_ == 1) lib_label += focus_indicator(settings_, border_ansi);
    out.push_back(box_top(lib_label, total_width, border_ansi));

    int total = static_cast<int>(meta_lib_view_.size());
    if (total == 0) {
        int mid = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid) {
                std::string plain = (meta_filter_ != 0) ? "NO TRACKS MATCH THE FILTER"
                                    : meta_query_.empty() ? "NO TRACKS FOUND"
                                    : "NO MATCHING TRACK";
                std::string text = apply_font_map(plain, settings_.font_map);
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

    if (meta_folder_view_) {   // the same list grouped by folder: header rows, files numbered per folder
        const std::vector<MetaFolderRow> rows = meta_folder_rows();
        const int rtotal = static_cast<int>(rows.size());
        const int cur = meta_folder_cursor(rows);
        const int rscroll = std::clamp(cur - height / 2, 0, std::max(0, rtotal - height));
        const int fidx_w = 3;
        for (int row = 0; row < height; ++row) {
            const int ri = rscroll + row;
            std::string content;
            bool edited = false, needs_choice = false;
            const bool is_sel = ri == cur && ri < rtotal;
            const bool is_header = ri < rtotal && rows[static_cast<size_t>(ri)].header;
            if (ri < rtotal) {
                const MetaFolderRow& r = rows[static_cast<size_t>(ri)];
                if (r.header) {
                    content = std::string(r.closed ? "\xe2\x96\xb8 " : "\xe2\x96\xbe ") + apply_font_map(r.name, settings_.font_map) + "  (" + std::to_string(r.count) + ")";
                } else {
                    const auto& t = meta_lib_view_[static_cast<size_t>(r.lib)];
                    const int title_w = std::max(5, inner - 2 - fidx_w - 2);
                    content = "  " + pad_right(apply_font_map(std::to_string(r.num), settings_.font_map), fidx_w) + settings_.list_separator + " "
                            + pad_right(truncate_str(apply_font_map(list_row_title(t.path, t.title), settings_.font_map), title_w), title_w);
                    const MetaEditEntry* e = meta_entry(path_utf8(t.path));
                    edited = e && e->any_edited();
                    needs_choice = e && e->any_needs_choice();
                }
            }
            const std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string base = is_sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color)
                                      : (ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color));
            if (is_header && !is_sel) base += header_sgr(settings_);   // folder headers stand out
            if (edited) out.push_back(bar + " " + base + (needs_choice ? legend_sgr(settings_) : header_sgr(settings_)) + padded + "\x1b[0m" + base + "\x1b[0m " + bar);
            else out.push_back(bar + " " + base + padded + "\x1b[0m " + bar);
        }
        std::string footer;
        const int remaining = rtotal - (rscroll + height);
        if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
        if (!meta_session_.empty()) {
            const std::string tag = "[ " + std::to_string(meta_session_.size()) + " edited ]";
            footer += footer.empty() ? tag : "  " + tag;
        }
        out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
        return out;
    }

    int scroll = std::clamp(meta_lib_selected_ - height / 2, 0, std::max(0, total - height));
    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        bool edited = false;
        bool needs_choice = false;
        if (idx < total) {
            const auto& t = meta_lib_view_[idx];
            int title_w = std::max(5, inner - idx_w - 2);
            std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
            std::string t_title = apply_font_map(list_row_title(t.path, t.title), settings_.font_map);
            content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                    + pad_right(truncate_str(t_title, title_w), title_w);
            // Files carrying pending edits are painted in the header colour,
            // the same bold highlight an edited field gets in the FIELDS
            // panel -- so the list answers "which files is that panel talking
            // about?" without having to walk over them one by one.
            const MetaEditEntry* e = meta_entry(path_utf8(t.path));
            edited = e && e->any_edited();
            needs_choice = e && e->any_needs_choice();   // several fetched candidates wait for a decision
        }
        // The selected row stays highlighted in EVERY focus, not just while
        // the pane itself owns focus: arrows scroll the library from the
        // search field too (see handle_meta_key(), focus 0), and without the
        // highlight there was nothing on screen saying which row those arrows
        // were moving. Matches build_list_panel() in Browse, which never gates
        // its cursor on focus either -- the LIBRARY box's "◀" stays the focus
        // marker, so there is still exactly one thing naming the focused pane.
        bool sel = (idx == meta_lib_selected_);
        std::string padded = pad_right(truncate_str(content, inner), inner);
        std::string base;
        if (sel) {
            base = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
        } else {
            base = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
        }
        // Colour AFTER padding: display_width() is UTF-8-aware but not
        // ANSI-aware, so a coloured row would otherwise measure wrong.
        if (edited) {
            out.push_back(bar + " " + base + (needs_choice ? legend_sgr(settings_) : header_sgr(settings_)) + padded
                          + "\x1b[0m" + base + "\x1b[0m " + bar);
        } else {
            out.push_back(bar + " " + base + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    // One number for the highlight above: how many files in the session have
    // pending edits (meta_session_ only ever holds those).
    if (!meta_session_.empty()) {
        std::string tag = "[ " + std::to_string(meta_session_.size()) + " edited ]";
        footer += footer.empty() ? tag : "  " + tag;
    }
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

// The five editable fields. This is where the header-colour highlight and
// the block cursor live, and both are built the same way build_list_panel()
// builds a row: plain segments are measured first, then the already-sized
// pieces get wrapped in colour -- display_width() is UTF-8-aware but NOT
// ANSI-aware, so colouring before padding would miscount the row.
std::vector<std::string> App::build_meta_fields_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    std::string path = meta_hovering_path();
    std::string name = path.empty() ? std::string() : path_utf8(path_from_utf8(path).filename());
    std::string sel_pos;
    if (!meta_lib_view_.empty()) {
        sel_pos = "  " + std::to_string(meta_lib_selected_ + 1) + "/" + std::to_string(meta_lib_view_.size());
    }
    std::string label = "EDIT" + sel_pos + (name.empty() ? std::string() : " " + truncate_str(name, 40))
                      + (meta_focus_ == 2 ? focus_indicator(settings_, border_ansi) : std::string());
    out.push_back(box_top(label, total_width, border_ansi));

    if (path.empty()) {
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == height / 2) {
                std::string text = apply_font_map("NO TRACK SELECTED", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + padded + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    const MetaEditEntry* entry = meta_entry(path);
    std::vector<std::string> cover_pane_cache;
    const int label_w = 8;
    for (int row = 0; row < height; ++row) {
        if (row >= kMetaFieldCount) { // below the fields: the "Cover Options" button, then the cover of the track centred in the free space
            std::string padded;
            const int btn_row = kMetaFieldCount + 1;
            const int pic_first = btn_row + 2;
            const int pic_rows = height - pic_first;
            if (row == btn_row) {
                const bool hov = (meta_focus_ == 2) && meta_field_ >= kMetaFieldCount;
                std::string b1 = "[ Cover Options ]", b2 = "[ Style Option ]", b3 = "[ Video Option ]";
                if (display_width(b1) + display_width(b2) + display_width(b3) + 4 > inner) { b1 = "[ Cover ]"; b2 = "[ Style ]"; b3 = "[ Video ]"; }   // a narrow pane: short labels, still one line
                const int total = display_width(b1) + 2 + display_width(b2) + 2 + display_width(b3);
                const int left = std::max(0, (inner - total) / 2);
                const std::string lit = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color), plain = header_sgr(settings_);
                const std::string base = ansi_for(settings_.list_color, false);
                padded = std::string(static_cast<size_t>(left), ' ') + (hov && meta_field_ == kMetaFieldCount ? lit : plain) + b1 + "\x1b[0m" + base + "  "
                       + (hov && meta_field_ == kMetaFieldCount + 1 ? lit : plain) + b2 + "\x1b[0m" + base + "  "
                       + (hov && meta_field_ == kMetaFieldCount + 2 ? lit : plain) + b3 + "\x1b[0m" + base;
                padded += std::string(static_cast<size_t>(std::max(0, inner - left - total)), ' ');
            } else if (row >= pic_first && pic_rows >= 3) {
                if (row == pic_first) cover_pane_cache = meta_cover_pane(inner, pic_rows);
                const size_t ri = static_cast<size_t>(row - pic_first);
                padded = ri < cover_pane_cache.size() ? cover_pane_cache[ri] : pad_right(std::string(), inner);
            } else {
                padded = pad_right(std::string(), inner);
            }
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + padded + "\x1b[0m " + bar);
            continue;
        }
        int i = row;
        std::string lab = pad_right(meta_field_label(i), label_w);
        std::string val_raw = meta_hover_values_[i];
        if (val_raw.empty()) {
            val_raw = (i == 0) ? std::string("-")
                    : (meta_hover_resolved_ ? "(untagged)" : "(reading ...)");
        }
        // Two states share the "hovered" row, matching the PATHS tab's
        // highlighted-vs-activated folder field: hovered alone just
        // highlights the tag (no caret, Up/Down move between tags);
        // meta_field_editing_ additionally arms the caret and swaps in the
        // CHANGE colours (change_sgr()) -- the same ones an activated PATHS
        // row uses -- so it's visually obvious typing is live.
        bool hovered = (meta_focus_ == 2) && (i == meta_field_);
        bool editing = hovered && meta_field_editing_;
        bool edited = entry && entry->edited[i];
        const int val_w = std::max(4, inner - label_w);

        std::string base = hovered ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color)
                               : (ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color));

        std::string val;
        int used = 0;
        if (editing) {
            // The field being typed into is painted by paint_edit_field():
            // the window scrolls so the caret stays inside the panel, the
            // marked range is drawn in reverse video, and the caret itself
            // is the block glyph this panel uses for a cursor (it renders
            // whole frames, so there is no real terminal cursor to move --
            // which is also why an arrow press in the caret row is free to switch
            // tabs while the text still has nowhere to go).
            // The caret only belongs to this row if the offsets were last
            // set against exactly this (file, field) pair -- see the field
            // editor in handle_meta_key().
            const std::string owner = path + "#" + std::to_string(i);
            size_t c = val_raw.size();
            size_t a = c;
            if (edit_owner_ == owner) { c = edit_caret_; a = edit_anchor_; }
            const std::string restore = change_sgr(settings_) + (edited ? header_sgr(settings_) : std::string());
            EditPaint p = paint_edit_field(val_raw, c, a, val_w, restore, /*block=*/true);
            // paint_edit_field() only re-emits `restore` after a selection
            // run, so the CHANGE colours have to be switched on explicitly
            // here too -- same as the PATHS/Colors tabs' own activated-field
            // rendering (change_sgr() prefixed before the painted value).
            val = restore + p.s;
            used = display_width(lab) + p.cols; // p.cols already ignores the ANSI codes
        } else {
            val = truncate_str(val_raw, val_w);
            used = display_width(lab) + display_width(val);
        }
        std::string fill(std::max(0, inner - used), ' ');

        std::string row_ansi = base + lab;
        // Header colour: edited by hand or fetched with a single candidate. Legend colour: several candidates --
        // a choice is waiting (ENTER opens it).
        if (edited) row_ansi += (entry && entry->needs_choice(i)) ? legend_sgr(settings_) : header_sgr(settings_);
        row_ansi += val;
        if (edited) row_ansi += "\x1b[0m" + base; // back to the row's own colours for the padding
        row_ansi += fill + "\x1b[0m";
        out.push_back(bar + " " + row_ansi + " " + bar);
    }

    int n_edited = 0, n_choice = 0;
    if (entry) {
        for (bool b : entry->edited) if (b) ++n_edited;
        for (int i = 0; i < kMetaFieldCount; ++i) if (entry->needs_choice(i)) ++n_choice;
    }
    std::string footer = n_edited ? ("( " + std::to_string(n_edited) + " edited, pending )") : "";
    if (n_choice) footer += std::string(footer.empty() ? "( " : " ( ") + std::to_string(n_choice) + " to choose )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_meta_fetch_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    out.push_back(box_top("FETCH LIST (" + std::to_string(meta_fetch_list_.size()) + ")" + focus_indicator(settings_, border_ansi),
                          total_width, border_ansi));

    int total = static_cast<int>(meta_fetch_list_.size());
    if (total == 0) {
        int mid = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid) {
                std::string text = apply_font_map("NOTHING QUEUED -- PRESS 'a' ON A TRACK IN THE EDIT TAB",
                                                  settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + padded + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    int scroll = std::clamp(meta_fetch_selected_ - height / 2, 0, std::max(0, total - height));
    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const std::string& p = meta_fetch_list_[idx];
            int title_w = std::max(5, inner - idx_w - 2);
            const MetaEditEntry* e = meta_entry(p);
            std::string base = list_row_title(path_from_utf8(p), path_utf8(path_from_utf8(p).stem()));
            std::string shown = (e && e->any_edited()) ? (base + " [edited]") : base;
            std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
            std::string t_title = apply_font_map(shown, settings_.font_map);
            content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                    + pad_right(truncate_str(t_title, title_w), title_w);
        }
        bool sel = (idx == meta_fetch_selected_);
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

// Full-screen meta editor, assembled exactly like build_playlist_screen():
// header box, tab strip, optional search row, boxed panels, then a footer
// that is ALWAYS three lines (prompt padded to two + status, or legend's
// two rows + status) so switching states never makes the layout jump.
void App::build_meta_screen(std::ostringstream& frame, int W, int target_height) const {
    if (W < 60) W = 60;
    std::string border = ansi_for(settings_.border_color, false);
    std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string HI = "\x1b[7m", R = "\x1b[0m";

    int fixed_rows = 3; // the two strip lines + bottom border
    {   // title on the border line + the settings-style tab strip (same as the playlist menu). On the EDIT tab the search
        // field sits on the strip's second line, left of the tab indicators (pushed right so 25 characters always fit).
        const int search_max = 25;
        const std::string prefix = " Search: ";
        const int field_w = search_max + 1;                                    // 25 characters + the caret block
        const int inner_w = meta_tab_ == 1 ? 82   // the FETCH LIST option line (" Fetch: " + six check boxes + focus mark)
                          : static_cast<int>(prefix.size()) + field_w + 2 + 1; // + room for the focus mark
        std::string line2;
        int line2_cols = 0;
        if (meta_tab_ == 0) {
            std::string field;
            int field_cols = 0;
            if (meta_focus_ == 0) { // focus is in the box: caret + block cursor
                EditPaint p = paint_edit_field(meta_query_, edit_caret_, edit_anchor_, field_w,
                                               meta_query_.empty() ? search_idle_sgr(settings_) : search_sgr(settings_), true);
                field = search_run(settings_, p.s, !meta_query_.empty());
                field_cols = p.cols;
            } else { // focus elsewhere: plain text, with the placeholder hint
                const std::string plain = truncate_str(meta_query_.empty() ? std::string("(type to filter)") : meta_query_, field_w);
                field = search_run(settings_, plain, !meta_query_.empty());
                field_cols = static_cast<int>(display_width(plain));
            }
            std::string focus_mark;
            int focus_w = 0;
            if (meta_focus_ == 0) { focus_mark = " " + focus_sgr(settings_) + "\xe2\x97\x80" + "\x1b[0m"; focus_w = 2; }
            line2 = search_idle_sgr(settings_) + prefix + field + "\x1b[0m" + focus_mark;
            line2_cols = static_cast<int>(prefix.size()) + field_cols + focus_w;
        }
        if (meta_tab_ == 1) {
            // FETCH LIST: the fetch options as check boxes, where the EDIT tab has its search. Only the ticked tags are
            // fetched, for every title of the list (ENTER). Missing only / all untick everything else and vice versa.
            const std::string prefix = " Fetch: ";
            std::string opts;
            int opts_cols = 0;
            for (int m = 0; m < kMetaFetchOptCount; ++m) {
                const bool on = (meta_fetch_opts_ & (1u << m)) != 0;
                const std::string word = std::string(on ? "[x] " : "[ ] ") + meta_fetch_opt_label(m);
                const bool cursor = (meta_fetch_focus_ == 1 && m == meta_opt_cursor_);
                std::string shown = word;
                if (cursor) shown = "\x1b[7m" + word + "\x1b[0m" + search_idle_sgr(settings_);
                else if (on) shown = header_sgr(settings_) + word + "\x1b[0m" + search_idle_sgr(settings_);
                opts += shown;
                opts_cols += static_cast<int>(word.size());
                if (m + 1 < kMetaFetchOptCount) { opts += "  "; opts_cols += 2; }
            }
            std::string focus_mark;
            int focus_w = 0;
            if (meta_fetch_focus_ == 1) { focus_mark = " " + focus_sgr(settings_) + "\xe2\x97\x80" + "\x1b[0m"; focus_w = 2; }
            line2 = search_idle_sgr(settings_) + prefix + opts + "\x1b[0m" + focus_mark;
            line2_cols = static_cast<int>(prefix.size()) + opts_cols + focus_w;
        }
        const auto strip = menu_tab_strip(W, "META EDITOR", {"EDIT", "FETCH LIST"}, meta_tab_, border, border,
                                          settings_.box_upper_left, settings_.box_upper_right, settings_.box_vertical,
                                          inner_w, line2, line2_cols,
                                          ansi_for(settings_.tab_current_color, false), ansi_for(settings_.tab_other_color, false));
        frame << strip.first << "\n" << strip.second << "\n";
    }
    frame << box_bottom(W, "", border_bottom) << "\n";

    // Sized against BOTH bounds that matter: term_rows_ - 1 is what
    // clamp_output_rows() will actually keep (a footer line chopped off
    // every frame is a status message or half a confirmation prompt gone),
    // while player_view_height() is what the other overlays are sized to --
    // except on a short terminal, where the Browse view's metadata/progress/
    // search panels don't shrink and that value is several rows taller than
    // the screen. total = fixed_rows + (panel_h + 2) + 3 must fit the
    // smaller of the two: 2 for the panel's own border rows, 5 for the
    // footer (four legend rows + status -- see where it's written below).
    // The legend first: its height (4 rows, more when Move / Filter use a second row) decides how much is left for the panels.
    // Left/Right and ESC say what they do RIGHT NOW: inside a text box
    // that is in use they move the caret / leave the box, otherwise they
    // switch the tab / close the editor (see handle_meta_key()).
    const bool in_text = meta_tab_ == 0 && ((meta_focus_ == 2 && meta_field_editing_) || (meta_focus_ == 0 && text_engaged("meta-search")));
    const std::string lr = in_text ? "[\u2190\u2192] Cursor" : "[\u2190\u2192] Tab";
    const bool in_search = meta_tab_ == 0 && meta_focus_ == 0;
    const std::string esc = in_search ? "[ESC] Clear/Exit\001Exit" : in_text ? "[ESC] Leave field" : "[ESC] Exit";
    std::string enter_hint = "[ENTER] Edit";
    if (meta_tab_ == 0 && meta_focus_ == 2) {
        const MetaEditEntry* he = meta_entry(meta_hovering_path());
        if (meta_field_ == kMetaFieldCount) enter_hint = "[ENTER] Cover Options\001Cover";
        else if (meta_field_ == kMetaFieldCount + 1) enter_hint = "[ENTER] Style Option\001Style";
        else if (meta_field_ == kMetaFieldCount + 2) enter_hint = "[ENTER] Video Option\001Video";
        else if (he && meta_field_ >= 0 && he->needs_choice(meta_field_)) enter_hint = "[ENTER] Choose";
    }
    // The legend is a small tree table: one row per group of commands (label | commands), joined by tree lines. Four rows
    // on both tabs, so the footer always has the same height -- 4 legend rows + the status row -- and switching tabs or a
    // message appearing never moves the panels. (A fetch confirmation replaces all of it; see the prompt branch.)
    //   Move     moving around / working on the hovered title
    //   Fetch    fetching tags (the same commands on both tabs)
    //   View     the library view (EDIT tab) / the fetch list (FETCH LIST tab)
    //   Session  save / discard / leave
    // It adapts to the width: the commands of a row are spaced out while they fit and packed tighter, then the last ones
    // are replaced by "..." when the terminal is narrow (the most important commands come first in every row).
    std::vector<LegendGroup> groups;
    const std::string rescan = "[" + hotkey_text("HKeyRescan", "R") + "] Rescan paths\001Rescan";
    const std::string fetch_missing = "[" + hotkey_text("HKeyFetchTags", "B") + "] Fetch missing\001Missing";
    if (meta_tab_ == 0) {
        groups.push_back({"Move", {lr, "[TAB] Focus", "[\u2191\u2193] Navi.", "[SHIFT+\u2191\u2193] Page\001Page", "[CTRL+\u2191\u2193] Top/bottom\001Top/bot.", enter_hint, "[SHIFT+p] Play", "[SHIFT+c] Current song\001Current"}});
        groups.back().max_lines = 2;   // Move: a second row when SHIFT+c and the others do not fit one (120 columns)
        groups.push_back({"Fetch", {"[a] To list", "[SHIFT+f] All searched to fetch list\001Searched to list", "\002Single track\003", fetch_missing, "[SHIFT+n] (Re-)fetch all\001(Re-)fetch all"}});
        groups.push_back({"Filter", {"[SHIFT+v] Video only\001Video", "[f] Folder view\001Folders", "[r] Recently edited first\001Edited", "[n] Recently added first\001Added", "[x/T/A/L/Y] Specific missing tags\001Missing", rescan, "[t] ASCII / Full Resolution\001ASCII/Full"}});
        groups.back().max_lines = 2;   // Filter: two rows too, broken between two commands
    } else {
        LegendGroup mv{"Move", {meta_fetch_focus_ == 1 ? std::string("[\u2190\u2192] Option") : lr, "[TAB] Focus"}};
        if (meta_fetch_focus_ == 1) mv.cmds.push_back("[SPACE] Tick");
        mv.cmds.push_back("[\u2191\u2193] Navi.");
        mv.cmds.push_back("[SHIFT+\u2191\u2193] Page\001Page");
        mv.cmds.push_back("[CTRL+\u2191\u2193] Top/bottom\001Top/bot.");
        mv.cmds.push_back("[SHIFT+p] Play");
        groups.push_back(mv);
        // \002...\003 marks a sub-heading: paint_legend_row() draws it in the header colour (the markers take no column)
        groups.push_back({"Fetch", {"[ENTER] Fetch list\001Fetch list", "[SHIFT+c] Cancel fetch\001Cancel", "\002Single track\003", fetch_missing, "[SHIFT+n] (Re-)fetch all\001(Re-)fetch all"}});
        groups.push_back({"List", {"[DEL] Remove", "[SHIFT+d] Empty list\001Empty", rescan}});
    }
    groups.push_back({"Session", {"[CTRL+s] Save", "[CTRL+SHIFT+x] Discard", esc}});
    int budget = std::min(target_height, term_rows_ - 1);
    auto lrows = layout_legend_tree(groups, W - 1);
    if (budget - fixed_rows - (2 + static_cast<int>(lrows.size()) + 1) < 8)   // a low terminal: no second legend rows, the panels keep their room
        { for (auto& g : groups) g.max_lines = 1; lrows = layout_legend_tree(groups, W - 1); }
    const int footer_rows = std::max(5, static_cast<int>(lrows.size()) + 1);   // the legend + the status row
    int panel_h = std::max(6, budget - fixed_rows - (2 + footer_rows));
    meta_page_rows_ = std::max(1, panel_h - 3); // no upper cap: fills a maximised window (7 = 2 border rows + the 5-row footer)
    if (meta_tab_ == 0) {
        int left_w = W / 2;
        int right_w = W - left_w;
        meta_pane_col_ = left_w;
        auto left_lines = build_meta_library_panel(left_w, panel_h);
        auto right_lines = build_meta_fields_panel(right_w, panel_h);
        size_t rows = std::max(left_lines.size(), right_lines.size());
        for (size_t i = 0; i < rows; ++i) {
            std::string l = (i < left_lines.size()) ? left_lines[i] : std::string(left_w, ' ');
            std::string r = (i < right_lines.size()) ? right_lines[i] : std::string(right_w, ' ');
            frame << l << r << "\n";
        }
    } else {
        for (auto& l : build_meta_fetch_panel(W, panel_h)) frame << l << "\n";
    }

    if (meta_prompt_ != MetaPrompt::None) {
        // Wrapped to at most 2 lines: the AcoustID disclaimer is longer
        // than a narrow terminal, and an unwrapped bar would wrap into a
        // third line and push the whole overlay past its own height.
        std::string text;
        std::string colors;
        if (meta_prompt_ == MetaPrompt::Fetch) {
            text = meta_fetch_disclaimer() + "   [Y]es   [N]o   [ESC] cancel";
            colors = "\x1b[43;30m"; // yellow, same as the playlist editor's save prompt
        } else if (meta_prompt_ == MetaPrompt::Save) {
            int open_choices = 0;
            for (const auto& en : meta_session_) for (int i = 0; i < kMetaFieldCount; ++i) if (en.needs_choice(i)) ++open_choices;
            text = "Want to save? Applies every pending edit to the files."
                 + (open_choices ? " " + std::to_string(open_choices) + " fetched tag" + (open_choices == 1 ? "" : "s")
                                       + " still ha" + (open_choices == 1 ? "s" : "ve") + " several options -- the first one is saved." : std::string())
                 + "   [Y]es   [N]o   [ESC] cancel";
            colors = "\x1b[43;30m";
        } else if (meta_prompt_ == MetaPrompt::ClearFetch) {
            text = "Empty the fetch list (" + std::to_string(meta_fetch_list_.size()) + " titles)? Pending edits, fetched tags included, are kept.   [Y]es   [N]o   [ESC] cancel";
            colors = "\x1b[43;30m";
        } else {
            text = "Want to discard changes? The files themselves were never touched.   [Y]es   [N]o   [ESC] cancel";
            colors = "\x1b[41;97m"; // red, same as its delete prompt
        }
        auto lines = wrap_lines(text, std::max(10, W - 2), 2);
        // Padded to the same five rows the legend footer always uses (four legend rows + the status row), so a
        // prompt appearing (or going away) can never change the height of
        // what sits above it.
        while (static_cast<int>(lines.size()) < footer_rows) lines.push_back(std::string());
        for (const auto& l : lines) {
            // The trailing row is reserved (so the footer never changes
            // height between a prompt and a hint/status), but a reserved
            // row stays bare: a full-width empty color bar reads as a
            // rendering glitch, not as an empty status line.
            if (l.empty()) frame << "\n";
            else frame << colors << " " << l << " \x1b[0m\n";
        }
    } else {
        {
            for (size_t i = 0; i < lrows.size(); ++i) frame << " " << paint_legend(settings_, lrows, i, border, 0, true) << "\n";
        }
        if (!meta_status_.empty()) frame << "\x1b[38;5;246m" << truncate_str(meta_status_, W) << "\x1b[0m\n";
        else frame << "\n";
    }
}

// ---------------------------------------------------------------------------
// Listening history overlay (Mode::History, HKeyHistory = Shift+H)
//
// Modelled on the two overlays above: same tab strip, same boxed panel, same
// "two legend rows + one status row" footer, so its height never depends on
// what it happens to be showing. Three tabs:
//
//   1 HISTORY     the last 100 plays, newest first, each with a PLAY / done /
//                 skip marker (the live play is the top row while music runs)
//   2 TOP TRACKS  one row per title -- length and play count -- sorted by play
//                 count; 'r' flips between most- and least-played first
//   3 TOP PLAYLISTS  the top 5 saved playlists of the week / month / quarter /
//                 year / overall in one pane (history_playlists_lines())
//   4 HABITS      session / time-per-day / play-behaviour aggregates, every
//                 category under a header_sgr() header, exactly like the
//                 Settings section titles and the cheat sheet's categories
//
// It is a pure viewer: no key in here edits anything, and 'r' only flips an
// in-memory sort order. The data is maintained outside this overlay -- see
// history_end_current_play()/history_begin_current_play() below (called on
// every playback hand-over) and the frame loop's history_.add_listened().
// ---------------------------------------------------------------------------


} // namespace muisc
