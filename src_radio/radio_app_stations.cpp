#include "radio_app.h"
#include "string_util.h"
#if defined(_WIN32)
#include "win_compat.h"
#endif

// ---- the big STATIONS overlay (key L): add by URL (a), preset name (SHIFT+c) -------------------------------
void RadioApp::stov_add_commit() {
    StationsOverlay& so = ui.model.stov;
    std::string url = so.add_url;
    while (!url.empty() && (url.back() == ' ' || url.back() == '\t')) url.pop_back();
    while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) url.erase(url.begin());
    const bool http = url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
    if (!http && url.rfind("lavfi:", 0) != 0) { so.add_error = "Not a stream address: it has to start with http:// or https://"; return; }
    for (size_t i = 0; i < ui.stations.size(); ++i)
        if (same_stream_url(ui.stations[i].url, url)) {
            so.add_error = "Already in your list as \"" + ui.stations[i].name + "\" (no. " + std::to_string(i + 1) + ")";
            return;
        }
    Station st;
    st.url = url;
    std::string name = so.add_name;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.empty()) {   // the host name
        size_t a = url.find("://");
        a = a == std::string::npos ? 0 : a + 3;
        size_t b = url.find_first_of("/:?", a);
        name = url.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (name.rfind("www.", 0) == 0) name.erase(0, 4);
    }
    st.name = name;
    st.dial_mhz = dial_for_name(name);
    ui.stations.push_back(st);   // appended to the end, so every preset slot (a station INDEX) stays valid
    std::string where, err;
    const bool saved = append_station_to_file(st, ui.stations, &where, &err);
    ui.refilter(); ui.refilter_menu(); ui.refilter_lists_stations();
    so.flash = "Added \"" + st.name + "\" as no. " + std::to_string(ui.stations.size())
             + (saved ? "  (saved to " + where + ")" : "  -- NOT saved: " + err + " (it stays in the list until you quit)");
    so.add_open = false; so.add_error.clear(); set_text_entry(false);
}

void RadioApp::stov_snapshot(const std::string& what) {
    ui.sync_active();
    stov_undo.push_back({ui.stations, ui.model.banks, ui.model.presets, ui.model.bank_active, ui.model.lists, history, ui.model.lmenu.items, what});
    if (stov_undo.size() > 20) stov_undo.erase(stov_undo.begin());
    ui.model.stov.undo_count = static_cast<int>(stov_undo.size());
}

// The stream that plays goes on whatever happens to the list: only its index is looked up again (by its URL; -1 = not in the list).
void RadioApp::stov_resync_tuned() {
    const RadioStatus rs = engine.status();
    if (rs.tuned_url.empty()) return;
    int found = -1;
    for (size_t i = 0; i < ui.stations.size(); ++i) if (same_stream_url(ui.stations[i].url, rs.tuned_url)) { found = static_cast<int>(i); break; }
    if (found != rs.tuned_index) engine.set_tuned_index(found);
}

void RadioApp::stov_refresh() {
    ui.refilter(); ui.refilter_menu(); ui.refilter_presets(); ui.refilter_lists_stations(); ui.refilter_saved_lists(); ui.refilter_main_lists();
    UiModel& um = ui.model;
    um.cursor = std::clamp(um.cursor, 0, std::max(0, static_cast<int>(um.visible.size()) - 1));
}

// Writes everything a change touches; returns "" or what could not be written.
std::string RadioApp::stov_persist_all() {
    std::string err, bad;
    if (!save_all_stations(ui.stations, nullptr, &err)) bad = err;
    else if (!save_preset_names(ui.stations)) bad = "cannot write " + preset_names_path();
    else if (!ui.persist()) bad = "cannot write " + presets_path();
    else if (!save_station_lists(ui.stations, ui.model.lists)) bad = "cannot write " + station_lists_path();
    return bad;
}

