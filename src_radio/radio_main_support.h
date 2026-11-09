// The helpers of the radio main loop (radio_main.cpp): the text edit, the Ui state, the geometry check of the test frames.
// Kept in a header of their own so that radio_main.cpp holds the loop only; only radio_main.cpp includes it.
#pragma once
#include "bars_rows.h"
#include "translit.h"
#include "utf8_util.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "about_text.h"
#include "radio_browser.h"
#include "radio_engine.h"
#include "terminal_gfx.h"
#include "remote_control.h"
#include "update_check.h"
#include "open_url.h"
#include "radio_stations.h"
#include "radio_ui.h"
#include "scope_window.h"
#include "keyboard_layout.h"
#include "spectrogram.h"
#include "mode_switch.h"
#include "settings.h"
#include "radio_settings.h"
#include "terminal_font.h"
#include "radio_fuzzy.h"
#include "radio_history.h"
#include "radio_yt.h"
#include "wizard.h"      // the help wizard's pages, shared with the music player
#include "terminal_ui.h"

using namespace muisc;
using namespace muisc::radio;
using Clock = std::chrono::steady_clock;

namespace radio_main_detail {

inline std::string strip_ansi(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
            continue;
        }
        o += s[i];
    }
    return o;
}

// Longest text a search box takes (bytes), same order of magnitude as the player's boxes.
constexpr size_t kTextMax = 200;