void RadioApp::stov_delete(int idx) {
    StationsOverlay& so = ui.model.stov;
    if (idx < 0 || idx >= static_cast<int>(ui.stations.size())) return;
    if (ui.stations.size() <= 1) { so.flash = "the last station cannot be deleted"; return; }
    stov_snapshot("delete");
    const std::string nm = ui.stations[static_cast<size_t>(idx)].name;
    ui.stations.erase(ui.stations.begin() + idx);
    auto fix_slot = [&](int& v) { if (v == idx) v = -1; else if (v > idx) --v; };
    for (auto& b : ui.model.banks) for (int& v : b.slots) fix_slot(v);
    for (int& v : ui.model.presets) fix_slot(v);
    auto fix_items = [&](std::vector<int>& items) {
        items.erase(std::remove(items.begin(), items.end(), idx), items.end());
        for (int& v : items) if (v > idx) --v;
    };
    for (auto& l : ui.model.lists) fix_items(l.items);
    fix_items(history);
    fix_items(ui.model.lmenu.items);
    stov_resync_tuned();
    stov_refresh();
    const std::string bad = stov_persist_all();
    so.flash = "Deleted \"" + nm + "\"  (SHIFT+u undoes it)" + (bad.empty() ? "" : "  -- NOT saved: " + bad);
}

void RadioApp::stov_undo_do() {
    StationsOverlay& so = ui.model.stov;
    if (stov_undo.empty()) { so.flash = "nothing to undo"; return; }
    StationUndo u = std::move(stov_undo.back());
    stov_undo.pop_back();
    so.undo_count = static_cast<int>(stov_undo.size());
    ui.stations = std::move(u.stations);
    ui.model.banks = std::move(u.banks);
    ui.model.presets = std::move(u.presets);
    ui.model.bank_active = u.bank_active;
    ui.model.lists = std::move(u.lists);
    history = std::move(u.history);
    ui.model.lmenu.items = std::move(u.lm_items);
    stov_resync_tuned();
    stov_refresh();
    const std::string bad = stov_persist_all();
    so.flash = "Undone: " + u.what + (bad.empty() ? "" : "  -- NOT saved: " + bad);
}

void RadioApp::stov_edit_open(int idx) {
    StationsOverlay& so = ui.model.stov;
    const Station& st0 = ui.stations[static_cast<size_t>(idx)];
    so.edit_open = true; so.edit_idx = idx; so.edit_field = 0; so.edit_error.clear(); so.flash.clear();
    const std::string v[StationsOverlay::kEditFields] = {st0.name, st0.preset_name, st0.url, st0.genre, st0.country, st0.codec_hint, st0.bitrate_hint > 0 ? std::to_string(st0.bitrate_hint) : std::string("0")};
    for (int f = 0; f < StationsOverlay::kEditFields; ++f) { so.edit_text[f] = v[f]; so.edit_ed[f].to_end(v[f]); }
    set_text_entry(true);
}

void RadioApp::stov_edit_commit() {
    StationsOverlay& so = ui.model.stov;
    if (so.edit_idx < 0 || so.edit_idx >= static_cast<int>(ui.stations.size())) { so.edit_open = false; set_text_entry(false); return; }
    auto trim = [](std::string t) {
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        size_t b = 0; while (b < t.size() && (t[b] == ' ' || t[b] == '\t')) ++b;
        t.erase(0, b);
        for (auto& c : t) if (c == '|') c = '/'; else if (static_cast<unsigned char>(c) < 32) c = ' ';
        return t;
    };
    const std::string name = trim(so.edit_text[0]), pre = trim(so.edit_text[1]), url = trim(so.edit_text[2]);
    const std::string genre = trim(so.edit_text[3]), country = trim(so.edit_text[4]), codec = trim(so.edit_text[5]), kbps_s = trim(so.edit_text[6]);
    if (name.empty()) { so.edit_error = "The name cannot be empty"; so.edit_field = 0; return; }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0 && url.rfind("lavfi:", 0) != 0) { so.edit_error = "Not a stream address: it has to start with http:// or https://"; so.edit_field = 2; return; }
    for (size_t i = 0; i < ui.stations.size(); ++i)
        if (static_cast<int>(i) != so.edit_idx && same_stream_url(ui.stations[i].url, url)) {
            so.edit_error = "That address is already station no. " + std::to_string(i + 1) + ": \"" + ui.stations[i].name + "\""; so.edit_field = 2; return;
        }
    int kbps = 0;
    if (!kbps_s.empty()) {
        for (char c : kbps_s) if (c < '0' || c > '9') { so.edit_error = "KBPS is a number (0 = unknown)"; so.edit_field = 6; return; }
        kbps = std::min(99999, muisc::atoi_safe(kbps_s.c_str()));
    }
    Station& st0 = ui.stations[static_cast<size_t>(so.edit_idx)];
    if (st0.name == name && st0.preset_name == pre && st0.url == url && st0.genre == genre && st0.country == country && st0.codec_hint == codec && st0.bitrate_hint == kbps) {
        so.edit_open = false; so.edit_error.clear(); set_text_entry(false); so.flash = "nothing changed"; return;
    }
    stov_snapshot("edit of \"" + st0.name + "\"");
    if (st0.dial_mhz == dial_for_name(st0.name)) st0.dial_mhz = dial_for_name(name);
    st0.name = name; st0.preset_name = pre; st0.url = url; st0.genre = genre; st0.country = country; st0.codec_hint = codec; st0.bitrate_hint = kbps;
    stov_resync_tuned();
    stov_refresh();
    const std::string bad = stov_persist_all();
    so.flash = "Saved \"" + name + "\"  (SHIFT+u undoes it)" + (bad.empty() ? "" : "  -- NOT saved: " + bad);
    so.edit_open = false; so.edit_error.clear(); set_text_entry(false);
}

void RadioApp::browse_focus(BrowseFocus f) {
    BrowseModel& bm = ui.model.browse;
    bm.focus = f;
    if (f != BrowseFocus::Results) bm.edit[static_cast<int>(f)].to_end(bm.text[static_cast<int>(f)]);
    set_text_entry(f != BrowseFocus::Results);
}

void RadioApp::browse_open() {
    BrowseModel& bm = ui.model.browse;
    bm.open = true; bm.flash.clear();
    browse_focus(BrowseFocus::Name);
}

void RadioApp::browse_close() {
    ui.model.browse.open = false; set_text_entry(false); ui.model.browse.flash.clear();
}

void RadioApp::browse_search() {
    BrowseModel& bm = ui.model.browse;
    BrowseQuery q;
    q.name = bm.text[0]; q.tags = bm.text[1]; q.country = bm.text[2];
    q.state = bm.text[3]; q.language = bm.text[4]; q.bitrate = bm.text[5];
    bm.flash.clear();
    bm.cursor = 0;
    browse_jump_to_results = true;
    browser.search(q);
}

// The hovered result as a station of the main list (`a`): appended to the end -- so every preset slot, which
// holds a station INDEX, stays valid -- and written to stations.txt.
void RadioApp::browse_add(const BrowseStation& b) {
    BrowseModel& bm = ui.model.browse;
    Station st = browse_to_station(b);
    for (size_t i = 0; i < ui.stations.size(); ++i)
        if (same_stream_url(ui.stations[i].url, st.url)) { bm.flash = "\"" + ui.stations[i].name + "\" is already in your list (no. " + std::to_string(i + 1) + ")"; return; }
    ui.stations.push_back(st);
    std::string where, err;
    const bool saved = append_station_to_file(st, ui.stations, &where, &err);
    ui.refilter(); ui.refilter_menu();
    bm.flash = "Added \"" + st.name + "\" as no. " + std::to_string(ui.stations.size())
             + (saved ? "  (saved to " + where + ")" : "  -- NOT saved: " + err + " (it stays in the list until you quit)");
}