inline size_t count_chars(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// A text field limited in CHARACTERS (the editor limits bytes): a typed character into a full field is refused, a
// paste that is too long keeps what fits. Returns true when the text changed. (Same logic as the preset name box.)
inline bool edit_limited(std::string& text, EditState& e, int key, size_t max_chars, std::string* status) {
    const std::string before = text;
    const EditState before_edit = e;
    if (!edit_text_key(text, e, key, max_chars * 4, status)) return false;
    if (count_chars(text) > max_chars) {
        if (key == kKeyCtrlV) {
            const size_t start = std::min(before_edit.caret, before_edit.anchor);
            while (count_chars(text) > max_chars && e.caret > start) {
                size_t i = e.caret - 1;
                while (i > start && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) --i;
                text.erase(i, e.caret - i);
                e.caret = i;
            }
            e.anchor = e.caret;
        } else { text = before; e = before_edit; return false; }
    }
    return text != before;
}

// The text for searching: itself, and behind it its Latin spelling when it holds Kanji / kana / Cyrillic ... (translit.h, the same
// as the music player's search): "米津玄師" is found by typing "yonezu".
inline std::string search_text(const std::string& s) { return translit_possible(s) ? with_latin_spelling(s) : s; }

struct Ui {
    std::vector<Station> stations;
    UiModel model;
    // Station indices matching `query` (name, genre or country), typo tolerant like the music player's search
    // (radio_fuzzy.h): an exact substring first (earlier hit first), then close fuzzy matches; no query = list order.
    std::vector<int> filter(const std::string& query, const std::vector<int>* pool = nullptr) const {
        std::vector<int> out;
        const size_t n = pool ? pool->size() : stations.size();
        std::vector<std::pair<double, int>> scored;
        for (size_t p = 0; p < n; ++p) {
            const int i = pool ? (*pool)[p] : static_cast<int>(p);
            if (i < 0 || i >= static_cast<int>(stations.size())) continue;
            if (query.empty()) { out.push_back(i); continue; }
            const auto& s = stations[static_cast<size_t>(i)];
            double sc = fuzzy_score(query, search_text(s.name));
            sc = std::max(sc, fuzzy_score(query, search_text(s.preset_name)));
            sc = std::max(sc, fuzzy_score(query, search_text(s.genre)));
            sc = std::max(sc, fuzzy_score(query, search_text(s.country)));
            if (sc > 0.0) scored.emplace_back(sc, i);
        }
        if (!query.empty()) {
            std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (const auto& pr : scored) out.push_back(pr.second);
        }
        return out;
    }
    // Indices of `names` matching `query` the same way (best match first; no query = in order).
    static std::vector<int> fuzzy_names(const std::vector<std::string>& names, const std::string& query) {
        std::vector<int> out;
        std::vector<std::pair<double, int>> scored;
        for (size_t i = 0; i < names.size(); ++i) {
            if (query.empty()) { out.push_back(static_cast<int>(i)); continue; }
            const double sc = fuzzy_score(query, search_text(names[i]));
            if (sc > 0.0) scored.emplace_back(sc, static_cast<int>(i));
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& pr : scored) out.push_back(pr.second);
        return out;
    }
    void refilter_menu() {
        model.menu.visible = filter(model.menu.search);
        model.menu.cursor = std::clamp(model.menu.cursor, 0, std::max(0, static_cast<int>(model.menu.visible.size()) - 1));
    }
    // Saved station lists whose name contains `query` (main search box, "p:").
    void refilter_main_lists() {
        std::vector<std::string> names;
        for (const auto& l : model.lists) names.push_back(l.name);
        model.lvisible = fuzzy_names(names, model.search_lists ? model.search : std::string());
        model.lcursor = std::clamp(model.lcursor, 0, std::max(0, static_cast<int>(model.lvisible.size()) - 1));
    }
    // The list the STATIONS pane shows, or nullptr (none chosen, or it has been deleted / renamed meanwhile).
    const StationList* active_list_ptr() const {
        if (model.active_list.empty()) return nullptr;
        for (const auto& l : model.lists) if (l.name == model.active_list) return &l;
        return nullptr;
    }
    // Sorts station indices by name (case-insensitive, stable) -- the SHIFT+t "A-Z" order. Without `az` the
    // order stays what filter() produced: the list order, i.e. the order the stations were added.
    void sort_by_name(std::vector<int>& v, bool az) const {
        if (!az) return;
        std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
            return ascii_lower_str(stations[static_cast<size_t>(a)].name) < ascii_lower_str(stations[static_cast<size_t>(b)].name);
        });
    }
    void refilter() {
        const StationList* al = active_list_ptr();
        if (!al) model.active_list.clear();
        // while the box searches the LISTS ("p:") the stations are not filtered by what is typed there
        const std::string q = model.search_lists ? std::string() : model.search;
        model.visible = al ? filter(q, &al->items) : filter(q);
        sort_by_name(model.visible, model.sort_az);
        model.cursor = std::clamp(model.cursor, 0, std::max(0, static_cast<int>(model.visible.size()) - 1));
    }
    // SHIFT+t in the main UI: flips the sort and keeps the cursor on the same station.
    void toggle_sort() {
        const int keep = (model.cursor >= 0 && model.cursor < static_cast<int>(model.visible.size())) ? model.visible[static_cast<size_t>(model.cursor)] : -1;
        model.sort_az = !model.sort_az;
        refilter();
        for (size_t i = 0; i < model.visible.size(); ++i) if (model.visible[i] == keep) { model.cursor = static_cast<int>(i); break; }
    }
    // STATION LISTS menu: STATIONS pane (SEARCH ALL STATIONS) and the saved-lists search.
    void refilter_lists_stations() {
        ListMenuModel& lm = model.lmenu;
        lm.visible = filter(lm.search);
        sort_by_name(lm.visible, lm.sort_az);
        lm.cursor = std::clamp(lm.cursor, 0, std::max(0, static_cast<int>(lm.visible.size()) - 1));
    }
    void toggle_lists_sort() {
        ListMenuModel& lm = model.lmenu;
        const int keep = (lm.cursor >= 0 && lm.cursor < static_cast<int>(lm.visible.size())) ? lm.visible[static_cast<size_t>(lm.cursor)] : -1;
        lm.sort_az = !lm.sort_az;
        refilter_lists_stations();
        for (size_t i = 0; i < lm.visible.size(); ++i) if (lm.visible[i] == keep) { lm.cursor = static_cast<int>(i); break; }
    }
    void refilter_saved_lists() {
        ListMenuModel& lm = model.lmenu;
        std::vector<std::string> names;
        for (const auto& l : model.lists) names.push_back(l.name);
        lm.mvisible = fuzzy_names(names, lm.msearch);
        lm.mcursor = std::clamp(lm.mcursor, 0, std::max(0, static_cast<int>(lm.mvisible.size()) - 1));
    }
    // Preset (bank) indices whose name contains `query` (case-insensitive), in list order.
    void refilter_presets() {
        MenuModel& mn = model.menu;
        std::vector<std::string> names;
        for (const auto& b : model.banks) names.push_back(b.name);
        mn.pvisible = fuzzy_names(names, mn.psearch);
        mn.pcursor = std::clamp(mn.pcursor, 0, std::max(0, static_cast<int>(mn.pvisible.size()) - 1));
    }
    // Puts the cursor of the SELECT PRESET pane on the active preset (if the search lets it through).
    void cursor_to_active_preset() {
        MenuModel& mn = model.menu;
        for (size_t i = 0; i < mn.pvisible.size(); ++i)
            if (mn.pvisible[i] == model.bank_active) { mn.pcursor = static_cast<int>(i); return; }
    }
    // `model.presets` is the working copy of the active preset's slots: copy it back before anything
    // looks at (or writes) `banks`.
    void sync_active() {
        if (model.bank_active >= 0 && model.bank_active < static_cast<int>(model.banks.size()))
            model.banks[static_cast<size_t>(model.bank_active)].slots = model.presets;
    }
    void use_bank(int i) {
        if (i < 0 || i >= static_cast<int>(model.banks.size())) return;
        sync_active();
        model.bank_active = i;
        model.presets = model.banks[static_cast<size_t>(i)].slots;
    }
    bool persist() {
        sync_active();
        PresetStore store;
        store.banks = model.banks;
        store.active = model.bank_active;
        return save_presets(stations, store);
    }
    // Shift+Left / Shift+Right: previous / next preset (wraps).
    void step_bank(int delta) {
        const int n = static_cast<int>(model.banks.size());
        if (n < 2) return;
        use_bank(((model.bank_active + delta) % n + n) % n);
        persist();
    }
    bool bank_name_taken(const std::string& name, int except) const {
        const std::string l = ascii_lower_str(name);
        for (size_t i = 0; i < model.banks.size(); ++i)
            if (static_cast<int>(i) != except && ascii_lower_str(model.banks[i].name) == l) return true;
        return false;
    }
};