// ENTER in RESULTS: tune it. A stream that is already in the list tunes the list entry (so the list, the band and
// the presets light up); anything else is tuned directly (index -1).
void RadioApp::browse_tune(const BrowseStation& b) {
    const Station st = browse_to_station(b);
    int idx = -1;
    for (size_t i = 0; i < ui.stations.size(); ++i) if (same_stream_url(ui.stations[i].url, st.url)) { idx = static_cast<int>(i); break; }
    if (idx >= 0) tune_to(idx); else engine.tune(st, -1);
    browser.count_click(b.uuid);   // Radio Browser asks for one click per started stream
    ui.model.browse.flash = "Tuning \"" + st.name + "\"";
}

// ---- STATION LISTS menu (Shift+K) -- behaves like the player's playlist editor ----------------------
// Tab tells which text field (if any) owns the keyboard; the terminal's text-entry mode follows it.
std::string RadioApp::n_stations(size_t n) {
    return std::to_string(n) + (n == 1 ? " station" : " stations");
}

bool RadioApp::lm_in_text() {
    const ListMenuModel& lm = ui.model.lmenu;
    if (!lm.open || lm.confirm_exit || lm.confirm_delete) return false;
    if (lm.tab == 0) return lm.focus == ListFocus::Name || lm.focus == ListFocus::Search;
    return lm.mfocus == ListManageFocus::Search;
}

void RadioApp::lm_sync_entry() {
    set_text_entry(lm_in_text());
}

void RadioApp::lm_open() {
    ListMenuModel& lm = ui.model.lmenu;
    // Like the player: opening always starts a fresh, empty list on tab 1 with the name field focused
    // (an existing list comes back only explicitly, tab 2 -> ENTER; ESC asks before dropping unsaved work).
    lm.open = true; lm.tab = 0; lm.focus = ListFocus::Name; lm.typing = false;
    lm.name.clear(); lm.name_edit.reset();
    lm.items.clear(); lm.item_cursor = 0;
    lm.search.clear(); lm.search_edit.reset(); lm.cursor = 0;
    lm.msearch.clear(); lm.msearch_edit.reset(); lm.mcursor = 0; lm.mfocus = ListManageFocus::List;
    lm.dirty = false; lm.confirm_exit = false; lm.confirm_delete = false; lm.flash.clear();
    ui.refilter_lists_stations();
    ui.refilter_saved_lists();
    lm_sync_entry();
}

void RadioApp::lm_close() {
    ListMenuModel& lm = ui.model.lmenu;
    lm.open = false; lm.confirm_exit = false; lm.confirm_delete = false; lm.flash.clear();
    set_text_entry(false);
    ui.refilter(); ui.refilter_main_lists();   // a list shown in the STATIONS pane may have changed or be gone
}

void RadioApp::lm_focus(ListFocus f) {
    ListMenuModel& lm = ui.model.lmenu;
    lm.focus = f;
    lm.typing = false;
    if (f == ListFocus::Name) lm.name_edit.to_end(lm.name);
    if (f == ListFocus::Search) lm.search_edit.to_end(lm.search);
    lm_sync_entry();
}

void RadioApp::lm_mfocus(ListManageFocus f) {
    ListMenuModel& lm = ui.model.lmenu;
    lm.mfocus = f;
    lm.typing = false;
    if (f == ListManageFocus::Search) lm.msearch_edit.to_end(lm.msearch);
    lm_sync_entry();
}

// CTRL+s. Saves under the typed name and STAYS in the menu (like the player); only the "Save changes ... before
// exiting?" prompt saves AND leaves. An empty name is refused and focuses the name field.
void RadioApp::lm_save(bool leave) {
    ListMenuModel& lm = ui.model.lmenu;
    const std::string nm = clean_list_name(lm.name);
    if (nm.empty()) {
        lm.flash = "enter a name first";
        lm.tab = 0;
        lm_focus(ListFocus::Name);
        return;
    }
    StationList sl;
    sl.name = nm; sl.items = lm.items;
    bool replaced = false;
    for (auto& l : ui.model.lists)
        if (ascii_lower_str(l.name) == ascii_lower_str(nm)) { l = sl; replaced = true; break; }   // same name: overwritten, like the player's playlists
    if (!replaced) ui.model.lists.push_back(sl);
    if (lm.name != nm) { lm.name = nm; lm.name_edit.to_end(lm.name); }
    ui.refilter_saved_lists();
    if (!save_station_lists(ui.stations, ui.model.lists)) {
        lm.flash = "save failed: cannot write " + station_lists_path();
        return;
    }
    lm.flash = std::string(replaced ? "saved list \"" : "saved new list \"") + nm + "\" (" + n_stations(lm.items.size()) + ")";
    lm.dirty = false;
    if (leave) lm_close();
}

// Tab 2, ENTER: the hovered saved list moves into the editor (tab 1).
void RadioApp::lm_load() {
    ListMenuModel& lm = ui.model.lmenu;
    if (lm.mcursor < 0 || lm.mcursor >= static_cast<int>(lm.mvisible.size())) return;
    const StationList& l = ui.model.lists[static_cast<size_t>(lm.mvisible[static_cast<size_t>(lm.mcursor)])];
    lm.name = l.name; lm.name_edit.to_end(lm.name);
    lm.items = l.items; lm.item_cursor = 0;
    lm.dirty = false;    // freshly loaded from disk: nothing to lose yet
    lm.tab = 0;
    lm.flash = "editing \"" + l.name + "\" (" + n_stations(l.items.size()) + ")";
    lm_focus(ListFocus::Search);
}

void RadioApp::lm_delete() {
    ListMenuModel& lm = ui.model.lmenu;
    if (lm.mcursor < 0 || lm.mcursor >= static_cast<int>(lm.mvisible.size())) return;
    const size_t li = static_cast<size_t>(lm.mvisible[static_cast<size_t>(lm.mcursor)]);
    const std::string nm = ui.model.lists[li].name;
    ui.model.lists.erase(ui.model.lists.begin() + static_cast<std::ptrdiff_t>(li));
    ui.refilter_saved_lists();
    lm.flash = save_station_lists(ui.stations, ui.model.lists) ? "deleted list \"" + nm + "\""
                                                                : "deleted \"" + nm + "\" -- but could not write " + station_lists_path();
}

// ENTER in SEARCH / STATIONS: the hovered station goes to the end of the list (a station sits in a list once).
void RadioApp::lm_add() {
    ListMenuModel& lm = ui.model.lmenu;
    if (lm.cursor < 0 || lm.cursor >= static_cast<int>(lm.visible.size())) return;
    const int idx = lm.visible[static_cast<size_t>(lm.cursor)];
    const std::string& nm = ui.stations[static_cast<size_t>(idx)].name;
    if (std::find(lm.items.begin(), lm.items.end(), idx) != lm.items.end()) { lm.flash = "\"" + nm + "\" is already in this list"; return; }
    lm.items.push_back(idx);
    lm.dirty = true;
    lm.flash = "added \"" + nm + "\" (" + n_stations(lm.items.size()) + ")";
}

void RadioApp::lm_remove() {
    ListMenuModel& lm = ui.model.lmenu;
    if (lm.item_cursor < 0 || lm.item_cursor >= static_cast<int>(lm.items.size())) return;
    const std::string nm = ui.stations[static_cast<size_t>(lm.items[static_cast<size_t>(lm.item_cursor)])].name;
    lm.items.erase(lm.items.begin() + lm.item_cursor);
    lm.item_cursor = std::clamp(lm.item_cursor, 0, std::max(0, static_cast<int>(lm.items.size()) - 1));
    lm.dirty = true;
    lm.flash = "removed \"" + nm + "\" (" + n_stations(lm.items.size()) + ")";
}

void RadioApp::lm_move(int dir) {
    ListMenuModel& lm = ui.model.lmenu;
    const int target = lm.item_cursor + dir;
    if (lm.item_cursor < 0 || lm.item_cursor >= static_cast<int>(lm.items.size()) || target < 0 || target >= static_cast<int>(lm.items.size())) return;
    std::swap(lm.items[static_cast<size_t>(lm.item_cursor)], lm.items[static_cast<size_t>(target)]);
    lm.item_cursor = target;
    lm.dirty = true;
}