// Removes `idx` from every preset slot. Returns true if it was in one.
inline bool clear_station_from_presets(std::vector<int>& presets, int idx) {
    bool any = false;
    for (auto& p : presets) if (p == idx) { p = -1; any = true; }
    return any;
}

inline bool check_geometry(const std::vector<std::string>& frame, const UiModel& m) {
    const int want_w = std::clamp(m.cols, kUiCols, kMaxCols);
    const int want_h = std::max(m.rows, kUiRows);
    bool ok = static_cast<int>(frame.size()) == want_h;
    if (!ok) std::fprintf(stderr, "GEOMETRY: %zu rows, expected %d\n", frame.size(), want_h);
    for (size_t i = 0; i < frame.size(); ++i) {
        const int w = display_width(strip_ansi(frame[i]));
        if (w != want_w) { std::fprintf(stderr, "GEOMETRY: row %zu is %d columns, expected %d\n", i, w, want_w); ok = false; }
    }
    return ok;
}

// MOUSIKI_RADIO_SIZE=160x40 lets --dump / --selftest render a bigger terminal.
inline void apply_test_size(UiModel& m) {
    if (const char* e = std::getenv("MOUSIKI_RADIO_SIZE")) {
        int c = 0, r = 0;
        if (std::sscanf(e, "%dx%d", &c, &r) == 2) { m.cols = c; m.rows = r; }
    }
}

inline void print_plain(const std::vector<std::string>& frame) {
    for (const auto& l : frame) std::cout << strip_ansi(l) << "\n";
}

} // namespace radio_main_detail
using namespace radio_main_detail;
