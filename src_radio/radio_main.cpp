// mousiki_radio -- radio mode prototype (standalone executable).
//
//   mousiki_radio                 interactive
//   mousiki_radio --dump          render one idle frame as plain text and exit
//   mousiki_radio --dump-menu [q] render the PRESETS menu (optionally with search text q) and exit
//   mousiki_radio --dump-settings [row [col [edit]]] render the RADIO SETTINGS screen and exit
//   mousiki_radio --dump-overlay [1|2] render the main screen with the oscilloscope (1) / loudness (2) overlay and exit
//   mousiki_radio --dump-cheat [n] render the CHEATSHEET (scrolled down n lines) and exit
//   mousiki_radio --dump-browse   render the RADIO BROWSER menu with sample results and exit
//   mousiki_radio --dump-lists [1|2|prompt] render the STATION LISTS menu (tab 1 / tab 2 / unsaved-changes prompt) with sample data and exit
//   mousiki_radio --rb-search [name=.. tags=.. country=.. state=.. language=.. bitrate=..]
//                                 one real Radio Browser search from the command line (connectivity test)
//   mousiki_radio --selftest [s]  headless: tune the offline test signal for s seconds
//                                 (default 6) using the null audio device, then print
//                                 the frame and check its geometry. Needs ffmpeg.
//
// Later this becomes a mode of the main app: App would own a RadioEngine and call
// render_radio_frame() instead of its own panels while radio mode is on. Nothing in
// radio/ depends on App or Player, so that hook is the only coupling.
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

#if defined(_WIN32)
#include "win_compat.h"
#endif

using namespace muisc;
using namespace muisc::radio;
using Clock = std::chrono::steady_clock;

namespace {

std::string strip_ansi(const std::string& s) {
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

size_t count_chars(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// A text field limited in CHARACTERS (the editor limits bytes): a typed character into a full field is refused, a
// paste that is too long keeps what fits. Returns true when the text changed. (Same logic as the preset name box.)
bool edit_limited(std::string& text, EditState& e, int key, size_t max_chars, std::string* status) {
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
static std::string search_text(const std::string& s) { return translit_possible(s) ? with_latin_spelling(s) : s; }

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
bool clear_station_from_presets(std::vector<int>& presets, int idx) {
    bool any = false;
    for (auto& p : presets) if (p == idx) { p = -1; any = true; }
    return any;
}

bool check_geometry(const std::vector<std::string>& frame, const UiModel& m) {
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
void apply_test_size(UiModel& m) {
    if (const char* e = std::getenv("MOUSIKI_RADIO_SIZE")) {
        int c = 0, r = 0;
        if (std::sscanf(e, "%dx%d", &c, &r) == 2) { m.cols = c; m.rows = r; }
    }
}

void print_plain(const std::vector<std::string>& frame) {
    for (const auto& l : frame) std::cout << strip_ansi(l) << "\n";
}

} // namespace

int radio_main(int argc, char** argv) {
    {   const muisc::ModeConfig mk = muisc::read_mode_config(); g_mode_key_next = mk.key_next; g_mode_key_prev = mk.key_prev; }
    // (Windows console set-up and the locale are done once by main() / radio_standalone.cpp.)
    const std::string mode = argc > 1 ? argv[1] : "";
    std::string cfg_source;
    RadioSettings cfg = load_radio_settings(&cfg_source);
    // Still the colours of a built-in theme as an earlier version had them: the theme's fixed colours now (same in every terminal).
    if (cfg_source != "built-in defaults")
        for (const auto& [was, now] : themes::renewed_builtins()) {
            if (!themes::file_holds(radio_config_path(), was, true)) continue;
            theme_apply(cfg, now);
            save_radio_settings(cfg, nullptr);
            break;
        }
    set_emoji_replacement(true);   // station names may carry emoji: always drawn as "?" so the frame cannot shift

    Ui ui;
    ui.model.shuffle = cfg.playback_shuffle;
    std::string source;
    // Where the stations / presets / station lists live: ~/.config/mousiki by default, or what the PATHS tab says.
    StoragePaths storage = resolve_storage_paths(cfg);
    set_storage_files(storage.stations_file.string(), storage.lists_file.string(), storage.presets_file.string());
    auto load_storage = [&]() {
        ui.stations = load_stations(&source);
        ui.model.stations = &ui.stations;
        PresetStore store = load_presets(ui.stations);
        ui.model.banks = std::move(store.banks);
        ui.model.bank_active = std::clamp(store.active, 0, static_cast<int>(ui.model.banks.size()) - 1);
        ui.model.presets = ui.model.banks[static_cast<size_t>(ui.model.bank_active)].slots;
        ui.model.lists = load_station_lists(ui.stations);
    };
    load_storage();
    ui.refilter();
    ui.refilter_menu();
    ui.refilter_presets();
    ui.refilter_lists_stations();
    ui.refilter_saved_lists();
    ui.refilter_main_lists();

    RadioEngine engine;
    // Settings that live in the audio path (mono fold, loudness normalisation): pushed to the engine at start and after every change.
    auto apply_audio = [&]() {
        engine.set_stereo(cfg.stereo);
        engine.set_tune_noise(cfg.tune_noise);
        engine.set_normalization(cfg.normalize, static_cast<float>(cfg.normalize_target_lufs), static_cast<float>(cfg.normalize_max_boost_db));
        engine.set_equalizer(cfg.eq_enabled, cfg.eq_gains);
    };
    apply_audio();
    // Timeshift buffer (pause / rewind): a ring file in the cache folder, sized by the GENERAL tab's "Timeshift buffer".
    const char* ts_home = std::getenv("HOME");
    if (!ts_home || !*ts_home) ts_home = std::getenv("USERPROFILE");
    const std::string timeshift_file = (std::filesystem::path(ts_home && *ts_home ? ts_home : ".") / ".cache" / "mousiki" / "radio_timeshift.pcm").string();
    int timeshift_applied = -1;
    auto apply_timeshift = [&]() {
        if (timeshift_minutes(cfg) == timeshift_applied) return;
        timeshift_applied = timeshift_minutes(cfg);
        engine.set_timeshift(timeshift_applied, timeshift_file);
    };
    apply_timeshift();
    RadioHistory rhist;
    RadioYt yt;
    rhist.open(storage.history_dir);
    yt.set_download_dir(storage.download_dir);

    // ------------------------------------------------------------------ headless modes
    if (mode == "--dump") {
        apply_test_size(ui.model);
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
    }
    if (mode == "--dump-menu") {
        // The PRESETS menu as plain text; an optional second argument is typed into its search box.
        apply_test_size(ui.model);
        // --dump-menu [text] [p|new|rename]: text goes into the station search; "p" puts it into the preset
        // search instead; "new" / "rename" show the name overlay with `text` typed into it.
        ui.model.menu.open = true;
        const std::string how = argc > 3 ? argv[3] : "";
        if (argc > 2 && how == "p") {
            ui.model.menu.psearch = argv[2]; ui.model.menu.target = SearchTarget::Presets; ui.refilter_presets();
        } else if (argc > 2 && (how == "new" || how == "rename")) {
            ui.model.menu.focus = MenuFocus::Presets;
            ui.model.menu.name.open = true;
            ui.model.menu.name.rename = how == "rename";
            ui.model.menu.name.target = ui.model.bank_active;
            ui.model.menu.name.text = argv[2];
        } else if (argc > 2) { ui.model.menu.search = argv[2]; ui.refilter_menu(); }
        ui.model.menu.search_edit.to_end(ui.model.menu.search);
        ui.model.menu.psearch_edit.to_end(ui.model.menu.psearch);
        ui.model.menu.name.edit.to_end(ui.model.menu.name.text);
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
    }
    if (mode == "--dump-lists") {
        // The STATION LISTS menu with sample data (never touches stationlists.txt): tab 1, tab 2 ("2") or the
        // unsaved-changes prompt ("prompt").
        apply_test_size(ui.model);
        const std::string how = argc > 2 ? argv[2] : "";
        ListMenuModel& lm = ui.model.lmenu;
        lm.open = true;
        lm.name = "Morning drive";
        lm.name_edit.to_end(lm.name);
        for (size_t i = 0; i < ui.stations.size() && i < 4; i += 2) lm.items.push_back(static_cast<int>(i));
        lm.dirty = how == "prompt";
        lm.confirm_exit = how == "prompt";
        lm.focus = ListFocus::Stations;
        lm.sort_az = true;
        StationList a; a.name = "Morning drive"; a.items = lm.items;
        StationList b; b.name = "Chill"; b.items = {1, 2};
        StationList c; c.name = "Test signals"; c.items = {4, 5, 6};
        ui.model.lists = {a, b, c};
        if (how == "2") lm.tab = 1;
        ui.refilter_lists_stations();
        ui.refilter_saved_lists();
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
    }
    if (mode == "--dump-stations") {
        // The big STATIONS overlay: --dump-stations [search [add|edit|delete]]
        apply_test_size(ui.model);
        ui.model.stov.open = true;
        if (argc > 2) { ui.model.search = argv[2]; ui.refilter(); }
        const std::string how = argc > 3 ? argv[3] : "";
        if (how == "add") { ui.model.stov.add_open = true; ui.model.stov.add_url = "https://example.org/stream.mp3"; ui.model.stov.add_edit[0].to_end(ui.model.stov.add_url); }
        if (how == "edit" && !ui.stations.empty()) {
            StationsOverlay& so = ui.model.stov;
            const Station& st0 = ui.stations[0];
            so.edit_open = true; so.edit_idx = 0;
            const std::string v[StationsOverlay::kEditFields] = {st0.name, st0.preset_name, st0.url, st0.genre, st0.country, st0.codec_hint, std::to_string(st0.bitrate_hint)};
            for (int f = 0; f < StationsOverlay::kEditFields; ++f) { so.edit_text[f] = v[f]; so.edit_ed[f].to_end(v[f]); }
        }
        if (how == "delete" && !ui.stations.empty()) ui.model.stov.del_idx = 0;
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
    }
    if (mode == "--dump-history") {
        // The LISTENING HISTORY: --dump-history [tab 0-2 [yt]]  (reads the real history folder; "yt" shows the overlay with sample results)
        apply_test_size(ui.model);
        HistoryModel& hm = ui.model.hmenu;
        hm.open = true; hm.hist = &rhist; hm.now = static_cast<long long>(std::time(nullptr));
        hm.tab = std::clamp(argc > 2 ? std::atoi(argv[2]) : 0, 0, 2);
        if (argc > 3 && std::string(argv[3]) == "yt") {
            hm.yt_open = true; hm.yt_text = "Daft Punk One More Time"; hm.yt_edit.to_end(hm.yt_text); hm.yt_in_field = false;
            hm.yt_dir = storage.download_dir.string(); hm.yt.searched = true;
            const char* titles[4] = {"Daft Punk - One More Time (Official Video)", "One More Time", "Daft Punk - One More Time (Live)", "One More Time (Radio Edit)"};
            for (int i = 0; i < 4; ++i) {
                YtItem it; it.video_id = "id" + std::to_string(i); it.title = titles[i]; it.uploader = "Daft Punk"; it.duration_sec = 320.0 - i * 20;
                it.state = i == 1 ? 2 : i == 2 ? 1 : i == 3 ? 3 : 0; it.note = i == 1 ? "One_More_Time.opus" : "yt-dlp not found";
                hm.yt.items.push_back(it);
            }
            hm.yt_cursor = 1;
        }
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
    }
    if (mode == "--dump-settings") {
        // The RADIO SETTINGS screen: --dump-settings [row [col]] puts the cursor there ("edit" as 4th arg opens the cell).
        apply_test_size(ui.model);
        SettingsModel& sm = ui.model.settings;
        sm.open = true;
        const std::string a2 = argc > 2 ? argv[2] : "";
        if (a2 == "onoff" || a2 == "anim" || a2 == "paths" || a2 == "ref" || a2 == "about") {   // --dump-settings onoff|anim|paths|ref|about [row [edit]]
            const bool anim = a2 == "anim", paths = a2 == "paths", ref = a2 == "ref", about = a2 == "about";
            sm.tab = about ? 5 : ref ? 4 : paths ? 3 : anim ? 2 : 1;
            const int maxrow = about ? static_cast<int>(about_app_text().size()) : ref ? kKeyActionCount + 1 : paths ? kPathRowCount : anim ? rs_row_count(2) : kOnOffRowCount;
            sm.row = std::clamp(argc > 3 ? std::atoi(argv[3]) : 0, 0, maxrow - 1);
            if (ref && argc > 4 && std::string(argv[4]) == "edit") { sm.editing = true; sm.buffer = "u"; sm.edit.to_end(sm.buffer); }
            if (paths && argc > 4 && std::string(argv[4]) == "edit") { sm.editing = true; sm.buffer = "/home/me/radio files"; sm.edit.to_end(sm.buffer); }
        } else {
            sm.row = std::clamp(argc > 2 ? std::atoi(argv[2]) : 0, 0, color_tab_row_count() - 1);
            if (a2 == "pick" || a2 == "create") {   // --dump-settings pick|create: the THEME overlays
                sm.theme_list = themes::load_all();
                sm.theme_sel = 2; sm.theme_draft = cfg; sm.theme_base = sm.theme_list[0]; sm.theme_name = "My Theme";
                sm.theme_ui = a2 == "pick" ? 1 : 2; sm.theme_row = argc > 3 ? std::atoi(argv[3]) : -1;
            } else sm.theme_list = themes::load_all();
            sm.col = argc > 3 ? std::atoi(argv[3]) : 0;
            if (argc > 4 && std::string(argv[4]) == "edit") { sm.editing = true; sm.buffer = "21"; }
        }
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
    }
    if (mode == "--dump-overlay") {   // --dump-overlay [1|2]: the main screen with the SHIFT+o / SHIFT+v overlay
        apply_test_size(ui.model);
        ui.model.overlay = argc > 2 ? std::atoi(argv[2]) : 1;
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
    }
    if (mode == "--dump-cheat") {
        apply_test_size(ui.model);
        ui.model.cheat_open = true;
        ui.model.cheat_scroll = argc > 2 ? std::atoi(argv[2]) : 0;
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
    }
    if (mode == "--dump-browse") {
        // The RADIO BROWSER menu with made-up results (no network needed): shows the layout.
        apply_test_size(ui.model);
        BrowseModel& bm = ui.model.browse;
        bm.open = true;
        bm.text[0] = "jazz"; bm.text[1] = "smooth, lounge"; bm.text[2] = "DE"; bm.text[5] = "128-320";
        for (int i = 0; i < kBrowseFields; ++i) bm.edit[i].to_end(bm.text[i]);
        bm.focus = argc > 2 && std::string(argv[2]) == "fields" ? BrowseFocus::Tags : BrowseFocus::Results;
        auto v = std::make_shared<std::vector<BrowseStation>>();
        auto add = [&](const char* name, const char* cc, const char* country, const char* tags, const char* codec, int br, bool on) {
            BrowseStation b; b.uuid = "960e57c5-0601-11e8-ae97-52543be04c81"; b.name = name; b.countrycode = cc; b.country = country;
            b.tags = tags; b.codec = codec; b.bitrate = br; b.online = on; b.language = "german,english"; b.state = "Berlin";
            b.url = "http://example.com/stream.pls"; b.url_resolved = std::string("https://stream.example.com/") + codec + "/" + std::to_string(br);
            b.homepage = "https://www.example.com/radio"; b.votes = 1234; b.clickcount = 87; b.lastchecktime = "2026-10-05 18:16:35";
            v->push_back(b);
        };
        add("Smooth Jazz Lounge Berlin", "DE", "Germany", "jazz,smooth jazz,lounge,easy listening", "MP3", 128, true);
        add("Jazz Radio - The Very Long Name Of A Station That Needs A Marquee To Be Read", "DE", "Germany", "jazz,swing", "AAC", 192, true);
        add("Nightflight Lounge", "DE", "Germany", "lounge,chillout", "MP3", 320, false);
        add("Radio Paradise", "US", "United States", "eclectic,rock", "AAC", 128, true);
        bm.results = v; bm.state = BrowseState::Done; bm.server = "de1.api.radio-browser.info";
        bm.cursor = 1;
        ui.model.t_sec = 3.0;
        RadioStatus fake = engine.status();   // pretend the 4th result is tuned in, to show the highlight and the "Tuned in" line
        fake.state = StreamState::Live; fake.listening_sec = 872; fake.tuned_name = "Radio Paradise";
        fake.tuned_url = "https://stream.example.com/AAC/128"; fake.info.artist = "Twisted Sister"; fake.info.title = "I Wanna Rock";
        auto frame = render_radio_frame(ui.model, fake, engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
    }
    if (mode == "--rb-search") {
        // One real search, results as plain text. key=value arguments: name tags country state language bitrate.
        BrowseQuery q;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            const size_t eq = a.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = a.substr(0, eq), v = a.substr(eq + 1);
            if (k == "name") q.name = v; else if (k == "tags" || k == "tag") q.tags = v; else if (k == "country") q.country = v;
            else if (k == "state") q.state = v; else if (k == "language") q.language = v; else if (k == "bitrate") q.bitrate = v;
        }
        std::vector<std::pair<std::string, std::string>> args;
        std::string err;
        if (!build_browse_args(q, args, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        std::cout << "query:";
        for (const auto& kv : args) std::cout << " " << kv.first << "=" << kv.second;
        std::cout << "\n";
        RadioBrowser rb;
        rb.search(q);
        rb.wait();
        const BrowseSnapshot snap = rb.snapshot();
        if (snap.state != BrowseState::Done) { std::cout << "FAILED: " << snap.message << "\n"; return 3; }
        const size_t n = snap.results ? snap.results->size() : 0;
        std::cout << n << " stations from " << snap.server << "\n";
        for (size_t i = 0; i < n && i < 15; ++i) {
            const BrowseStation& b = (*snap.results)[i];
            std::cout << "  " << b.name << " | " << b.countrycode << " | " << b.bitrate << "k " << b.codec << " | " << b.tags << " | " << b.stream_url() << "\n";
        }
        return 0;
    }
    if (mode == "--selftest") {
        const double secs = argc > 2 ? std::atof(argv[2]) : 6.0;
#if !defined(_WIN32)
        setenv("MOUSIKI_RADIO_NULL", "1", 1);
#endif
        std::string err;
        if (!engine.open_device(&err)) std::fprintf(stderr, "device: %s\n", err.c_str());
        int idx = 0;
        for (size_t i = 0; i < ui.stations.size(); ++i) if (ui.stations[i].synthetic()) { idx = static_cast<int>(i); break; }
        engine.tune(ui.stations[static_cast<size_t>(idx)], idx);
        apply_test_size(ui.model);
        const auto t0 = Clock::now();
        auto last = t0;
        std::vector<std::string> frame;
        StreamState seen_live = StreamState::Idle;
        while (std::chrono::duration<double>(Clock::now() - t0).count() < secs) {
            const auto now = Clock::now();
            ui.model.dt = std::chrono::duration<double>(now - last).count();
            ui.model.t_sec = std::chrono::duration<double>(now - t0).count();
            last = now;
            RadioStatus st = engine.status();
            if (st.state == StreamState::Live) seen_live = StreamState::Live;
            frame = render_radio_frame(ui.model, st, engine, cfg);
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        print_plain(frame);
        RadioStatus st = engine.status();
        std::fprintf(stderr, "state=%d buffer=%.2fs listening=%.1fs reconnects=%d reached_live=%s\n",
                     static_cast<int>(st.state), st.buffer_sec, st.listening_sec, st.reconnects,
                     seen_live == StreamState::Live ? "yes" : "no");
        const bool geo = check_geometry(frame, ui.model);
        engine.stop();
        return (geo && seen_live == StreamState::Live) ? 0 : 2;
    }

    // ------------------------------------------------------------------ interactive
    {
        std::string err;
        engine.set_output_device(cfg.audio_device);   // the chosen sound output (CHOOSE AUDIO DEVICE; "" = the default)
        engine.open_device(&err); // on failure the UI shows the reason and stays usable
    }
    engine.set_volume(70);
    RadioBrowser browser;
    bool yt_jump = false;                 // after a YouTube search from the overlay: move to the results when they arrive
    double notice_until = 0.0;
    bool update_pending = false;   // a newer version was found: the overlay opens when the main screen is showing
    bool scope_win_was = scope_window_running();
    std::vector<std::string> prev_lines;               // the screen's lines as last sent (diff_frame_lines)
    bool out_clear_next = false;                       // the next frame clears the screen (full-screen spectrogram on / off)
    SpectroGfx spectro_gfx;                            // the spectrogram picture (tiles)
    auto last_full_frame = Clock::now();   // the window may have been opened in the player already
    unsigned long scope_win_tick = 0;
    bool spectro_win_was = spectro_window_running();
    // Starts a recording at stream position `from_frame` (frames at 48 kHz; from the record overlay). The file is
    // named after the wall-clock time the recorded audio was on air.
    auto start_record = [&](long long from_frame, double seconds_before_now) {
        const RadioStatus rs = engine.status();
        char stamp[32];
        const std::time_t tt = std::time(nullptr) - static_cast<std::time_t>(seconds_before_now);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tmv);
        std::string err;
        const std::string title = !rs.info.title.empty() ? (rs.info.artist.empty() ? rs.info.title : rs.info.artist + " - " + rs.info.title)
                                                          : (rs.info.station.empty() ? rs.tuned_name : rs.info.station);
        if (engine.start_recording(storage.download_dir.string(), stamp, title, from_frame, &err)) {
            ui.model.notice = "recording to " + (storage.download_dir / (std::string(stamp) + ".mp3")).string() + "   [y] stop";
            notice_until = ui.model.t_sec + 8;
        } else { ui.model.notice = "cannot record: " + err; notice_until = ui.model.t_sec + 6; }
    };
    int record_serial_seen = engine.status().record_serial;
    // Sleep timer: minute choices, the fade-out and the stop. The fade is 10 % of the time, between 30 s and 10 min (15 min -> 90 s,
    // 30 -> 3 min, 60 -> 6 min, 90 -> 9 min, 120 -> 10 min), and the gain follows t^2 (t = time left in the fade / its length): about
    // -12 dB half-way, -20 dB at 30 %, so the first half of the fade is hardly noticed and the last part is a gentle glide to silence.
    static constexpr int kSleepChoice[5] = {15, 30, 60, 90, 120};
    int sleep_minutes = 0;
    Clock::time_point sleep_deadline{};
    double sleep_fade_len = 0.0;
    auto sleep_cancel = [&]() { sleep_minutes = 0; engine.set_fade(1.0f); ui.model.sleep_running = 0; };
    // (MOUSIKI_RADIO_SLEEP_UNIT = seconds per "minute", 60 normally; the tests set it to 1.)
    const double sleep_unit = std::getenv("MOUSIKI_RADIO_SLEEP_UNIT") ? std::max(0.1, std::atof(std::getenv("MOUSIKI_RADIO_SLEEP_UNIT"))) : 60.0;
    auto sleep_start = [&](int minutes) {
        sleep_minutes = minutes;
        sleep_deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(minutes * sleep_unit));
        sleep_fade_len = std::clamp(minutes * 6.0 * sleep_unit / 60.0, 30.0 * sleep_unit / 60.0, 600.0 * sleep_unit / 60.0);
        engine.set_fade(1.0f);
        ui.model.notice = "sleep timer: the stream stops in " + std::to_string(minutes) + " min" + (cfg.sleep_fade ? " (fading out over the last " + std::to_string(static_cast<int>(sleep_fade_len)) + " s)" : "");
        notice_until = ui.model.t_sec + 6;
    };

    TerminalIO term;
    // Terminal graphics for the oscilloscope's image style: ask the terminal once, now that it is in raw mode.
    GfxProto gfx = gfx_probe(cfg.gfx_protocol);
    ui.model.gfx_proto = gfx;
    bool gfx_shown = false;
    bool gfx2_shown = false;   // the second picture of the big oscilloscope overlay (Kitty image id 1)
    auto next_frame = Clock::now();
    unsigned long gfx_tick = 0;
    bool running = true;
    bool switch_mode = false;
    bool last_too_small = false;
    int last_rows = 0, last_cols = 0;
    const auto t0 = Clock::now();
    auto last = t0;

    // Every tune goes through tune_to(): it remembers the station that was playing, so `b` can go back to
    // "the channel that was played before" in shuffle mode. `b` itself tunes with record = false (it pops
    // the history instead of growing it).
    std::vector<int> history;
    auto tune_to = [&](int idx, bool record = true) {
        if (idx < 0 || idx >= static_cast<int>(ui.stations.size())) return;
        const int cur = engine.status().tuned_index;
        if (record && cur >= 0 && cur != idx) {
            history.push_back(cur);
            if (history.size() > 100) history.erase(history.begin());
        }
        engine.tune(ui.stations[static_cast<size_t>(idx)], idx);
    };
    auto tune_visible = [&](int vis_pos) {
        if (vis_pos < 0 || vis_pos >= static_cast<int>(ui.model.visible.size())) return;
        tune_to(ui.model.visible[static_cast<size_t>(vis_pos)]);
    };
    // The channels `n` / `b` / `#` surf through: the stations of the station list shown in the STATIONS pane
    // (in the list's own order), or all stations when no list is shown.
    auto surf_pool = [&]() {
        std::vector<int> pool;
        const int total = static_cast<int>(ui.stations.size());
        if (const StationList* al = ui.active_list_ptr()) {
            for (int i : al->items) if (i >= 0 && i < total) pool.push_back(i);
        } else {
            for (int i = 0; i < total; ++i) pool.push_back(i);
        }
        return pool;
    };
    auto tune_relative = [&](int delta) {
        const std::vector<int> pool = surf_pool();
        const int n = static_cast<int>(pool.size());
        if (n == 0) return;
        const int tuned = engine.status().tuned_index;
        int pos = -1;
        for (int i = 0; i < n; ++i) if (pool[static_cast<size_t>(i)] == tuned) { pos = i; break; }
        if (pos < 0) pos = delta > 0 ? -1 : 0;     // not in the pool: n starts at its first, b at its last channel
        tune_to(pool[static_cast<size_t>(((pos + delta) % n + n) % n)]);
    };
    // '#': shuffle next -- always a random station (never the one already tuned), whatever the S/L mode is.
    std::mt19937 rng{std::random_device{}()};
    auto tune_shuffle = [&]() {
        const std::vector<int> pool = surf_pool();
        const int n = static_cast<int>(pool.size());
        if (n == 0) return;
        if (n == 1) { tune_to(pool[0]); return; }
        const int cur = engine.status().tuned_index;
        std::vector<int> cand;
        for (int i : pool) if (i != cur) cand.push_back(i);
        if (cand.empty()) return;
        tune_to(cand[static_cast<size_t>(std::uniform_int_distribution<int>(0, static_cast<int>(cand.size()) - 1)(rng))]);
    };
    // 'b': the only key that follows the S/L mode. S: the channel that was played before (history, repeatable).
    // L: the channel before the current one in the list.
    auto tune_back = [&]() {
        if (!ui.model.shuffle) { tune_relative(-1); return; }
        const int cur = engine.status().tuned_index;
        while (!history.empty()) {
            const int prev = history.back();
            history.pop_back();
            if (prev != cur) { tune_to(prev, false); return; }
        }
    };
    // Remote control (`mousiki --cmd next` ... see src/remote_control.h): the same actions as the keys, without touching the UI state.
    auto remote_apply = [&](const muisc::remote::Command& c) {
        const RadioStatus rs = engine.status();
        const bool tuned = rs.state != StreamState::Idle && rs.state != StreamState::Failed;
        const std::string& n = c.name;
        if (n == "next") tune_relative(+1);
        else if (n == "prev") tune_back();
        else if (n == "shuffle") tune_shuffle();
        else if (n == "play-pause") {
            if (!tuned) tune_visible(ui.model.cursor);
            else engine.toggle_pause();
        }
        else if (n == "play") {
            if (!tuned) tune_visible(ui.model.cursor);
            else if (rs.timeshift_paused) engine.toggle_pause();
            else if (rs.muted) engine.set_muted(false);
        }
        else if (n == "pause") { if (tuned && !rs.timeshift_paused) engine.toggle_pause(); }
        else if (n == "stop") engine.stop();
        else if (n == "volume-up") engine.set_volume(rs.volume + static_cast<int>(muisc::remote::number(c.arg, 5)));
        else if (n == "volume-down") engine.set_volume(rs.volume - static_cast<int>(muisc::remote::number(c.arg, 5)));
        else if (n == "volume") {
            const int v = static_cast<int>(muisc::remote::number(c.arg, rs.volume));
            engine.set_volume((!c.arg.empty() && (c.arg[0] == '+' || c.arg[0] == '-')) ? rs.volume + v : v);
        }
        else if (n == "mute") engine.set_muted(true);
        else if (n == "unmute") engine.set_muted(false);
        else if (n == "toggle-mute") engine.set_muted(!rs.muted);
        else if (n == "seek") { if (tuned) engine.jump(muisc::remote::number(c.arg, 0)); }
        else if (n == "live") { if (tuned) engine.go_live(); }
        else if (n == "playback-mode") { ui.model.shuffle = !ui.model.shuffle; cfg.playback_shuffle = ui.model.shuffle; ui.model.settings.dirty = true; }
        else if (n == "switch-mode") {
            const int dir = muisc::remote::number(c.arg, 1) < 0 ? -1 : 1;   // (-1: the previous mode)
            if (muisc::next_mode_in(muisc::read_mode_config(), 1, dir) != 1) { g_mode_switch_dir = dir; switch_mode = true; running = false; }
        }
        else if (n == "quit") running = false;
        // the phone's STATIONS list: open / close it, page through it, tune the tapped station
        else if (n == "remote-view") { ui.model.phone_view = c.arg == "1" ? 1 : 0; ui.model.phone_page = 0; ui.model.phone_note.clear(); }
        else if (n == "remote-scroll") {
            int pc = 0, pr = 0;
            muisc::remote::net::screen_wanted(pc, pr);
            const int per = muisc::remote::net::phone_list_rows(pr > 0 ? pr : 30);
            const int count = ui.stations.empty() ? 0 : static_cast<int>(ui.stations.size());
            const int pages = std::max(1, (count + per - 1) / per);
            ui.model.phone_page = std::clamp(ui.model.phone_page + static_cast<int>(muisc::remote::number(c.arg, 1)), 0, pages - 1);
        }
        else if (n == "tune") {
            const int k = static_cast<int>(muisc::remote::number(c.arg, 0)) - 1;
            if (k >= 0 && k < static_cast<int>(ui.stations.size())) {
                tune_to(k);
                ui.model.phone_note = "tuned " + ui.stations[static_cast<size_t>(k)].name;
            } else ui.model.phone_note = "no station " + c.arg;
        }
    };
    auto set_search_focus = [&](bool on) {
        ui.model.search_focus = on;
        if (on) ui.model.search_edit.to_end(ui.model.search);   // caret behind the text, nothing marked
        set_text_entry(on);
    };

    // ---- the big STATIONS overlay (key L): add by URL (a), preset name (SHIFT+c) -------------------------------
    auto stov_add_commit = [&]() {
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
    };
    // Deleting / editing a station changes the station list under everything that holds station INDICES (presets, station lists, the
    // history of tuned stations, the list editor): each change is one undo step (SHIFT+u) with a copy of all of them.
    struct StationUndo {
        std::vector<Station> stations;
        std::vector<PresetBank> banks;
        std::vector<int> presets;
        int bank_active = 0;
        std::vector<StationList> lists;
        std::vector<int> history;
        std::vector<int> lm_items;
        std::string what;
    };
    std::vector<StationUndo> stov_undo;
    auto stov_snapshot = [&](const std::string& what) {
        ui.sync_active();
        stov_undo.push_back({ui.stations, ui.model.banks, ui.model.presets, ui.model.bank_active, ui.model.lists, history, ui.model.lmenu.items, what});
        if (stov_undo.size() > 20) stov_undo.erase(stov_undo.begin());
        ui.model.stov.undo_count = static_cast<int>(stov_undo.size());
    };
    // The stream that plays goes on whatever happens to the list: only its index is looked up again (by its URL; -1 = not in the list).
    auto stov_resync_tuned = [&]() {
        const RadioStatus rs = engine.status();
        if (rs.tuned_url.empty()) return;
        int found = -1;
        for (size_t i = 0; i < ui.stations.size(); ++i) if (same_stream_url(ui.stations[i].url, rs.tuned_url)) { found = static_cast<int>(i); break; }
        if (found != rs.tuned_index) engine.set_tuned_index(found);
    };
    auto stov_refresh = [&]() {
        ui.refilter(); ui.refilter_menu(); ui.refilter_presets(); ui.refilter_lists_stations(); ui.refilter_saved_lists(); ui.refilter_main_lists();
        UiModel& um = ui.model;
        um.cursor = std::clamp(um.cursor, 0, std::max(0, static_cast<int>(um.visible.size()) - 1));
    };
    // Writes everything a change touches; returns "" or what could not be written.
    auto stov_persist_all = [&]() -> std::string {
        std::string err, bad;
        if (!save_all_stations(ui.stations, nullptr, &err)) bad = err;
        else if (!save_preset_names(ui.stations)) bad = "cannot write " + preset_names_path();
        else if (!ui.persist()) bad = "cannot write " + presets_path();
        else if (!save_station_lists(ui.stations, ui.model.lists)) bad = "cannot write " + station_lists_path();
        return bad;
    };
    auto stov_delete = [&](int idx) {
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
    };
    auto stov_undo_do = [&]() {
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
    };
    auto stov_edit_open = [&](int idx) {
        StationsOverlay& so = ui.model.stov;
        const Station& st0 = ui.stations[static_cast<size_t>(idx)];
        so.edit_open = true; so.edit_idx = idx; so.edit_field = 0; so.edit_error.clear(); so.flash.clear();
        const std::string v[StationsOverlay::kEditFields] = {st0.name, st0.preset_name, st0.url, st0.genre, st0.country, st0.codec_hint, st0.bitrate_hint > 0 ? std::to_string(st0.bitrate_hint) : std::string("0")};
        for (int f = 0; f < StationsOverlay::kEditFields; ++f) { so.edit_text[f] = v[f]; so.edit_ed[f].to_end(v[f]); }
        set_text_entry(true);
    };
    auto stov_edit_commit = [&]() {
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
            kbps = std::min(99999, std::atoi(kbps_s.c_str()));
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
    };

    // ---- RADIO BROWSER menu (Shift+S) ----------------------------------------------------------------
    unsigned browse_state_seen = 0;           // last BrowseState we reacted to (as int)
    unsigned browse_gen_seen = 0;
    bool browse_jump_to_results = false;      // ENTER in a pane: when the results arrive, move to RESULTS
    auto browse_focus = [&](BrowseFocus f) {
        BrowseModel& bm = ui.model.browse;
        bm.focus = f;
        if (f != BrowseFocus::Results) bm.edit[static_cast<int>(f)].to_end(bm.text[static_cast<int>(f)]);
        set_text_entry(f != BrowseFocus::Results);
    };
    auto browse_open = [&]() {
        BrowseModel& bm = ui.model.browse;
        bm.open = true; bm.flash.clear();
        browse_focus(BrowseFocus::Name);
    };
    // ------------------------------------------------------------------ RADIO SETTINGS (s)
    struct KeyUndo { std::map<std::string, std::string> keys; int row; };
    std::vector<KeyUndo> key_undo;
    RadioSettings settings_snapshot;
    bool settings_snapshot_valid = false;
    auto settings_save = [&]() {
        SettingsModel& sm = ui.model.settings;
        std::string err;
        if (save_radio_settings(cfg, &err)) { sm.status = "Saved " + radio_config_path().string(); sm.dirty = false; }
        else sm.status = "Could not save: " + err;
    };
    // The first time the help wizard is closed it stops opening by itself.
    // Written right away rather than at exit, so a crash cannot bring the
    // welcome back on the next start.
    auto wizard_seen_save = [&]() {
        if (cfg.wizard_seen) return;
        cfg.wizard_seen = true;
        save_radio_settings(cfg, nullptr);
    };
    auto wizard_open_now = [&]() {
        if (!wizard::kEnabled) return;
        wizard::open(ui.model.wizard, !cfg.wizard_seen);
        ui.model.wizard_open = true;
    };
    // A PATHS value changed: switch the storage files (copying what is missing there), reload what lives in them, move the
    // history store and tell the downloader about the new folder.
    auto storage_changed = [&]() {
        SettingsModel& sm = ui.model.settings;
        const StoragePaths np = resolve_storage_paths(cfg);
        std::string msg;
        if (np.stations_file != storage.stations_file || np.lists_file != storage.lists_file || np.presets_file != storage.presets_file) {
            engine.stop();   // the station numbers change with the files
            set_storage_files(np.stations_file.string(), np.lists_file.string(), np.presets_file.string());
            load_storage();
            ui.model.search_lists = false; ui.model.active_list.clear();
            ui.refilter(); ui.refilter_menu(); ui.refilter_presets(); ui.refilter_lists_stations(); ui.refilter_saved_lists(); ui.refilter_main_lists();
            msg = "Stations, presets and lists now come from the new folders (stopped playback). ";
        }
        if (np.history_dir != storage.history_dir) {
            rhist.close_live();
            rhist.open(np.history_dir);
            msg += "History moved. ";
        }
        yt.set_download_dir(np.download_dir);
        storage = np;
        sm.status = msg.empty() ? "" : msg;
    };
    auto theme_dir_path = [](std::string dir) {
        if (!dir.empty() && dir[0] == '~' && (dir.size() == 1 || dir[1] == '/' || dir[1] == '\\')) {
            const char* home = std::getenv("HOME");
            if (!home || !*home) home = std::getenv("USERPROFILE");
            if (home) dir = std::string(home) + dir.substr(1);
        }
        return std::filesystem::path(dir);
    };
    auto theme_io_rescan = [&]() {
        SettingsModel& sm = ui.model.settings;
        sm.theme_io_files = themes::list_theme_files(theme_dir_path(sm.theme_io_dir));
        sm.theme_io_sel = std::clamp(sm.theme_io_sel, 0, std::max(0, static_cast<int>(sm.theme_io_files.size()) - 1));
    };
    auto settings_open = [&]() {
        SettingsModel& sm = ui.model.settings;
        settings_snapshot = cfg; settings_snapshot_valid = true;
        sm.open = true; sm.editing = false; sm.status.clear();
        sm.tab = 0; sm.row = 0; sm.col = 0;   // like the music player: always start on the first tab
        sm.row = std::clamp(sm.row, 0, kColorRowCount - 1);
        sm.theme_ui = 0;
        sm.theme_list = themes::load_all();
    };
    // [S] saves and leaves the menu; ESC / Q leave WITHOUT saving: what was changed meanwhile is taken back
    // (the settings and the key undo stack as they were when the menu opened).
    auto settings_close = [&](bool save) {
        SettingsModel& sm = ui.model.settings;
        if (save) { settings_save(); }
        else if (sm.dirty && settings_snapshot_valid) {
            cfg = settings_snapshot;
            apply_audio();
            storage_changed();
            sm.dirty = false;
        }
        settings_snapshot_valid = false;
        key_undo.clear();
        sm.open = false; sm.editing = false; sm.theme_ui = 0; sm.remote_ov = muisc::remote_ui::State{};
        set_text_entry(false);
    };
    // CHOOSE AUDIO DEVICE (GENERAL -> AUDIO [ENTER], or its key on the main screen: overlay 9). The list is read when it opens.
    auto adev_open = [&](bool from_settings) {
        SettingsModel& sm = ui.model.settings;
        sm.adev_err.clear();
        sm.adev_list = muisc::list_output_devices(&sm.adev_err);
        sm.adev_active = engine.active_device();
        sm.adev_sel = 0;
        for (size_t i = 0; i < sm.adev_list.size(); ++i)
            if (!cfg.audio_device.empty() && sm.adev_list[i].name == cfg.audio_device) sm.adev_sel = static_cast<int>(i) + 1;
        sm.adev_open = true;
        sm.adev_from_settings = from_settings;
        if (!from_settings) { ui.model.overlay = 9; ui.model.overlay_row = 0; }
    };
    auto adev_close = [&]() {
        SettingsModel& sm = ui.model.settings;
        sm.adev_open = false;
        if (!sm.adev_from_settings) ui.model.overlay = 0;
    };
    // The pick is written into config.txt at once (one choice for all modes) and the stream moves to that device now.
    auto adev_pick = [&]() {
        SettingsModel& sm = ui.model.settings;
        const std::string pick = sm.adev_sel > 0 && sm.adev_sel <= static_cast<int>(sm.adev_list.size()) ? sm.adev_list[static_cast<size_t>(sm.adev_sel - 1)].name : std::string();
        cfg.audio_device = pick;
        if (settings_snapshot_valid) settings_snapshot.audio_device = pick;   // ESC in the settings does not take it back
        muisc::set_config_value("AudioDevice", pick);   // the player's config.txt (one choice for all modes)
        std::string err;
        engine.set_output_device(pick, &err);
        const std::string now = engine.active_device();
        std::string msg = "audio device: " + (pick.empty() ? std::string("System Default") : pick);
        if (!pick.empty() && !now.empty() && now != pick) msg += " -- could not be opened, the default device plays";
        if (!err.empty()) msg = "audio device: " + err;
        if (sm.adev_from_settings) sm.status = msg;
        else { ui.model.notice = msg; notice_until = ui.model.t_sec + 6; }
    };
    // Has anything been changed since the settings screen opened? (the same text that [s] would write)
    auto settings_changed = [&]() { return settings_snapshot_valid && radio_settings_text(cfg) != radio_settings_text(settings_snapshot); };
    // The CREATE THEME overlay. edit_current ([e]): it opens on the colours in use; if they are a theme of your own its name is
    // already in the NAME field and saving overwrites it (after a question), a built-in theme needs a new name.
    auto theme_open_creator = [&](bool edit_current) {
        SettingsModel& sm = ui.model.settings;
        sm.theme_list = themes::load_all();
        const int cur = theme_index(cfg, sm.theme_list);
        sm.theme_base = sm.theme_list[static_cast<size_t>(cur >= 0 ? cur : 0)];
        sm.theme_draft = cfg; sm.theme_name.clear(); sm.theme_row = -1; sm.theme_col = 0; sm.theme_ui = 2;
        sm.status.clear();
        if (edit_current) {
            if (cur >= 0 && !sm.theme_list[static_cast<size_t>(cur)].builtin) sm.theme_name = sm.theme_list[static_cast<size_t>(cur)].name;
            else sm.status = cur >= 0 ? "\"" + sm.theme_list[static_cast<size_t>(cur)].name + "\" is built in and cannot be overwritten: give your edit a new name"
                                      : "these colors are not a saved theme: give them a name";
        }
        sm.theme_name_initial = sm.theme_name;
        sm.theme_initial_kv = theme_capture(sm.theme_draft, sm.theme_base).kv;
        sm.leave_prompt = 0;
    };
    auto theme_changed = [&]() {
        const SettingsModel& sm = ui.model.settings;
        return sm.theme_name != sm.theme_name_initial || theme_capture(sm.theme_draft, sm.theme_base).kv != sm.theme_initial_kv;
    };
    auto theme_save_now = [&]() {
        SettingsModel& sm = ui.model.settings;
        themes::Theme t = theme_capture(sm.theme_draft, sm.theme_base);
        t.name = themes::sanitize_name(sm.theme_name);
        std::string err;
        if (!themes::save_theme(t, err)) { sm.status = err; return; }
        sm.theme_list = themes::load_all();
        theme_apply(cfg, t);
        sm.dirty = true; sm.theme_ui = 0;
        sm.status = "THEME \"" + t.name + "\" SAVED AND APPLIED (s saves the settings)";
    };
    // [s] in the creator: a name is needed, a built-in theme cannot be overwritten, overwriting a theme of your own is asked first.
    auto theme_try_save = [&]() {
        SettingsModel& sm = ui.model.settings;
        const std::string name = themes::sanitize_name(sm.theme_name);
        if (name.empty()) { sm.status = "give the theme a name first (NAME row), then [s]"; return; }
        const int at = themes::find_name(sm.theme_list, name);
        if (at >= 0) {
            if (sm.theme_list[static_cast<size_t>(at)].builtin) { sm.status = "\"" + sm.theme_list[static_cast<size_t>(at)].name + "\" is a built-in theme and cannot be overwritten -- pick another name"; return; }
            sm.leave_prompt = 3;
            return;
        }
        theme_save_now();
    };
    auto browse_close = [&]() { ui.model.browse.open = false; set_text_entry(false); ui.model.browse.flash.clear(); };
    auto browse_search = [&]() {
        BrowseModel& bm = ui.model.browse;
        BrowseQuery q;
        q.name = bm.text[0]; q.tags = bm.text[1]; q.country = bm.text[2];
        q.state = bm.text[3]; q.language = bm.text[4]; q.bitrate = bm.text[5];
        bm.flash.clear();
        bm.cursor = 0;
        browse_jump_to_results = true;
        browser.search(q);
    };
    // The hovered result as a station of the main list (`a`): appended to the end -- so every preset slot, which
    // holds a station INDEX, stays valid -- and written to stations.txt.
    auto browse_add = [&](const BrowseStation& b) {
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
    };
    // ENTER in RESULTS: tune it. A stream that is already in the list tunes the list entry (so the list, the band and
    // the presets light up); anything else is tuned directly (index -1).
    auto browse_tune = [&](const BrowseStation& b) {
        const Station st = browse_to_station(b);
        int idx = -1;
        for (size_t i = 0; i < ui.stations.size(); ++i) if (same_stream_url(ui.stations[i].url, st.url)) { idx = static_cast<int>(i); break; }
        if (idx >= 0) tune_to(idx); else engine.tune(st, -1);
        browser.count_click(b.uuid);   // Radio Browser asks for one click per started stream
        ui.model.browse.flash = "Tuning \"" + st.name + "\"";
    };

    // ---- STATION LISTS menu (Shift+K) -- behaves like the player's playlist editor ----------------------
    // Tab tells which text field (if any) owns the keyboard; the terminal's text-entry mode follows it.
    auto n_stations = [](size_t n) { return std::to_string(n) + (n == 1 ? " station" : " stations"); };
    auto lm_in_text = [&]() {
        const ListMenuModel& lm = ui.model.lmenu;
        if (!lm.open || lm.confirm_exit || lm.confirm_delete) return false;
        if (lm.tab == 0) return lm.focus == ListFocus::Name || lm.focus == ListFocus::Search;
        return lm.mfocus == ListManageFocus::Search;
    };
    auto lm_sync_entry = [&]() { set_text_entry(lm_in_text()); };
    auto lm_open = [&]() {
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
    };
    auto lm_close = [&]() {
        ListMenuModel& lm = ui.model.lmenu;
        lm.open = false; lm.confirm_exit = false; lm.confirm_delete = false; lm.flash.clear();
        set_text_entry(false);
        ui.refilter(); ui.refilter_main_lists();   // a list shown in the STATIONS pane may have changed or be gone
    };
    auto lm_focus = [&](ListFocus f) {
        ListMenuModel& lm = ui.model.lmenu;
        lm.focus = f;
        lm.typing = false;
        if (f == ListFocus::Name) lm.name_edit.to_end(lm.name);
        if (f == ListFocus::Search) lm.search_edit.to_end(lm.search);
        lm_sync_entry();
    };
    auto lm_mfocus = [&](ListManageFocus f) {
        ListMenuModel& lm = ui.model.lmenu;
        lm.mfocus = f;
        lm.typing = false;
        if (f == ListManageFocus::Search) lm.msearch_edit.to_end(lm.msearch);
        lm_sync_entry();
    };
    // CTRL+s. Saves under the typed name and STAYS in the menu (like the player); only the "Save changes ... before
    // exiting?" prompt saves AND leaves. An empty name is refused and focuses the name field.
    auto lm_save = [&](bool leave) {
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
    };
    // Tab 2, ENTER: the hovered saved list moves into the editor (tab 1).
    auto lm_load = [&]() {
        ListMenuModel& lm = ui.model.lmenu;
        if (lm.mcursor < 0 || lm.mcursor >= static_cast<int>(lm.mvisible.size())) return;
        const StationList& l = ui.model.lists[static_cast<size_t>(lm.mvisible[static_cast<size_t>(lm.mcursor)])];
        lm.name = l.name; lm.name_edit.to_end(lm.name);
        lm.items = l.items; lm.item_cursor = 0;
        lm.dirty = false;    // freshly loaded from disk: nothing to lose yet
        lm.tab = 0;
        lm.flash = "editing \"" + l.name + "\" (" + n_stations(l.items.size()) + ")";
        lm_focus(ListFocus::Search);
    };
    auto lm_delete = [&]() {
        ListMenuModel& lm = ui.model.lmenu;
        if (lm.mcursor < 0 || lm.mcursor >= static_cast<int>(lm.mvisible.size())) return;
        const size_t li = static_cast<size_t>(lm.mvisible[static_cast<size_t>(lm.mcursor)]);
        const std::string nm = ui.model.lists[li].name;
        ui.model.lists.erase(ui.model.lists.begin() + static_cast<std::ptrdiff_t>(li));
        ui.refilter_saved_lists();
        lm.flash = save_station_lists(ui.stations, ui.model.lists) ? "deleted list \"" + nm + "\""
                                                                    : "deleted \"" + nm + "\" -- but could not write " + station_lists_path();
    };
    // ENTER in SEARCH / STATIONS: the hovered station goes to the end of the list (a station sits in a list once).
    auto lm_add = [&]() {
        ListMenuModel& lm = ui.model.lmenu;
        if (lm.cursor < 0 || lm.cursor >= static_cast<int>(lm.visible.size())) return;
        const int idx = lm.visible[static_cast<size_t>(lm.cursor)];
        const std::string& nm = ui.stations[static_cast<size_t>(idx)].name;
        if (std::find(lm.items.begin(), lm.items.end(), idx) != lm.items.end()) { lm.flash = "\"" + nm + "\" is already in this list"; return; }
        lm.items.push_back(idx);
        lm.dirty = true;
        lm.flash = "added \"" + nm + "\" (" + n_stations(lm.items.size()) + ")";
    };
    auto lm_remove = [&]() {
        ListMenuModel& lm = ui.model.lmenu;
        if (lm.item_cursor < 0 || lm.item_cursor >= static_cast<int>(lm.items.size())) return;
        const std::string nm = ui.stations[static_cast<size_t>(lm.items[static_cast<size_t>(lm.item_cursor)])].name;
        lm.items.erase(lm.items.begin() + lm.item_cursor);
        lm.item_cursor = std::clamp(lm.item_cursor, 0, std::max(0, static_cast<int>(lm.items.size()) - 1));
        lm.dirty = true;
        lm.flash = "removed \"" + nm + "\" (" + n_stations(lm.items.size()) + ")";
    };
    auto lm_move = [&](int dir) {
        ListMenuModel& lm = ui.model.lmenu;
        const int target = lm.item_cursor + dir;
        if (lm.item_cursor < 0 || lm.item_cursor >= static_cast<int>(lm.items.size()) || target < 0 || target >= static_cast<int>(lm.items.size())) return;
        std::swap(lm.items[static_cast<size_t>(lm.item_cursor)], lm.items[static_cast<size_t>(target)]);
        lm.item_cursor = target;
        lm.dirty = true;
    };

    // REFERENCE tab: the last 5 key changes can be undone with Ctrl+Shift+U. An entry is the key map from before the
    // change and the action it concerned (-1 = all keys were reset); undoing it puts the cursor back on that row.
    auto key_change_begin = [&](int row) {
        key_undo.push_back({cfg.keys, row});
        if (key_undo.size() > 5) key_undo.erase(key_undo.begin());
    };
    auto key_change_end = [&](bool changed) { if (!changed && !key_undo.empty()) key_undo.pop_back(); };

    // SHIFT+9 / SHIFT+8: the oscilloscope / the spectrogram in their own windows (graphics card, needs SDL2). They work
    // whatever is switched off on the GENERAL tab, and also while an overlay is open.
    auto window_key_label = [&](const char* id) {
        for (int i = 0; i < kKeyActionCount; ++i)
            if (std::string(kKeyActions[i].id) == id) return muisc::symbol_key_label(key_binding(cfg, i));
        return std::string();
    };
    auto toggle_scope_window = [&]() {
        if (scope_window_running()) {
            scope_window_close();
            scope_win_was = false;
            ui.model.notice = "scope window closed"; notice_until = ui.model.t_sec + 3;
            return;
        }
        ScopeWinConfig c;
        fill_scope_window_config(cfg, engine.status(), c);
        scope_window_set_config(c);   // the settings are there before the first sample
        std::string err;
        if (scope_window_open(&err)) {
            scope_win_was = true;
            ui.model.notice = "scope window opened: F / double-click fullscreen, T always on top, ESC or " +
                              window_key_label("ScopeWindow") + " closes";
            notice_until = ui.model.t_sec + 6;
        } else { ui.model.notice = err; notice_until = ui.model.t_sec + 10; }
    };
    auto toggle_spectro_window = [&]() {
        if (spectro_window_running()) {
            spectro_window_close();
            spectro_win_was = false;
            ui.model.notice = "spectrogram window closed"; notice_until = ui.model.t_sec + 3;
            return;
        }
        SpectroWinConfig c;
        fill_spectro_window_config(cfg, engine.status(), c);
        spectro_window_set_config(c);
        std::string err;
        if (spectro_window_open(&err)) {
            spectro_win_was = true;
            ui.model.notice = "spectrogram window opened: F / double-click fullscreen, T always on top, ESC or " +
                              window_key_label("SpectroWindow") + " closes";
            notice_until = ui.model.t_sec + 6;
        } else { ui.model.notice = err; notice_until = ui.model.t_sec + 10; }
    };
    // First start of the radio after an install: greet with the help wizard,
    // the same way the music player does. Closing it writes WizardSeen=true.
    if (wizard::kEnabled && !cfg.wizard_seen) wizard_open_now();
    muisc::remote::start("radio");
    while (running) {
        muisc::remote::tick("radio");
        for (const muisc::remote::Command& rc : muisc::remote::poll()) {
            if (rc.name != "wizard") { remote_apply(rc); continue; }
            if (!ui.model.wizard_open) wizard_open_now();   // SHIFT+w from the remote: open / close the help wizard
            else { ui.model.wizard_open = false; out_clear_next = true; wizard_seen_save(); }
        }
        // the remote app's web server follows the Remote App switch (cheap when nothing changed)
        muisc::remote::net::configure({cfg.remote_app, cfg.remote_port, cfg.remote_pin});
        {   // ... and its phone screen, when a phone asks for it (at most ~14 a second)
            static std::chrono::steady_clock::time_point phone_built{};
            int pc = 0, pr = 0;
            const auto pnow = std::chrono::steady_clock::now();
            if (muisc::remote::net::screen_wanted(pc, pr) && pnow - phone_built >= std::chrono::milliseconds(70)) {
                phone_built = pnow;
                std::vector<muisc::remote::net::Zone> zones;
                const std::vector<std::string> lines = build_phone_screen(ui.model, engine.status(), cfg, pc, pr, zones);
                muisc::remote::net::set_screen("radio", pc, pr, lines, zones, cfg.list_fg);
            }
        }
        {   // "a newer version is out": once, as a small overlay once the main screen is showing
            muisc::update_check::Notice n;
            if (!update_pending && muisc::update_check::take(n)) {
                ui.model.update_latest = n.latest; ui.model.update_current = n.current;
                ui.model.update_url = muisc::update_check::release_url(n);
                update_pending = true;
            }
            const UiModel& um = ui.model;
            const bool main_screen = um.overlay == 0 && !um.cheat_open && !um.wizard_open && !um.scope_big && !um.spectro_full &&
                                     !um.menu.open && !um.browse.open && !um.settings.open && !um.lmenu.open && !um.hmenu.open && !um.stov.open;
            if (update_pending && main_screen) { update_pending = false; ui.model.overlay = 8; }
        }
        {   // what `mousiki --status` prints
            const RadioStatus rs = engine.status();
            const char* st = rs.state == StreamState::Idle ? "stopped" : rs.state == StreamState::Failed ? "failed"
                           : rs.timeshift_paused ? "paused" : rs.muted ? "muted" : "playing";
            muisc::remote::publish_status({{"mode", "radio"}, {"state", st}, {"station", rs.tuned_name.empty() ? rs.info.station : rs.tuned_name},
                                           {"artist", rs.info.artist}, {"title", rs.info.title}, {"volume", std::to_string(rs.volume)},
                                           {"muted", rs.muted ? "yes" : "no"}, {"behind_live_sec", std::to_string(static_cast<int>(rs.behind_sec))}});
        }
        // search results arrive on a worker thread: copy the newest snapshot into the model, once per frame
        {
            const BrowseSnapshot snap = browser.snapshot();
            BrowseModel& bm = ui.model.browse;
            bm.results = snap.results; bm.state = snap.state; bm.message = snap.message; bm.server = snap.server;
            const unsigned st_now = static_cast<unsigned>(snap.state);
            if (snap.generation != browse_gen_seen) { browse_gen_seen = snap.generation; bm.cursor = 0; }
            if (st_now != browse_state_seen) {
                browse_state_seen = st_now;
                if (snap.state == BrowseState::Done && bm.open && browse_jump_to_results && snap.results && !snap.results->empty())
                    browse_focus(BrowseFocus::Results);
                if (snap.state != BrowseState::Searching) browse_jump_to_results = false;
            }
            const int nres = snap.results ? static_cast<int>(snap.results->size()) : 0;
            bm.cursor = std::clamp(bm.cursor, 0, std::max(0, nres - 1));
        }
        if (sleep_minutes > 0) {                                           // sleep timer: fade, then stop
            const double left = std::chrono::duration<double>(sleep_deadline - Clock::now()).count();
            ui.model.sleep_running = sleep_minutes; ui.model.sleep_left = left;
            if (left <= 0.0) {
                engine.stop(); sleep_cancel();
                ui.model.notice = "sleep timer: stream stopped"; notice_until = ui.model.t_sec + 10;
            } else if (cfg.sleep_fade && left < sleep_fade_len) {
                const double t = left / sleep_fade_len;
                engine.set_fade(static_cast<float>(t * t));
            } else engine.set_fade(1.0f);
        }
        apply_timeshift();   // the buffer size changed in the settings (or was put back by discarding them)
        engine.set_scope_raw(cfg.osci().music);   // oscilloscope music mode: the scopes get the unprocessed stream
        set_marquee_enabled(cfg.marquee);   // GENERAL "Marquee scrolling"
        radio_enforce_visuals(cfg);   // GENERAL "Use oscilloscope / spectrogram": a switched-off one never stays
        {   // the spectrogram window (SHIFT+8): the same
            const bool running_sp = spectro_window_running();
            if (running_sp && (scope_win_tick % 6) == 3) {
                SpectroWinConfig c;
                fill_spectro_window_config(cfg, engine.status(), c);
                spectro_window_set_config(c);
            }
            if (spectro_win_was && !running_sp) {
                const std::string e = spectro_window_take_error();
                ui.model.notice = e.empty() ? "spectrogram window closed" : e;
                notice_until = ui.model.t_sec + (e.empty() ? 3 : 10);
            }
            spectro_win_was = running_sp;
        }
        {   // the scope window (SHIFT+9): its settings / title, and a window closed from its own side
            const bool running = scope_window_running();
            if (running && (++scope_win_tick % 6) == 0) {
                ScopeWinConfig c;
                fill_scope_window_config(cfg, engine.status(), c);
                scope_window_set_config(c);
            }
            if (scope_win_was && !running) {
                const std::string e = scope_window_take_error();
                ui.model.notice = e.empty() ? "scope window closed" : e;
                notice_until = ui.model.t_sec + (e.empty() ? 3 : 10);
            }
            scope_win_was = running;
        }
        if (!ui.model.scope_notice.empty() && ui.model.t_sec > ui.model.scope_notice_until) ui.model.scope_notice.clear();
        {   // history store, YouTube overlay and recording notices: once per frame
            const RadioStatus rs = engine.status();
            const long long now_unix = static_cast<long long>(std::time(nullptr));
            rhist.tick(rs, ui.model.dt, now_unix);
            HistoryModel& hm = ui.model.hmenu;
            hm.hist = &rhist; hm.now = now_unix;
            ui.model.hmenu.hist = &rhist;
            if (hm.yt_open) {
                hm.yt = yt.snapshot();
                if (yt_jump && !hm.yt.searching) {
                    yt_jump = false;
                    if (!hm.yt.items.empty()) { hm.yt_in_field = false; set_text_entry(false); }
                }
                hm.yt_cursor = std::clamp(hm.yt_cursor, 0, std::max(0, static_cast<int>(hm.yt.items.size()) - 1));
            }
            if (rs.record_serial != record_serial_seen) {
                record_serial_seen = rs.record_serial;
                ui.model.notice = rs.record_note; notice_until = ui.model.t_sec + 10;
            }
            if (!ui.model.notice.empty() && ui.model.t_sec > notice_until) ui.model.notice.clear();
        }
        int k;
        int menus_inject = 0;   // the key of the entry chosen in the MENUS index: handled next, as if it had just been typed
        while ((k = (menus_inject ? std::exchange(menus_inject, 0) : term.poll_key())) != 0) {
            const bool arrow = last_key_was_arrow();
            const int nvis = static_cast<int>(ui.model.visible.size());
            // ------------------------------------------------------------------ HELP WIZARD (SHIFT+w)
            if (ui.model.wizard_open) {
                // The page / part / contents state and what a key does live in src/wizard.cpp, the same for both modes.
                wizard::Key wk = wizard::Key::None;
                if (ui.model.wizard.searching) {      // SHIFT+s: the query has the keyboard (CTRL+C copies there)
                    wizard::search_key(ui.model.wizard, k, arrow, arrow && last_key_was_shifted());
                    set_text_entry(ui.model.wizard.searching);
                    continue;
                }
                if (k == 3 || k == kKeyCtrlC) running = false;
                else if (arrow) {
                    const bool shifted = last_key_was_shifted();
                    if (k == 'C') wk = wizard::Key::Right;
                    else if (k == 'D') wk = wizard::Key::Left;
                    else if (k == 'A') wk = shifted ? wizard::Key::PageUp : wizard::Key::Up;
                    else if (k == 'B') wk = shifted ? wizard::Key::PageDown : wizard::Key::Down;
                } else if (k == 27 || k == 'q' || k == 'Q' || key_translate(cfg, k) == 'W') wk = wizard::Key::Close;
                else if (k == 13 || k == 10) wk = wizard::Key::Enter;
                else if (k == 8 || k == 127) wk = wizard::Key::Back;                 // BACKSPACE and [t]: the table of content
                else if (k == 't' || k == 'T') wk = wizard::Key::Contents;
                else if (k == 'S') wk = wizard::Key::Search;                         // SHIFT+s: search the whole text
                if (wk == wizard::Key::Search) set_text_entry(true);
                if (wizard::handle_key(ui.model.wizard, wk)) {
                    set_text_entry(false);
                    ui.model.wizard_open = false;
                    out_clear_next = true;
                    wizard_seen_save();
                }
                continue;
            }
            // SHIFT+w opens the wizard from every menu and overlay too -- unless a text field has the keyboard (then it is a letter).
            if (wizard::kEnabled && !arrow && !text_entry_active() && key_translate(cfg, k) == 'W') {
                wizard_open_now();
                continue;
            }
            // ------------------------------------------------------------------ CHEATSHEET (?)
            if (ui.model.cheat_open) {
                UiModel& um = ui.model;
                if (um.cheat_searching) {   // SHIFT+s: the search field has the keyboard (CTRL+C copies there)
                    const int n = static_cast<int>(um.cheat_result_rows.size());
                    if (!arrow && k == 27) {
                        if (!um.cheat_query.empty()) { um.cheat_query.clear(); um.cheat_qcaret = um.cheat_qanchor = 0; um.cheat_sel = um.cheat_first = 0; }
                        else um.cheat_searching = false;
                    } else if (arrow && (k == 'A' || k == 'B')) {
                        const int step = last_key_was_shifted() ? 5 : 1;
                        um.cheat_sel = std::clamp(um.cheat_sel + (k == 'B' ? step : -step), 0, std::max(0, n - 1));
                    } else if (!arrow && (k == 13 || k == 10)) {
                        if (n > 0) um.cheat_jump_row = um.cheat_result_rows[static_cast<size_t>(std::clamp(um.cheat_sel, 0, n - 1))];
                        um.cheat_searching = false;
                    } else if (muisc::edit_text_key(um.cheat_query, um.cheat_qcaret, um.cheat_qanchor, k, 80, nullptr)) {
                        um.cheat_sel = um.cheat_first = 0;
                    }
                    set_text_entry(um.cheat_searching);
                    continue;
                }
                if (!arrow && k == 'S') { set_text_entry(true); um.cheat_searching = true; um.cheat_query.clear(); um.cheat_qcaret = um.cheat_qanchor = 0; um.cheat_sel = um.cheat_first = 0; continue; }
                if (k == 3 || k == kKeyCtrlC) running = false;
                else if (arrow) {
                    if (k == 'A') ui.model.cheat_scroll -= (last_key_was_shifted() ? 10 : 1);        // up (SHIFT / PgUp: a page); the renderer clamps both ends
                    else if (k == 'B') ui.model.cheat_scroll += (last_key_was_shifted() ? 10 : 1);   // down
                } else if (k == '?' || k == 27) ui.model.cheat_open = false;
                else if (k == 'j') ++ui.model.cheat_scroll;
                else if (k == 'k') --ui.model.cheat_scroll;
                continue;
            }
            // ------------------------------------------------------------------ CHOOSE AUDIO DEVICE (over the settings or the main screen)
            if (ui.model.settings.adev_open) {
                SettingsModel& sm = ui.model.settings;
                if (k == 3 || k == kKeyCtrlC) { adev_close(); if (sm.open) settings_close(false); running = false; continue; }
                const int rows = 1 + static_cast<int>(sm.adev_list.size());
                if (arrow && k == 'A') { sm.adev_sel = (sm.adev_sel + rows - 1) % rows; continue; }
                if (arrow && k == 'B') { sm.adev_sel = (sm.adev_sel + 1) % rows; continue; }
                if (!arrow && (k == 'r' || k == 'R')) {   // look again (a headset that was just plugged in)
                    const std::string keep = sm.adev_sel > 0 && sm.adev_sel <= static_cast<int>(sm.adev_list.size()) ? sm.adev_list[static_cast<size_t>(sm.adev_sel - 1)].name : std::string();
                    sm.adev_err.clear();
                    sm.adev_list = muisc::list_output_devices(&sm.adev_err);
                    sm.adev_sel = 0;
                    for (size_t i = 0; i < sm.adev_list.size(); ++i) if (!keep.empty() && sm.adev_list[i].name == keep) sm.adev_sel = static_cast<int>(i) + 1;
                    continue;
                }
                if (!arrow && (k == 13 || k == 10)) { adev_pick(); adev_close(); continue; }
                if (!arrow && (k == 27 || key_translate(cfg, k) == kKeyDegree)) adev_close();
                continue;
            }
            // ------------------------------------------------------------------ RADIO SETTINGS (s)
            if (ui.model.settings.open) {
                SettingsModel& sm = ui.model.settings;
                if (sm.remote_ov.open) {   // the REMOTE APP overlay has the keys
                    if (k == 3 || k == kKeyCtrlC) { settings_close(false); running = false; continue; }
                    muisc::remote_ui::Values v{cfg.remote_app, cfg.remote_port, cfg.remote_pin};
                    bool close = false;
                    if (muisc::remote_ui::key(sm.remote_ov, v, k, arrow, close)) {
                        cfg.remote_app = v.enabled; cfg.remote_port = v.port; cfg.remote_pin = v.pin;
                        muisc::remote::net::configure({cfg.remote_app, cfg.remote_port, cfg.remote_pin});
                        sm.dirty = true;
                    }
                    if (close) { sm.remote_ov = muisc::remote_ui::State{}; sm.status = cfg.remote_app ? "remote app on (s saves)" : "remote app off (s saves)"; }
                    continue;
                }
                if (sm.leave_prompt != 0 && !sm.editing) {
                    // the question on top: [s] / ENTER save, [d] discard, anything else (ESC) goes back to editing; "overwrite?" wants [y]
                    const int kind = sm.leave_prompt;
                    sm.leave_prompt = 0;
                    if (k == 3 || k == kKeyCtrlC) { settings_close(false); running = false; continue; }
                    const bool save = !arrow && (k == 's' || k == 'S' || k == 13 || k == 10);
                    const bool discard = !arrow && (k == 'd' || k == 'D');
                    if (kind == 3) { if (!arrow && (k == 'y' || k == 'Y')) theme_save_now(); continue; }
                    if (kind == 4) {   // "delete the theme?": only y says yes
                        if (!arrow && (k == 'y' || k == 'Y') && sm.theme_sel >= 0 && sm.theme_sel < static_cast<int>(sm.theme_list.size())) {
                            const std::string name = sm.theme_list[static_cast<size_t>(sm.theme_sel)].name;
                            std::string err;
                            if (themes::delete_theme(name, err)) {
                                sm.theme_list = themes::load_all();
                                sm.theme_sel = std::clamp(sm.theme_sel, 0, std::max(0, static_cast<int>(sm.theme_list.size()) - 1));
                                sm.status = "THEME \"" + name + "\" DELETED";
                            } else sm.status = err;
                        }
                        continue;
                    }
                    if (kind == 1) { if (save) settings_close(true); else if (discard) settings_close(false); }
                    else if (save) theme_try_save();     // may ask once more (overwriting a theme of your own)
                    else if (discard) { sm.theme_ui = 0; sm.status.clear(); }
                    continue;
                }
                if (sm.theme_ui != 0) {                                        // the THEME picker / CREATE THEME overlay
                    const int nt = static_cast<int>(sm.theme_list.size());
                    if (sm.editing) {
                        if (k == 3 || k == kKeyCtrlC) { sm.editing = false; set_text_entry(false); running = false; continue; }
                        if (k == 27) { sm.editing = false; sm.status.clear(); set_text_entry(false); continue; }
                        if ((k == 13 || k == 10) && (sm.theme_ui == 4 || sm.theme_ui == 5)) {
                            if (sm.theme_ui == 4 && sm.theme_io_row == 0) sm.theme_io_name = themes::sanitize_name(sm.buffer);
                            else {
                                sm.theme_io_dir = sm.buffer;
                                if (sm.theme_ui == 5) { theme_io_rescan(); sm.theme_io_sel = 0; sm.theme_io_row = sm.theme_io_files.empty() ? 0 : 1; }
                            }
                            sm.editing = false; set_text_entry(false);
                            continue;
                        }
                        if (k == 13 || k == 10) {
                            if (sm.theme_row < 0) sm.theme_name = themes::sanitize_name(sm.buffer);
                            else {
                                std::string* f = color_field(sm.theme_draft, sm.theme_row, sm.theme_col);
                                if (f && valid_color_value(sm.buffer)) { *f = sm.buffer == "0" ? std::string() : sm.buffer; sm.status.clear(); }
                                else sm.status = "Not a colour: use a palette number 0-255 (empty or 0 = terminal colour)";
                            }
                            sm.editing = false; set_text_entry(false);
                            continue;
                        }
                        if (sm.theme_ui == 4 || sm.theme_ui == 5) {
                            edit_text_key(sm.buffer, sm.edit, k, (sm.theme_ui == 4 && sm.theme_io_row == 0) ? static_cast<size_t>(themes::kMaxNameLen) : 240, &sm.status);
                            continue;
                        }
                        if (sm.theme_row >= 0 && textedit_detail::is_text_key(k) && !arrow && !(k >= '0' && k <= '9')) continue;
                        edit_text_key(sm.buffer, sm.edit, k, sm.theme_row < 0 ? static_cast<size_t>(themes::kMaxNameLen) : 3, &sm.status);
                        continue;
                    }
                    if (k == 3 || k == kKeyCtrlC) { settings_close(false); running = false; continue; }
                    if (sm.theme_ui == 4) {                                    // export the current colours as a theme file
                        if (k == 27 || (!arrow && (k == 'q' || k == 'Q'))) { sm.theme_ui = 0; sm.status.clear(); continue; }
                        if (arrow && k == 'A') { sm.theme_io_row = 0; continue; }
                        if (arrow && k == 'B') { sm.theme_io_row = 1; continue; }
                        if (k == 13 || k == 10) {
                            sm.buffer = sm.theme_io_row == 0 ? sm.theme_io_name : sm.theme_io_dir;
                            sm.edit.to_end(sm.buffer); sm.editing = true; sm.status.clear(); set_text_entry(true);
                            continue;
                        }
                        if (!arrow && (k == 's' || k == 'S')) {
                            const std::string name = themes::sanitize_name(sm.theme_io_name);
                            if (name.empty()) { sm.status = "give the theme a name first"; continue; }
                            const int cur = theme_index(cfg, sm.theme_list);
                            themes::Theme t = theme_capture(cfg, sm.theme_list[static_cast<size_t>(cur >= 0 ? cur : 0)]);
                            t.name = name;
                            const std::filesystem::path file = theme_dir_path(sm.theme_io_dir) / themes::file_name_for(name);
                            std::string err;
                            if (!themes::write_theme_file(file, t, err)) { sm.status = err; continue; }
                            sm.theme_ui = 0; sm.status = "EXPORTED " + file.string();
                        }
                        continue;
                    }
                    if (sm.theme_ui == 5) {                                    // import theme files from a folder
                        const int nf = static_cast<int>(sm.theme_io_files.size());
                        auto import_files = [&](const std::vector<std::filesystem::path>& files) {
                            int n = 0; std::string last, err;
                            for (const auto& f : files)
                                for (themes::Theme& t : themes::read_theme_file(f)) {
                                    const std::string name = themes::import_theme(t, err);
                                    if (!name.empty()) { ++n; last = name; }
                                }
                            sm.theme_list = themes::load_all();
                            sm.theme_ui = 0;
                            sm.status = n == 0 ? (err.empty() ? std::string("no theme found in that file") : err)
                                               : "IMPORTED " + std::to_string(n) + " THEME" + (n == 1 ? "" : "S") + (n == 1 ? ": " + last : std::string()) + " -- pick it in the THEME row";
                        };
                        if (k == 27 || (!arrow && (k == 'q' || k == 'Q'))) { sm.theme_ui = 0; sm.status.clear(); continue; }
                        if (arrow && k == 'A') { if (sm.theme_io_row == 1 && sm.theme_io_sel > 0) --sm.theme_io_sel; else sm.theme_io_row = 0; continue; }
                        if (arrow && k == 'B') {
                            if (sm.theme_io_row == 0) { if (nf > 0) sm.theme_io_row = 1; }
                            else if (sm.theme_io_sel < nf - 1) ++sm.theme_io_sel;
                            continue;
                        }
                        if (k == 13 || k == 10) {
                            if (sm.theme_io_row == 0) { sm.buffer = sm.theme_io_dir; sm.edit.to_end(sm.buffer); sm.editing = true; sm.status.clear(); set_text_entry(true); }
                            else if (sm.theme_io_sel >= 0 && sm.theme_io_sel < nf) import_files({sm.theme_io_files[static_cast<size_t>(sm.theme_io_sel)]});
                            continue;
                        }
                        if (!arrow && k == 'a') { if (nf > 0) import_files(sm.theme_io_files); continue; }
                        continue;
                    }
                    if (sm.theme_ui == 1 || sm.theme_ui == 3) {
                        if (sm.theme_peek) { sm.theme_peek = false; continue; }   // any key closes the preview
                        if (!arrow && k == 'P') { sm.theme_peek = true; continue; }
                        if (k == 27 || (!arrow && (k == 'q' || k == 'Q'))) { sm.theme_ui = sm.theme_ui == 3 ? 2 : 0; continue; }
                        if (arrow && (k == 'A' || k == 'B') && last_key_was_shifted()) {   // SHIFT+up / down: a page
                            sm.theme_sel = std::clamp(sm.theme_sel + (k == 'A' ? -10 : 10), 0, std::max(0, nt - 1));
                            continue;
                        }
                        if ((arrow && k == 'A') || (!arrow && k == 'k')) { sm.theme_sel = std::max(0, sm.theme_sel - 1); continue; }
                        if ((arrow && k == 'B') || (!arrow && k == 'j')) { sm.theme_sel = std::min(std::max(0, nt - 1), sm.theme_sel + 1); continue; }
                        if (k == kKeyHome) { sm.theme_sel = 0; continue; }
                        if (k == kKeyEnd) { sm.theme_sel = std::max(0, nt - 1); continue; }
                        if (sm.theme_ui == 1 && !arrow && k == 'D' && sm.theme_sel >= 0 && sm.theme_sel < nt) {   // SHIFT+d: delete a theme of your own
                            const themes::Theme& t = sm.theme_list[static_cast<size_t>(sm.theme_sel)];
                            if (t.builtin) sm.status = "\"" + t.name + "\" is a built-in theme and cannot be deleted";
                            else sm.leave_prompt = 4;   // "Do you want to delete theme?"
                            continue;
                        }
                        if ((k == 13 || k == 10) && sm.theme_sel >= 0 && sm.theme_sel < nt) {
                            const themes::Theme& t = sm.theme_list[static_cast<size_t>(sm.theme_sel)];
                            if (sm.theme_ui == 1) {
                                theme_apply(cfg, t);
                                sm.dirty = true; sm.theme_ui = 0; sm.status = "THEME: " + t.name + " (s saves the settings)";
                            } else {
                                theme_apply(sm.theme_draft, t);
                                sm.theme_base = t; sm.theme_ui = 2; sm.status = "template: " + t.name;
                            }
                        }
                        continue;
                    }
                    // --- CREATE THEME
                    if (k == 27 || (!arrow && (k == 'q' || k == 'Q'))) {
                        if (theme_changed()) { sm.leave_prompt = 2; continue; }   // anything made here is lost on ESC: ask first
                        sm.theme_ui = 0; sm.status.clear(); continue;
                    }
                    if (!arrow && (k == 's' || k == 'S')) { theme_try_save(); continue; }
                    const bool up = (arrow && k == 'A') || (!arrow && k == 'k');
                    const bool down = (arrow && k == 'B') || (!arrow && k == 'j');
                    const bool left = (arrow && k == 'D') || (!arrow && k == 'h');
                    const bool right = (arrow && k == 'C') || (!arrow && k == 'l');
                    if (up) { if (sm.theme_row >= 0) { --sm.theme_row; if (sm.theme_row < 0) sm.theme_col = 0; } continue; }
                    if (down) {
                        if (sm.theme_row < kColorRowCount - 1) {
                            ++sm.theme_row;
                            if (sm.theme_col == 1 && !color_field(sm.theme_draft, sm.theme_row, 1)) sm.theme_col = 0;
                        }
                        continue;
                    }
                    if (left) { sm.theme_col = 0; continue; }
                    if (right) { if (sm.theme_row < 0 || color_field(sm.theme_draft, sm.theme_row, 1)) sm.theme_col = 1; continue; }
                    if (k == 13 || k == 10) {
                        if (sm.theme_row < 0 && sm.theme_col == 1) {
                            sm.theme_ui = 3; sm.theme_sel = 0;
                            for (int i = 0; i < nt; ++i) if (sm.theme_list[static_cast<size_t>(i)].name == sm.theme_base.name) sm.theme_sel = i;
                            continue;
                        }
                        if (sm.theme_row < 0) sm.buffer = sm.theme_name;
                        else { const std::string* f = color_field(sm.theme_draft, sm.theme_row, sm.theme_col); sm.buffer = f ? *f : std::string(); }
                        sm.edit.to_end(sm.buffer); sm.editing = true; sm.status.clear(); set_text_entry(true);
                    }
                    continue;
                }
                if (sm.editing && sm.tab == 3) {                              // a folder field: full line editor
                    if (k == 3) { sm.editing = false; set_text_entry(false); running = false; continue; }
                    if (k == 27 && !last_key_was_arrow()) { sm.editing = false; sm.status.clear(); set_text_entry(false); continue; }
                    if (k == 13 || k == 10) {
                        std::string* f = path_row_text(cfg, sm.row);
                        if (f) {
                            std::string v = sm.buffer;
                            while (!v.empty() && v.back() == ' ') v.pop_back();
                            if (!v.empty() && v[0] == '~' && (v.size() == 1 || v[1] == '/' || v[1] == '\\')) {
                                const char* home = std::getenv("HOME");
                                if (!home || !*home) home = std::getenv("USERPROFILE");
                                if (home) v = std::string(home) + v.substr(1);
                            }
                            if (sm.row == 5 && muisc::book_path_conflicts(muisc::load_settings().local_music_paths, v)) {
                                sm.status = "CONFLICT: that folder is also a LOCAL MUSIC PATH of the player. Pick a sub folder of it or another folder";
                                sm.editing = false; set_text_entry(false);
                                continue;
                            }
                            if (*f != v) { *f = v; sm.dirty = true; storage_changed(); }
                        }
                        sm.editing = false; set_text_entry(false);
                        continue;
                    }
                    edit_text_key(sm.buffer, sm.edit, k, 400, &sm.status);
                    continue;
                }
                if (sm.editing && sm.tab == 4) {                              // a key: one character or a name
                    if (k == 3) { sm.editing = false; set_text_entry(false); running = false; continue; }
                    if (k == 27 && !last_key_was_arrow()) { sm.editing = false; sm.status.clear(); set_text_entry(false); continue; }
                    if (k == 13 || k == 10) {
                        std::string v = sm.buffer;
                        while (!v.empty() && v.back() == ' ') v.pop_back();
                        if (v.empty() && sm.buffer.find(' ') != std::string::npos) v = "SPACE";
                        std::string up = v;
                        for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                        if (up == "SPACE" || up == "TAB" || up == "BACKSPACE") v = up;
                        if (key_code(v) == 0) { sm.status = "Not a key: type one character, or SPACE / TAB / BACKSPACE"; sm.buffer.clear(); sm.edit.reset(); continue; }
                        const std::string conflict = key_conflict(cfg, v, (sm.row - 1));
                        if (!conflict.empty()) {
                            sm.status = "KEY \"" + v + "\" ALREADY USED BY " + conflict + " -- try another key";
                            sm.buffer.clear(); sm.edit.reset();
                            continue;                                           // stays in the field, like the player
                        }
                        key_change_begin((sm.row - 1));
                        const auto before = cfg.keys;
                        if (v == kKeyActions[(sm.row - 1)].def) cfg.keys.erase(kKeyActions[(sm.row - 1)].id); else cfg.keys[kKeyActions[(sm.row - 1)].id] = v;
                        key_change_end(cfg.keys != before);
                        sm.dirty = true; sm.status = std::string("UPDATED ") + kKeyActions[(sm.row - 1)].label;
                        sm.editing = false; set_text_entry(false);
                        continue;
                    }
                    edit_text_key(sm.buffer, sm.edit, k, 12, &sm.status);
                    continue;
                }
                if (sm.editing) {
                    if (k == 3 || k == kKeyCtrlC) { sm.editing = false; set_text_entry(false); running = false; continue; }
                    if (k == 27) { sm.editing = false; sm.status.clear(); set_text_entry(false); continue; }
                    if (k == 13 || k == 10) {
                        std::string* f = color_field(cfg, color_row_of_tab(sm.row), sm.col);
                        if (f && valid_color_value(sm.buffer)) {
                            *f = sm.buffer == "0" ? std::string() : sm.buffer;
                            sm.dirty = true; sm.status.clear();
                        } else sm.status = "Not a colour: use a palette number 0-255 (empty or 0 = terminal colour)";
                        sm.editing = false; set_text_entry(false);
                        continue;
                    }
                    // Same caret keys as every other field (LEFT/RIGHT move it, SHIFT+LEFT/RIGHT mark, HOME/END,
                    // DEL / BACKSPACE, CTRL+C/X/V). TAB and the arrows never leave the field: only ENTER or ESC
                    // do. Typing is digits only (a palette number); a pasted non-number is refused by ENTER.
                    if (textedit_detail::is_text_key(k) && !arrow && !(k >= '0' && k <= '9')) continue;
                    edit_text_key(sm.buffer, sm.edit, k, 3, &sm.status);
                    continue;
                }
                if (k == 3 || k == kKeyCtrlC) { settings_close(false); running = false; continue; }
                if (k == kKeyCtrlShiftU) {                                     // undo the last key change (up to 5, newest first)
                    if (key_undo.empty()) { sm.status = "nothing to undo"; continue; }
                    const KeyUndo u = key_undo.back();
                    key_undo.pop_back();
                    int where = u.row;
                    if (where < 0) {                                           // a full reset: go to the first key that comes back
                        for (int i = 0; i < kKeyActionCount && where < 0; ++i) {
                            RadioSettings old_s; old_s.keys = u.keys;                  // the binding as it was before the reset
                            if (key_binding(old_s, i) != key_binding(cfg, i)) where = i;
                        }
                    }
                    cfg.keys = u.keys;
                    sm.tab = 4; sm.col = 0; sm.row = std::clamp(where, 0, kKeyActionCount - 1) + 1;   // row 0 is the reset line
                    sm.dirty = true;
                    sm.status = u.row < 0 ? "UNDONE: reset of all keys" : std::string("UNDONE: ") + kKeyActions[u.row].label + " is now \"" + key_binding(cfg, u.row) + "\"";
                    continue;
                }
                if (k == 9) {                                                  // TAB: COLORS <-> GENERAL
                    sm.tab = (sm.tab + 1) % 6; sm.row = 0; sm.col = 0; sm.status.clear();
                    continue;
                }
                const int nrows = sm.tab == 1 ? kOnOffRowCount : sm.tab == 2 ? (rs_set_hifi_rows(cfg.bars_style == 1), rs_row_count(2)) : sm.tab == 3 ? kPathRowCount : sm.tab == 4 ? kKeyActionCount + 1 : sm.tab == 5 ? static_cast<int>(about_app_text().size()) : color_tab_row_count();   // + the THEME and SMOOTHNESS rows
                // the Volume indicator row has two values (the look | gradient or not): the only row of the other tabs that keeps its column
                const bool vol_row = (sm.tab == 1 || sm.tab == 2) && std::string(rs_row_label(sm.tab, sm.row)) == "Volume indicator";
                auto fix_col = [&]() {
                    const bool vol_now = (sm.tab == 1 || sm.tab == 2) && std::string(rs_row_label(sm.tab, sm.row)) == "Volume indicator";
                    if ((sm.tab >= 1 && !vol_now) || (sm.tab == 0 && sm.row != 0 && !color_field(cfg, color_row_of_tab(sm.row), sm.col))) sm.col = 0;
                };
                auto toggle = [&](int dir) {   // GENERAL and ANIMA/VIZ rows: left / right / ENTER
                    if (sm.tab == 1 && std::string(rs_row_label(sm.tab, sm.row)) == "Choose Audio Device") {   // the CHOOSE AUDIO DEVICE overlay
                        if (dir == 0) adev_open(true);
                        else sm.status = "[ENTER] lists the audio devices";
                        return;
                    }
                    if (sm.tab == 1 && std::string(rs_row_label(sm.tab, sm.row)) == "Remote App") {   // opens the REMOTE APP overlay
                        sm.remote_ov = muisc::remote_ui::State{};
                        sm.remote_ov.open = true;
                        return;
                    }
                    if (sm.tab == 3) {   // PATHS: only the bool row toggles; the text rows are edited with ENTER
                        if (!path_row_is_bool(sm.row)) {
                            if (dir == 0) {
                                std::string* f = path_row_text(cfg, sm.row);
                                sm.editing = true; sm.buffer = f ? *f : std::string(); sm.edit.to_end(sm.buffer); sm.status.clear(); set_text_entry(true);
                            }
                            return;
                        }
                        cfg.download_same_as_player = !cfg.download_same_as_player;
                        sm.dirty = true; sm.status.clear(); storage_changed();
                        return;
                    }
                    if (vol_row) {   // LEFT / RIGHT (and h / l) switch between the look and the gradient, ENTER changes the one that is lit
                        if (dir != 0) { sm.col = dir > 0 ? 1 : 0; sm.status = "[ENTER] changes the value that is lit; [<-] [->] switch between the look and the gradient"; return; }
                        if (sm.col == 1) cfg.volume_gradient = !cfg.volume_gradient;
                        else rs_row_change(cfg, sm.tab, sm.row, 0);
                        sm.dirty = true;
                        return;
                    }
                    if (sm.tab == 1 && std::string(rs_row_label(sm.tab, sm.row)) == "Terminal default font") {   // applied and saved at once, for all modes
                        rs_row_change(cfg, sm.tab, sm.row, dir);
                        muisc::set_config_value("TerminalDefaultFont", cfg.terminal_default_font ? "true" : "false");
                        sm.status = muisc::terminal_font::apply(cfg.terminal_default_font ? std::string(muisc::terminal_font::kTerminalDefault) : cfg.terminal_font);
                        return;
                    }
                    rs_row_change(cfg, sm.tab, sm.row, dir);
                    ui.model.shuffle = cfg.playback_shuffle;
                    sm.dirty = true; sm.status.clear();
                    apply_audio();
                };
                if (arrow) {
                    if (k == 'A') sm.row = std::max(0, sm.row - 1);
                    else if (k == 'B') sm.row = std::min(nrows - 1, sm.row + 1);
                    else if (sm.tab >= 1 && sm.tab <= 3) toggle(k == 'C' ? +1 : -1);
                    else if (sm.row == 2) { cfg.border_smooth = !cfg.border_smooth; sm.dirty = true; sm.status.clear(); }   // SMOOTHNESS
                    else if (k == 'C') sm.col = 1;
                    else if (k == 'D') sm.col = 0;
                    fix_col();
                    continue;
                }
                if (k == 27 || k == 'q' || k == 'Q') {
                    if (settings_changed()) { sm.leave_prompt = 1; continue; }   // unsaved changes: ask first
                    settings_close(false);
                    continue;
                }
                if (k == 's' || k == 'S') { settings_close(true); continue; }
                if (sm.tab == 0 && !arrow && k == 'e') { theme_open_creator(true); continue; }   // edit the theme in use
                if (k == 'j') { sm.row = std::min(nrows - 1, sm.row + 1); fix_col(); continue; }
                if (k == 'k') { sm.row = std::max(0, sm.row - 1); fix_col(); continue; }
                if (sm.tab == 0 && !arrow && (k == 'E' || k == 'I')) {        // SHIFT+e / SHIFT+i: export / import a theme
                    sm.theme_list = themes::load_all();
                    if (sm.theme_io_dir.empty()) sm.theme_io_dir = themes::exchange_dir().string();
                    if (k == 'E') {
                        const int cur = theme_index(cfg, sm.theme_list);
                        sm.theme_io_name = cur >= 0 ? sm.theme_list[static_cast<size_t>(cur)].name : std::string("My Theme");
                        sm.theme_io_row = 0; sm.theme_ui = 4;
                    } else {
                        sm.theme_io_sel = 0; theme_io_rescan();
                        sm.theme_io_row = sm.theme_io_files.empty() ? 0 : 1; sm.theme_ui = 5;
                    }
                    sm.status.clear();
                    continue;
                }
                if (sm.tab == 5) continue;                                     // ABOUT APP: only scrolls
                if (sm.tab == 4) {                                             // REFERENCE
                    if (sm.row == 0) {                                          // the RESET line (right under the note)
                        if (k == 13 || k == 10) {
                            if (cfg.keys.empty()) sm.status = "all keys already have their default";
                            else { key_change_begin(-1); cfg.keys.clear(); sm.dirty = true; sm.status = "ALL KEYS RESET TO DEFAULT (Ctrl+Shift+U undoes it)"; }
                        }
                        continue;
                    }
                    if (k == 13 || k == 10) { sm.editing = true; sm.buffer.clear(); sm.edit.reset(); sm.status.clear(); set_text_entry(true); }
                    else if (k == kKeyDelete || k == 127) {
                        key_change_begin(sm.row - 1);
                        const auto before = cfg.keys;
                        cfg.keys.erase(kKeyActions[sm.row - 1].id);
                        key_change_end(cfg.keys != before);
                        sm.dirty = true; sm.status = std::string("default key restored for ") + kKeyActions[sm.row - 1].label;
                    }
                    continue;
                }
                if (sm.tab >= 1) {
                    if (k == 'h') toggle(-1);
                    else if (k == 'l') toggle(+1);
                    else if (k == 13 || k == 10 || k == ' ') toggle(0);
                    continue;
                }
                if (sm.row == 2) {                                              // SMOOTHNESS: rough <-> smooth
                    if (k == 'h' || k == 'l' || k == 13 || k == 10 || k == ' ') { cfg.border_smooth = !cfg.border_smooth; sm.dirty = true; sm.status.clear(); }
                    continue;
                }
                if (k == 'h') { sm.col = 0; continue; }
                if (k == 'l') { if (sm.row == 0 || color_field(cfg, color_row_of_tab(sm.row), 1)) sm.col = 1; continue; }
                if ((k == 13 || k == 10) && sm.row == 0) {          // THEME | CREATE THEME
                    sm.theme_list = themes::load_all();
                    const int cur = theme_index(cfg, sm.theme_list);
                    if (sm.col == 0) { sm.theme_sel = std::max(0, cur); sm.theme_ui = 1; sm.status.clear(); }
                    else theme_open_creator(false);
                    continue;
                }
                if (k == 13 || k == 10) {
                    const std::string* f = color_field(cfg, color_row_of_tab(sm.row), sm.col);
                    if (f) { sm.editing = true; sm.buffer = *f; sm.edit.to_end(sm.buffer); sm.status.clear(); set_text_entry(true); }
                    continue;
                }
                continue;
            }
            // ------------------------------------------------------------------ LISTENING HISTORY (SHIFT+h)
            if (ui.model.hmenu.open) {
                HistoryModel& hm = ui.model.hmenu;
                const int nh = static_cast<int>(rhist.size());
                const int nt = static_cast<int>(rhist.top_channels(true).size());
                if (hm.yt_open) {                                              // the YouTube overlay owns every key
                    if (k == 3) { hm.yt_open = false; set_text_entry(false); running = false; continue; }
                    if (k == 27 && !arrow) { hm.yt_open = false; yt_jump = false; set_text_entry(false); continue; }
                    const int nres = static_cast<int>(hm.yt.items.size());
                    if (hm.yt_in_field) {
                        if (k == 13 || k == 10) {
                            if (!hm.yt_text.empty()) { yt.search(hm.yt_text); yt_jump = true; hm.yt_cursor = 0; }
                        } else if (k == 9 || (arrow && k == 'B')) {
                            if (nres > 0) { hm.yt_in_field = false; set_text_entry(false); }
                        } else edit_text_key(hm.yt_text, hm.yt_edit, k, 200, nullptr);
                        continue;
                    }
                    if (arrow && k == 'A') hm.yt_cursor = std::max(0, hm.yt_cursor - 1);
                    else if (arrow && k == 'B') hm.yt_cursor = std::min(std::max(0, nres - 1), hm.yt_cursor + 1);
                    else if (!arrow && k == 'k') hm.yt_cursor = std::max(0, hm.yt_cursor - 1);
                    else if (!arrow && k == 'j') hm.yt_cursor = std::min(std::max(0, nres - 1), hm.yt_cursor + 1);
                    else if (k == 13 || k == 10) { if (nres > 0) yt.download(static_cast<size_t>(hm.yt_cursor)); }
                    else if (k == 9 || k == '/') { hm.yt_in_field = true; hm.yt_edit.to_end(hm.yt_text); set_text_entry(true); }
                    continue;
                }
                if (k == 3 || k == kKeyCtrlC) { running = false; continue; }
                if (!arrow && (k == 27 || k == 'q' || k == 'Q' || k == key_of(cfg, "History"))) { hm.open = false; hm.flash.clear(); continue; }
                if (!arrow && (k == '1' || k == '2' || k == '3')) { hm.tab = k - '1'; hm.flash.clear(); continue; }
                if (k == 9 || (arrow && k == 'C')) { hm.tab = (hm.tab + 1) % 3; hm.flash.clear(); continue; }
                if (arrow && k == 'D') { hm.tab = (hm.tab + 2) % 3; hm.flash.clear(); continue; }
                int& cur = hm.tab == 1 ? hm.top_cursor : hm.tab == 0 ? hm.cursor : hm.scroll;
                const int last = hm.tab == 0 ? nh - 1 : hm.tab == 1 ? nt - 1 : 1000000;
                auto move = [&](int d) { cur = std::clamp(cur + d, 0, std::max(0, last)); };
                if ((arrow && k == 'A') || (!arrow && k == 'k')) { move(arrow && last_key_was_shifted() ? -10 : -1); continue; }
                if ((arrow && k == 'B') || (!arrow && k == 'j')) { move(arrow && last_key_was_shifted() ? +10 : +1); continue; }
                if (k == kKeyHome || (!arrow && k == 'g')) { cur = 0; continue; }
                if (k == kKeyEnd || (!arrow && k == 'G')) { cur = std::max(0, last); continue; }
                if (!arrow && k == 'r' && hm.tab == 1) { hm.most_first = !hm.most_first; hm.top_cursor = 0; continue; }
                if (!arrow && k == 'y' && hm.tab == 0) {
                    if (nh == 0) { hm.flash = "nothing to search for yet"; continue; }
                    const HistoryEntry& e = rhist.newest(static_cast<size_t>(std::clamp(hm.cursor, 0, nh - 1)));
                    std::string q = e.artist.empty() ? e.title : e.title.empty() ? e.artist : e.artist + " " + e.title;
                    if (q.empty()) { hm.flash = "this line has no artist or title to search for"; continue; }
                    hm.yt_open = true; hm.yt_in_field = true; hm.yt_text = q; hm.yt_edit.to_end(hm.yt_text);
                    hm.yt_cursor = 0; yt.clear(); yt_jump = false; hm.flash.clear();
                    yt.set_download_dir(storage.download_dir);
                    hm.yt_dir = storage.download_dir.string();
                    set_text_entry(true);
                    yt.search(q); yt_jump = true;                              // the query is already right most of the time
                    continue;
                }
                continue;
            }
            // ------------------------------------------------------------------ SHIFT+o / SHIFT+v overlays (main screen)
            if (ui.model.stov.open && ui.model.stov.del_idx >= 0 && !ui.model.cheat_open) {   // the warning before a deletion
                StationsOverlay& so = ui.model.stov;
                if (k == 3) { running = false; continue; }
                if (!arrow && (k == 'y' || k == 'Y')) { const int d = so.del_idx; so.del_idx = -1; stov_delete(d); }
                else if ((!arrow && (k == 'n' || k == 'N')) || (k == 27 && !arrow)) { so.del_idx = -1; so.flash = "kept"; }
                continue;
            }
            if (ui.model.stov.open && (ui.model.stov.add_open || ui.model.stov.edit_open) && !ui.model.cheat_open) {
                StationsOverlay& so = ui.model.stov;
                if (k == 3) { set_text_entry(false); running = false; continue; }
                if (k == 27 && !arrow) { so.add_open = false; so.edit_open = false; so.add_error.clear(); so.edit_error.clear(); set_text_entry(false); continue; }
                if (so.edit_open) {
                    constexpr int nf = StationsOverlay::kEditFields;
                    auto go = [&](int f) { so.edit_field = (f % nf + nf) % nf; so.edit_ed[so.edit_field].to_end(so.edit_text[so.edit_field]); };
                    if (k == 13 || k == 10) stov_edit_commit();
                    else if (k == 9 || (arrow && k == 'B')) go(so.edit_field + 1);
                    else if (arrow && k == 'A') go(so.edit_field - 1);
                    else if (edit_text_key(so.edit_text[so.edit_field], so.edit_ed[so.edit_field], k, 500, nullptr)) so.edit_error.clear();   // (Left / Right / Home / End move the caret)
                    continue;
                }
                if (k == 13 || k == 10) { stov_add_commit(); continue; }
                if (k == 9 || (arrow && (k == 'A' || k == 'B'))) { so.add_field ^= 1; (so.add_field ? so.add_edit[1] : so.add_edit[0]).to_end(so.add_field ? so.add_name : so.add_url); continue; }
                std::string& buf = so.add_field ? so.add_name : so.add_url;
                if (edit_text_key(buf, so.add_edit[so.add_field], k, 500, nullptr)) so.add_error.clear();
                continue;
            }
            if (ui.model.overlay == 8 && !ui.model.cheat_open) {                 // "a newer version is out"
                if (k == 3 || k == kKeyCtrlC) { ui.model.overlay = 0; running = false; continue; }
                if (!arrow && (k == 'o' || k == 'O')) {
                    if (!muisc::open_url(ui.model.update_url)) { ui.model.notice = "could not open the browser: " + ui.model.update_url; notice_until = ui.model.t_sec + 6; }
                    ui.model.overlay = 0;
                } else if (!arrow && k == 27) {
                    ui.model.overlay = 0;
                }
                continue;
            }
            if (ui.model.overlay == 5 && !ui.model.cheat_open) {                 // the record overlay (y)
                int& orow = ui.model.overlay_row;
                constexpr int kRows = static_cast<int>(sizeof(kRecordBack) / sizeof(kRecordBack[0]));
                if (k == 3 || k == kKeyCtrlC) { ui.model.overlay = 0; running = false; continue; }
                if (arrow) {
                    if (k == 'A') orow = (orow + kRows - 1) % kRows;
                    else if (k == 'B') orow = (orow + 1) % kRows;
                    continue;
                }
                if (k == 13 || k == 10 || (k == 'y' && !arrow)) {
                    // seconds back from the moment the overlay opened (-1 = everything that is buffered)
                    const double want = kRecordBack[orow] < 0 ? ui.model.rec_avail_sec : static_cast<double>(kRecordBack[orow]);
                    const double back = std::min(want, ui.model.rec_avail_sec);
                    const long long from = std::max(0LL, ui.model.rec_anchor - static_cast<long long>(back * 48000.0));
                    const RadioStatus rs = engine.status();
                    ui.model.overlay = 0;
                    start_record(from, back + rs.behind_sec);
                    continue;
                }
                if (k == 27) { ui.model.overlay = 0; continue; }
                continue;
            }
            if (ui.model.overlay == 7 && !ui.model.cheat_open) {                 // the MENUS index (<)
                int& orow = ui.model.overlay_row;
                if (k == 3 || k == kKeyCtrlC) { ui.model.overlay = 0; running = false; continue; }
                if (arrow) {
                    if (k == 'A' || k == 'B' || k == 'C' || k == 'D') orow = menus_move(orow, k == 'A' ? 0 : k == 'B' ? 1 : k == 'D' ? 2 : 3);
                    continue;
                }
                if (k == 13 || k == 10) {   // open it: close the index, then handle the entry's key
                    const int chosen = key_of(cfg, kMenusEntries[std::clamp(orow, 0, kMenusCount - 1)].id);
                    ui.model.overlay = 0;
                    if (chosen > 0) menus_inject = chosen;
                    continue;
                }
                if (k == 27 || k == key_of(cfg, "Menus")) ui.model.overlay = 0;
                continue;
            }
            if (ui.model.overlay == 3 && !ui.model.cheat_open) {                 // the sleep timer overlay
                int& orow = ui.model.overlay_row;
                if (k == 3 || k == kKeyCtrlC) { ui.model.overlay = 0; settings_save(); running = false; continue; }
                if (arrow) {
                    if (k == 'A') orow = (orow + 6) % 7;
                    else if (k == 'B') orow = (orow + 1) % 7;
                    continue;
                }
                if (k == 13 || k == 10) {
                    if (orow < 5) { sleep_start(kSleepChoice[orow]); ui.model.overlay = 0; }
                    else if (orow == 5) { cfg.sleep_fade = !cfg.sleep_fade; ui.model.settings.dirty = true; }
                    else { sleep_cancel(); ui.model.notice = "sleep timer: off"; notice_until = ui.model.t_sec + 4; ui.model.overlay = 0; }
                    continue;
                }
                if (k == 27 || k == key_of(cfg, "SleepTimer")) { ui.model.overlay = 0; settings_save(); }
                continue;
            }
            if (ui.model.overlay == 4 && !ui.model.cheat_open) {                 // the equaliser overlay (SHIFT+e)
                EqUi& eu = ui.model.eq;
                const bool earrow = last_key_was_arrow();
                auto eq_close = [&]() { ui.model.overlay = 0; eu.naming = false; set_text_entry(false); settings_save(); };
                auto eq_changed = [&]() { apply_audio(); ui.model.settings.dirty = true; };
                if (eu.naming) {                                                    // the "Save as:" prompt owns every key
                    if (!earrow && k == 27) { eu.naming = false; eu.name.clear(); eu.name_edit.reset(); eu.status = "Save cancelled"; set_text_entry(false); continue; }
                    if (!earrow && (k == 13 || k == 10)) { if (eq_commit_name(cfg, eu)) { ui.model.settings.dirty = true; settings_save(); } set_text_entry(false); continue; }
                    edit_text_key(eu.name, eu.name_edit, k, kEqNameMaxBytes, nullptr);
                    continue;
                }
                if (k == 3 || k == kKeyCtrlC) { eq_close(); running = false; continue; }
                const bool armed = eu.delete_armed;
                eu.delete_armed = false;
                eu.status.clear();
                if (earrow && k == 'C') { eu.band = (eu.band + 1) % kEqBands; continue; }
                if (earrow && k == 'D') { eu.band = (eu.band + kEqBands - 1) % kEqBands; continue; }
                if (earrow && k == 'A') { eq_set_gain(cfg, eu.band, cfg.eq_gains[static_cast<size_t>(eu.band)] + 1.0f); eq_changed(); continue; }
                if (earrow && k == 'B') { eq_set_gain(cfg, eu.band, cfg.eq_gains[static_cast<size_t>(eu.band)] - 1.0f); eq_changed(); continue; }
                if (earrow) continue;
                if (k == ',' || k == '<') { eq_select_preset(cfg, eu, -1); eq_changed(); continue; }
                if (k == '.' || k == '>' || k == 9) { eq_select_preset(cfg, eu, +1); eq_changed(); continue; }
                if (k == '0') { eq_set_gain(cfg, eu.band, 0.0f); eq_changed(); continue; }
                if (k == ' ') { cfg.eq_enabled = !cfg.eq_enabled; eq_changed(); continue; }
                if (k == 'r' || k == 'R') { cfg.eq_gains = kEqPresets[0].gains; eu.last_preset = 0; eq_changed(); continue; }
                if (k == 27 || k == key_of(cfg, "EqMenu")) { eq_close(); continue; }
                if (k == 's' || k == 'S') { eq_begin_naming(cfg, eu); set_text_entry(true); continue; }
                if (k == kKeyDelete || k == 'x' || k == 'X') { if (eq_delete_custom(cfg, eu, armed)) { ui.model.settings.dirty = true; settings_save(); } continue; }
                continue;
            }
            if (ui.model.overlay == 10 && !ui.model.cheat_open) {  // spectrum bars options (SHIFT+b)
                int& orow = ui.model.overlay_row;
                const std::vector<int> brow = muisc::bars_rows(cfg);
                const int bn = static_cast<int>(brow.size());
                if (k == 3 || k == kKeyCtrlC) { ui.model.overlay = 0; settings_save(); running = false; continue; }
                if (arrow) {
                    if (k == 'A') orow = orow <= 0 ? bn - 1 : orow - 1;
                    else if (k == 'B') orow = orow >= bn - 1 ? 0 : orow + 1;
                    else if (k == 'C' || k == 'D') { muisc::bars_change(cfg, brow[static_cast<size_t>(std::clamp(orow, 0, bn - 1))], k == 'C' ? +1 : -1); ui.model.settings.dirty = true; }
                    orow = std::clamp(orow, 0, std::max(0, static_cast<int>(muisc::bars_rows(cfg).size()) - 1));
                    continue;
                }
                if (k == 27 || k == key_of(cfg, "BarsMenu")) { ui.model.overlay = 0; settings_save(); continue; }
                if (k == 'r' || k == 'R') { muisc::bars_reset(cfg); ui.model.settings.dirty = true; orow = std::clamp(orow, 0, bn - 1); continue; }
                continue;
            }
            if (ui.model.overlay == 6 && !ui.model.cheat_open) {   // spectrogram options (SHIFT+i)
                int& orow = ui.model.overlay_row;
                if (k == 3 || k == kKeyCtrlC) { ui.model.overlay = 0; settings_save(); running = false; continue; }
                if (arrow) {
                    if (k == 'A' || k == 'B') {   // only the rows of the view in use (the waterfall has others than the spectrogram); no "Display" row (the . key's job)
                        const std::vector<int> vis = spectro_visible_rows(cfg.spectro);
                        int at = 0;
                        for (size_t i = 0; i < vis.size(); ++i) if (vis[i] <= orow) at = static_cast<int>(i);
                        const int n = static_cast<int>(vis.size());
                        at = k == 'A' ? (at + n - 1) % n : (at + 1) % n;
                        orow = vis[static_cast<size_t>(at)];
                    }
                    else if (k == 'D' || k == 'C') {
                        if (orow == kSrDisplay) osci_adjust(cfg, kOrDisplay, k == 'C' ? +1 : -1);   // osci -> sphere -> spectro
                        else { spectro_adjust(cfg.spectro, orow, k == 'C' ? +1 : -1); cfg.spectro_sync(); }
                        ui.model.settings.dirty = true;
                    }
                    continue;
                }
                if (k == key_of(cfg, "ScopeWindow")) { toggle_scope_window(); continue; }      // the windows: always
                if (k == key_of(cfg, "SpectroWindow")) { toggle_spectro_window(); continue; }
                if (k == 27 || k == key_of(cfg, "SpectroMenu")) { ui.model.overlay = 0; settings_save(); continue; }
                if (k == key_of(cfg, "SpectroFull")) {
                    ui.model.spectro_full = !ui.model.spectro_full; if (ui.model.spectro_full) ui.model.scope_big = false; out_clear_next = true;
                    continue;
                }
                if (k == 'r' || k == 'R') { spectro_reset(cfg.spectro); cfg.spectro_sync(); ui.model.settings.dirty = true; continue; }
                continue;
            }
            if (ui.model.scope_big && ui.model.overlay == 0 && !ui.model.cheat_open) {   // the big oscilloscope overlay (SHIFT+0)
                UiModel& um = ui.model;
                if (um.scope_big_color_edit) {                    // the grid colour is being typed: every key belongs to the prompt
                    if (k == 3 || k == kKeyCtrlC) { running = false; continue; }
                    if (arrow) continue;
                    if (k == 27) { um.scope_big_color_edit = false; }
                    else if (k == 13 || k == 10) {
                        um.scope_big_color_edit = false;
                        if (um.scope_big_color_buf.empty()) cfg.scope_grid_color.clear();   // empty = the colour of inactive list elements
                        else cfg.scope_grid_color = std::to_string(std::clamp(std::atoi(um.scope_big_color_buf.c_str()), 0, 255));
                        settings_save();
                    } else if ((k == 127 || k == 8) && !um.scope_big_color_buf.empty()) um.scope_big_color_buf.pop_back();
                    else if (k >= '0' && k <= '9' && um.scope_big_color_buf.size() < 3) um.scope_big_color_buf.push_back(static_cast<char>(k));
                    continue;
                }
                if (um.scope_metrics_open) {                      // the metrics window: UP / DOWN pick a row, LEFT / RIGHT change it
                    if (k == 3 || k == kKeyCtrlC) { running = false; continue; }
                    if (arrow) {
                        if (k == 'A') um.scope_metrics_row = (um.scope_metrics_row + kScopeMetricRows - 1) % kScopeMetricRows;
                        else if (k == 'B') um.scope_metrics_row = (um.scope_metrics_row + 1) % kScopeMetricRows;
                        else if (k == 'C' || k == 'D') { scope_metric_step(cfg.scope_metrics, um.scope_metrics_row, k == 'C' ? 1 : -1); settings_save(); }
                    } else if (k == 27 || k == 'P') um.scope_metrics_open = false;
                    else if (k == 'r' || k == 'R') { cfg.scope_metrics = ScopeMetrics(); settings_save(); }
                    continue;
                }
                if (!arrow && k == 27) { um.scope_big = false; out_clear_next = true; continue; }
                if (!arrow && k == 'P') { um.scope_metrics_open = true; continue; }                                    // SHIFT+p: grid metrics
                if (!arrow && k == 'A') { cfg.scope_grid = !cfg.scope_grid; settings_save(); continue; }               // SHIFT+a: axes / grid
                if (!arrow && k == 'V') { cfg.scope_values = !cfg.scope_values; settings_save(); continue; }               // SHIFT+v: axis values
                if (!arrow && k == 'C') { um.scope_big_color_edit = true; um.scope_big_color_buf.clear(); continue; }   // SHIFT+c: grid colour
            }
            if (ui.model.spectro_full && ui.model.overlay == 0 && !ui.model.cheat_open && k == 27 && !arrow) {   // ESC leaves the full screen
                ui.model.spectro_full = false;
                out_clear_next = true;
                continue;
            }
            if (ui.model.overlay != 0 && !ui.model.cheat_open) {
                const bool osci = ui.model.overlay == 1;
                int& orow = ui.model.overlay_row;
                auto close_overlay = [&]() { ui.model.overlay = 0; settings_save(); };   // like the player: closing saves
                if (k == 3 || k == kKeyCtrlC) { close_overlay(); running = false; continue; }
                if (arrow) {
                    const std::vector<int> oids = osci_visible_rows(cfg);
                    const int nov = osci ? static_cast<int>(oids.size()) : 3;
                    orow = std::clamp(orow, 0, nov - 1);
                    if (k == 'A') orow = (orow + nov - 1) % nov;
                    else if (k == 'B') orow = (orow + 1) % nov;
                    else if (k == 'D' || k == 'C') {
                        if (osci) {
                            const int id = oids[static_cast<size_t>(orow)];
                            const std::string proto_before = cfg.gfx_protocol;
                            osci_adjust(cfg, id, k == 'C' ? +1 : -1);
                            if (cfg.gfx_protocol != proto_before) {   // another protocol was picked: ask / look again
                                if (gfx_shown) { std::string off = gfx_clear(gfx); write_frame(off); gfx_shown = false; }
                                if (gfx2_shown) { std::string off = gfx_clear(gfx, 1); write_frame(off); gfx2_shown = false; }
                                gfx = gfx_probe(cfg.gfx_protocol);
                                ui.model.gfx_proto = gfx;
                            }
                            ui.model.shuffle = cfg.playback_shuffle;
                        } else { norm_adjust(cfg, orow, k == 'C' ? +1 : -1); apply_audio(); }
                        ui.model.settings.dirty = true;
                    }
                    continue;
                }
                if (osci && k == key_of(cfg, "ScopeBig")) {   // the big overlay can be opened / closed from the options too
                    ui.model.scope_big = !ui.model.scope_big; ui.model.spectro_full = false; ui.model.scope_big_color_edit = false; ui.model.scope_metrics_open = false; out_clear_next = true;
                    continue;
                }
                if (k == 27 || (osci && k == key_of(cfg, "OsciMenu")) || (!osci && k == key_of(cfg, "NormMenu"))) { close_overlay(); continue; }
                if (k == 'r' || k == 'R') {
                    if (osci) osci_reset(cfg); else { norm_reset(cfg); apply_audio(); }
                    ui.model.settings.dirty = true;
                    continue;
                }
                if (!osci && (k == ' ' || k == 'v')) { cfg.normalize = !cfg.normalize; apply_audio(); ui.model.settings.dirty = true; continue; }
                continue;
            }
            // ------------------------------------------------------------------ RADIO BROWSER menu (Shift+S)
            if (ui.model.browse.open) {
                BrowseModel& bm = ui.model.browse;
                const int nres = bm.results ? static_cast<int>(bm.results->size()) : 0;
                const bool in_field = bm.focus != BrowseFocus::Results;
                const int fi = static_cast<int>(bm.focus);
                // In a pane Ctrl+C is COPY (text-entry mode delivers it as kKeyCtrlC); in RESULTS it quits.
                if ((k == 3 || k == kKeyCtrlC) && !in_field) { running = false; continue; }
                if (k == 9) {                                              // TAB: NAME ... BITRATE -> RESULTS -> NAME
                    browse_focus(bm.focus == BrowseFocus::Results ? BrowseFocus::Name : static_cast<BrowseFocus>(fi + 1));
                    continue;
                }
                if (k == 27) {                                             // ESC: clear the pane, else close
                    if (in_field && !bm.text[fi].empty()) { bm.text[fi].clear(); bm.edit[fi].reset(); }
                    else browse_close();
                    continue;
                }
                if (k == 13 || k == 10) {                                  // ENTER: search (pane) / tune (RESULTS)
                    if (in_field) browse_search();
                    else if (bm.cursor >= 0 && bm.cursor < nres) browse_tune((*bm.results)[static_cast<size_t>(bm.cursor)]);
                    continue;
                }
                if (arrow && (k == 'A' || k == 'B')) {
                    if (!in_field) {                                       // RESULTS: move the cursor
                        if (k == 'A') bm.cursor = std::max(0, bm.cursor - (last_key_was_shifted() ? 10 : 1));
                        else bm.cursor = std::min(std::max(0, nres - 1), bm.cursor + (last_key_was_shifted() ? 10 : 1));
                    } else if (k == 'A') { if (fi >= 2) browse_focus(static_cast<BrowseFocus>(fi - 2)); }   // up one pane row
                    else browse_focus(fi + 2 >= kBrowseFields ? BrowseFocus::Results : static_cast<BrowseFocus>(fi + 2));
                    continue;
                }
                if (in_field) {
                    std::string status;
                    edit_text_key(bm.text[fi], bm.edit[fi], k, kTextMax, &status);   // typing, caret, marking, Home/End, Del, Ctrl+C/X/V
                    if (!status.empty()) bm.flash = status;
                    continue;
                }
                // --- RESULTS focused
                if (k == '/') { browse_focus(BrowseFocus::Name); continue; }
                if (k == '?') { ui.model.cheat_open = true; ui.model.cheat_scroll = 0; continue; }
                if (k == 'a' && bm.cursor >= 0 && bm.cursor < nres) { browse_add((*bm.results)[static_cast<size_t>(bm.cursor)]); continue; }
                continue;
            }
            // ------------------------------------------------------------------ STATION LISTS menu (Shift+K)
            if (ui.model.lmenu.open) {
                ListMenuModel& lm = ui.model.lmenu;
                const int nstat = static_cast<int>(lm.visible.size());
                const int nitems = static_cast<int>(lm.items.size());
                const int nman = static_cast<int>(lm.mvisible.size());
                // prompts swallow every key but the three they care about (same as the player)
                if (lm.confirm_exit) {
                    if (k == 'y' || k == 'Y') { lm.confirm_exit = false; lm_save(true); lm_sync_entry(); }
                    else if (k == 'n' || k == 'N') lm_close();
                    else if (k == 27) { lm.confirm_exit = false; lm_sync_entry(); }
                    continue;
                }
                if (lm.confirm_delete) {
                    if (k == 'y' || k == 'Y') { lm.confirm_delete = false; lm_delete(); lm_sync_entry(); }
                    else if (k == 'n' || k == 'N' || k == 27) { lm.confirm_delete = false; lm_sync_entry(); }
                    continue;
                }
                const bool in_text = lm_in_text();
                if ((k == 3 || k == kKeyCtrlC) && !in_text) { running = false; continue; }   // in a text field Ctrl+C copies
                if (k == 27) {                                    // ESC: leave a field typed in, clear a search with text, else leave (asking if unsaved)
                    if (in_text && lm.typing) { lm.typing = false; continue; }   // Left/Right switch tabs again; the text stays
                    if (lm.tab == 0 && lm.focus == ListFocus::Search && !lm.search.empty()) {
                        lm.search.clear(); lm.search_edit.reset(); lm.cursor = 0; ui.refilter_lists_stations();
                    } else if (lm.tab == 1 && lm.mfocus == ListManageFocus::Search && !lm.msearch.empty()) {
                        lm.msearch.clear(); lm.msearch_edit.reset(); lm.mcursor = 0; ui.refilter_saved_lists();
                    } else if (lm.tab == 0 && lm.dirty) { lm.confirm_exit = true; set_text_entry(false); }
                    else lm_close();
                    continue;
                }
                if (k == kKeyCtrlS) {                             // CTRL+s saves from every pane of tab 1
                    if (lm.tab == 0) lm_save(false);
                    else lm.flash = "switch to CREATE / EDIT (\u2190\u2192) to save a list";
                    continue;
                }
                // Tab switch: plain Left/Right -- except in a text box that is in use (typed / edited in), where they move
                // the caret until ESC, TAB or ENTER. ALT+Left/Right switches from everywhere.
                if (k == kKeyAltLeft || k == kKeyAltRight || (arrow && (k == 'C' || k == 'D') && !(in_text && lm.typing))) {
                    lm.tab = 1 - lm.tab;
                    lm.typing = false;
                    if (lm.tab == 1) ui.refilter_saved_lists();
                    lm_sync_entry();
                    continue;
                }
                // Any other key that reaches a text box below (typing, BACKSPACE / DEL, HOME / END, marking,
                // the clipboard) puts it in use. Up/Down walk the list below the box, ENTER / TAB leave it.
                if (in_text && !(arrow && (k == 'A' || k == 'B')) && k != 13 && k != 10 && k != 9) lm.typing = true;
                if (lm.tab == 1) {
                    // --- tab 2: SAVED STATION LISTS
                    if (k == 9) { lm_mfocus(lm.mfocus == ListManageFocus::Search ? ListManageFocus::List : ListManageFocus::Search); continue; }
                    if (arrow && (k == 'A' || k == 'B')) {
                        lm.mcursor = k == 'A' ? std::max(0, lm.mcursor - (last_key_was_shifted() ? 10 : 1)) : std::min(std::max(0, nman - 1), lm.mcursor + (last_key_was_shifted() ? 10 : 1));
                        continue;
                    }
                    if (lm.mfocus == ListManageFocus::Search) {
                        if (k == 13 || k == 10) { lm_mfocus(ListManageFocus::List); continue; }
                        std::string status;
                        const bool changed = edit_text_key(lm.msearch, lm.msearch_edit, k, kTextMax, &status);
                        lm.flash = status;
                        if (changed) { lm.mcursor = 0; ui.refilter_saved_lists(); }
                        continue;
                    }
                    if (k == 13 || k == 10) { lm_load(); continue; }
                    if (k == kKeyDelete && nman > 0) { lm.confirm_delete = true; continue; }
                    if (k == 'j') { lm.mcursor = std::min(std::max(0, nman - 1), lm.mcursor + 1); continue; }
                    if (k == 'k') { lm.mcursor = std::max(0, lm.mcursor - 1); continue; }
                    if (k == '/') { lm_mfocus(ListManageFocus::Search); continue; }
                    if (k == '?') { ui.model.cheat_open = true; ui.model.cheat_scroll = 0; continue; }
                    continue;
                }
                // --- tab 1: CREATE / EDIT
                if (k == 9) {                                    // TAB: name -> search -> STATIONS -> LIST CONTENTS -> name
                    lm_focus(lm.focus == ListFocus::Name ? ListFocus::Search
                             : lm.focus == ListFocus::Search ? ListFocus::Stations
                             : lm.focus == ListFocus::Stations ? ListFocus::Contents : ListFocus::Name);
                    continue;
                }
                if (lm.focus == ListFocus::Name) {
                    if (k == 13 || k == 10) { lm_focus(ListFocus::Search); continue; }    // confirm the name, go and pick stations
                    std::string status;
                    if (edit_limited(lm.name, lm.name_edit, k, static_cast<size_t>(kListNameMax), &status)) lm.dirty = true;
                    lm.flash = status;
                    continue;
                }
                if (lm.focus == ListFocus::Search) {
                    if (arrow && (k == 'A' || k == 'B')) {      // Up/Down keep walking the results while the box has the keyboard
                        lm.cursor = k == 'A' ? std::max(0, lm.cursor - (last_key_was_shifted() ? 10 : 1)) : std::min(std::max(0, nstat - 1), lm.cursor + (last_key_was_shifted() ? 10 : 1));
                        continue;
                    }
                    if (k == 13 || k == 10) { lm_add(); continue; }
                    std::string status;
                    const bool changed = edit_text_key(lm.search, lm.search_edit, k, kTextMax, &status);
                    lm.flash = status;
                    if (changed) { lm.cursor = 0; ui.refilter_lists_stations(); }
                    continue;
                }
                if (lm.focus == ListFocus::Stations) {
                    if (arrow && (k == 'A' || k == 'B')) {
                        lm.cursor = k == 'A' ? std::max(0, lm.cursor - (last_key_was_shifted() ? 10 : 1)) : std::min(std::max(0, nstat - 1), lm.cursor + (last_key_was_shifted() ? 10 : 1));
                        continue;
                    }
                    if (k == 'j') { lm.cursor = std::min(std::max(0, nstat - 1), lm.cursor + 1); continue; }
                    if (k == 'k') { lm.cursor = std::max(0, lm.cursor - 1); continue; }
                    if (k == 13 || k == 10) { lm_add(); continue; }
                    if (k == 'T') {                              // SHIFT+t: list order <-> name A-Z (also for search results)
                        ui.toggle_lists_sort();
                        lm.flash = lm.sort_az ? "STATIONS sorted by name A-Z" : "STATIONS in list order";
                        continue;
                    }
                } else {   // Contents
                    if (arrow && (k == 'A' || k == 'B')) {
                        lm.item_cursor = k == 'A' ? std::max(0, lm.item_cursor - 1) : std::min(std::max(0, nitems - 1), lm.item_cursor + 1);
                        continue;
                    }
                    if (k == 'j') { lm.item_cursor = std::min(std::max(0, nitems - 1), lm.item_cursor + 1); continue; }
                    if (k == 'k') { lm.item_cursor = std::max(0, lm.item_cursor - 1); continue; }
                    if (k == 13 || k == 10) {                    // tune the hovered station (audition the list)
                        if (lm.item_cursor >= 0 && lm.item_cursor < nitems) tune_to(lm.items[static_cast<size_t>(lm.item_cursor)]);
                        continue;
                    }
                    if (k == kKeyDelete || k == 127 || k == 8 || k == 'd') { lm_remove(); continue; }
                    if (k == '4') { lm_move(-1); continue; }
                    if (k == '5') { lm_move(+1); continue; }
                }
                // keys shared by STATIONS and LIST CONTENTS (never reached from a text field)
                if (k == '/') { lm_focus(ListFocus::Search); continue; }
                if (k == '?') { ui.model.cheat_open = true; ui.model.cheat_scroll = 0; continue; }
                continue;
            }
            // ------------------------------------------------------------------ PRESETS menu (P)
            if (ui.model.menu.open) {
                MenuModel& mn = ui.model.menu;
                const int mvis = static_cast<int>(mn.visible.size());
                const int pvis = static_cast<int>(mn.pvisible.size());
                auto set_focus = [&](MenuFocus f) {
                    mn.focus = f;
                    if (f == MenuFocus::Search) { mn.search_edit.to_end(mn.search); mn.psearch_edit.to_end(mn.psearch); }
                    set_text_entry(f == MenuFocus::Search);
                };
                // SHIFT+d (twice) deletes the hovered preset; any other key withdraws the first press.
                const int del_pending = mn.del_confirm;
                if (!(k == 'D' && !arrow && mn.focus == MenuFocus::Presets)) mn.del_confirm = -1;
                auto tune_hovered = [&]() {
                    if (mn.cursor < 0 || mn.cursor >= mvis) return;
                    tune_to(mn.visible[static_cast<size_t>(mn.cursor)]);
                };
                auto open_hovered_preset = [&]() {
                    if (mn.pcursor < 0 || mn.pcursor >= pvis) return;
                    const int bi = mn.pvisible[static_cast<size_t>(mn.pcursor)];
                    ui.use_bank(bi);
                    mn.flash = "Preset \"" + ui.model.banks[static_cast<size_t>(bi)].name + "\" opened";
                    for (int sl = 0; sl < static_cast<int>(ui.model.presets.size()); ++sl)      // ENTER also tunes in: the first filled slot
                        if (ui.model.presets[static_cast<size_t>(sl)] >= 0) { tune_to(ui.model.presets[static_cast<size_t>(sl)]); mn.flash += "  -- tuned slot " + std::string(1, kPresetKeys[sl]); break; }
                    if (!ui.persist()) mn.flash += "  -- could not write " + presets_path();
                };
                auto close_menu = [&]() { mn.open = false; mn.name.open = false; set_focus(MenuFocus::Stations); set_text_entry(false); mn.flash.clear(); };
                // moves in the SELECT PRESET grid (4 columns)
                auto move_preset = [&](int d) { mn.pcursor = std::clamp(mn.pcursor + d, 0, std::max(0, pvis - 1)); };
                auto open_name_overlay = [&](bool rename) {
                    NameOverlay& ov = mn.name;
                    ov = NameOverlay{};
                    ov.open = true; ov.rename = rename;
                    if (rename) {
                        ov.target = mn.pvisible[static_cast<size_t>(mn.pcursor)];
                        ov.text = ui.model.banks[static_cast<size_t>(ov.target)].name;
                        ov.edit.to_end(ov.text);
                    }
                    set_text_entry(true);
                };

                // --- name overlay (Shift+N / Shift+C): owns every key while it is open
                if (mn.name.open) {
                    NameOverlay& ov = mn.name;
                    auto leave = [&]() { ov.open = false; set_text_entry(mn.focus == MenuFocus::Search); };
                    if (k == 27) { leave(); continue; }
                    if (k == 13 || k == 10) {
                        const std::string nm = clean_preset_name(ov.text);
                        if (nm.empty()) { ov.error = "type a name first"; continue; }
                        if (ui.bank_name_taken(nm, ov.rename ? ov.target : -1)) { ov.error = "a preset with that name exists"; continue; }
                        if (ov.rename) {
                            ui.model.banks[static_cast<size_t>(ov.target)].name = nm;
                            mn.flash = "Preset renamed to \"" + nm + "\"";
                        } else {
                            PresetBank b; b.name = nm;
                            ui.sync_active();
                            ui.model.banks.push_back(std::move(b));
                            ui.use_bank(static_cast<int>(ui.model.banks.size()) - 1);   // the new preset opens, ready to be filled
                            mn.psearch.clear(); mn.psearch_edit.reset();
                            mn.pcursor = 0;
                            mn.flash = "Preset \"" + nm + "\" created";
                        }
                        ui.refilter_presets();
                        ui.cursor_to_active_preset();
                        if (!ui.persist()) mn.flash += "  -- could not write " + presets_path();
                        leave();
                        continue;
                    }
                    // caret, marking, Home/End, Delete, Ctrl+C/X/V and typing -- the player's own text editor.
                    // The name is limited in CHARACTERS (kPresetNameMax), the editor in bytes, so measure after.
                    const std::string before = ov.text;
                    const EditState before_edit = ov.edit;
                    if (edit_text_key(ov.text, ov.edit, k, static_cast<size_t>(kPresetNameMax) * 4)) {
                        if (count_chars(ov.text) > static_cast<size_t>(kPresetNameMax)) {
                            if (k == kKeyCtrlV) {
                                // too long a paste: keep what fits, cut the surplus off the end of the pasted part
                                const size_t start = std::min(before_edit.caret, before_edit.anchor);
                                while (count_chars(ov.text) > static_cast<size_t>(kPresetNameMax) && ov.edit.caret > start) {
                                    size_t i = ov.edit.caret - 1;
                                    while (i > start && (static_cast<unsigned char>(ov.text[i]) & 0xC0) == 0x80) --i;
                                    ov.text.erase(i, ov.edit.caret - i);
                                    ov.edit.caret = i;
                                }
                                ov.edit.anchor = ov.edit.caret;
                            } else {                                   // a typed character into a full box: refused
                                ov.text = before; ov.edit = before_edit;
                            }
                        }
                        ov.error.clear();
                    }
                    continue;
                }

                // In the search box Ctrl+C is COPY (text-entry mode delivers it as kKeyCtrlC) and Shift+Left/Right
                // mark text; everywhere else they quit / switch the preset.
                if ((k == 3 || k == kKeyCtrlC) && mn.focus != MenuFocus::Search) { running = false; continue; }
                if ((k == kKeyShiftLeft || k == kKeyShiftRight) && mn.focus != MenuFocus::Search) {   // previous / next preset
                    if (ui.model.banks.size() > 1) {
                        ui.step_bank(k == kKeyShiftRight ? +1 : -1);
                        ui.cursor_to_active_preset();
                        mn.flash.clear();
                    }
                    continue;
                }
                if (arrow && !(mn.focus == MenuFocus::Search && (k == 'C' || k == 'D'))) {   // (search box: Left/Right = caret)
                    const bool grid = mn.focus == MenuFocus::Presets || (mn.focus == MenuFocus::Search && mn.target == SearchTarget::Presets);
                    if (grid) {
                        if (k == 'A') move_preset(-kPresetPaneCols);
                        else if (k == 'B') { const int last = std::max(0, pvis - 1);
                                             mn.pcursor = (mn.pcursor + kPresetPaneCols <= last) ? mn.pcursor + kPresetPaneCols
                                                        : (mn.pcursor / kPresetPaneCols < last / kPresetPaneCols ? last : mn.pcursor); }
                        else if (k == 'D') move_preset(-1);
                        else if (k == 'C') move_preset(+1);
                    } else {
                        if (k == 'A') mn.cursor = std::max(0, mn.cursor - 1);
                        else if (k == 'B') mn.cursor = std::min(std::max(0, mvis - 1), mn.cursor + 1);
                    }
                    continue;
                }
                if (k == 9) {                                                            // TAB: SEARCH -> SELECT PRESET -> STATIONS -> ...
                    set_focus(mn.focus == MenuFocus::Search ? MenuFocus::Presets
                              : mn.focus == MenuFocus::Presets ? MenuFocus::Stations : MenuFocus::Search);
                    continue;
                }
                if (k == 27) {                                                           // ESC
                    std::string& q = mn.target == SearchTarget::Presets ? mn.psearch : mn.search;
                    if (mn.focus == MenuFocus::Search && !q.empty()) {
                        q.clear(); (mn.target == SearchTarget::Presets ? mn.psearch_edit : mn.search_edit).reset();
                        mn.cursor = 0; mn.pcursor = 0; ui.refilter_menu(); ui.refilter_presets();
                    } else close_menu();
                    continue;
                }
                if (k == 13 || k == 10) {                                                // ENTER: tune a station / open a preset
                    if (mn.focus == MenuFocus::Presets) open_hovered_preset();
                    else if (mn.focus == MenuFocus::Search && mn.target == SearchTarget::Presets) { open_hovered_preset(); set_focus(MenuFocus::Presets); }
                    else { tune_hovered(); if (mn.focus == MenuFocus::Search) set_focus(MenuFocus::Stations); }
                    continue;
                }
                if (mn.focus == MenuFocus::Search) {
                    const bool presets = mn.target == SearchTarget::Presets;
                    std::string& q = presets ? mn.psearch : mn.search;
                    EditState& es = presets ? mn.psearch_edit : mn.search_edit;
                    std::string status;   // "COPIED" / "CUT" / "PASTED" -> the status line under the hints
                    const bool changed = edit_text_key(q, es, k, kTextMax, &status);
                    mn.flash = status;
                    if (!changed) continue;   // caret / marking / copy: the results stay exactly where they are
                    // "p:" typed into an empty box points the search at the presets, "s:" back at the stations
                    // (the older "/p:" and "/s:" still work)
                    const std::string lq = ascii_lower_str(q);
                    if (lq == "p:" || lq == "s:" || lq == "/p:" || lq == "/s:") {
                        q.clear(); es.reset();
                        mn.target = lq.find('p') != std::string::npos ? SearchTarget::Presets : SearchTarget::Stations;
                    }
                    mn.cursor = 0; mn.pcursor = 0;
                    ui.refilter_menu(); ui.refilter_presets();
                    continue;
                }
                if (k == '?') { ui.model.cheat_open = true; ui.model.cheat_scroll = 0; continue; }
                if (k == '/') { mn.target = mn.focus == MenuFocus::Presets ? SearchTarget::Presets : SearchTarget::Stations; set_focus(MenuFocus::Search); continue; }
                // --- SELECT PRESET focused
                if (mn.focus == MenuFocus::Presets) {
                    if (k == 'j') { move_preset(+kPresetPaneCols); continue; }
                    if (k == 'k') { move_preset(-kPresetPaneCols); continue; }
                    if (k == 'l') { move_preset(+1); continue; }
                    if (k == 'h') { move_preset(-1); continue; }
                    if (k == 'N') { open_name_overlay(false); continue; }                       // SHIFT+n: new preset
                    if (k == 'C' && pvis > 0) { open_name_overlay(true); continue; }            // SHIFT+c: rename the hovered preset
                    if (k == 'D' && !arrow && pvis > 0) {                                        // SHIFT+d: delete the hovered preset (twice)
                        const int bi = mn.pvisible[static_cast<size_t>(std::clamp(mn.pcursor, 0, pvis - 1))];
                        const std::string bname = ui.model.banks[static_cast<size_t>(bi)].name;
                        if (ui.model.banks.size() < 2) { mn.flash = "The last preset cannot be deleted"; continue; }
                        if (del_pending != bi) {
                            mn.del_confirm = bi;
                            mn.flash = "Delete preset \"" + bname + "\"? Press SHIFT+d again to confirm, any other key cancels";
                            continue;
                        }
                        ui.sync_active();
                        ui.model.banks.erase(ui.model.banks.begin() + bi);
                        if (bi < ui.model.bank_active) --ui.model.bank_active;
                        else if (bi == ui.model.bank_active) ui.model.bank_active = std::min(bi, static_cast<int>(ui.model.banks.size()) - 1);
                        ui.model.presets = ui.model.banks[static_cast<size_t>(ui.model.bank_active)].slots;
                        ui.refilter_presets();
                        mn.flash = "Preset \"" + bname + "\" deleted";
                        if (!ui.persist()) mn.flash += "  -- could not write " + presets_path();
                        continue;
                    }
                    continue;
                }
                // --- STATIONS focused
                if (k == 'j') { mn.cursor = std::min(std::max(0, mvis - 1), mn.cursor + 1); continue; }
                if (k == 'k') { mn.cursor = std::max(0, mn.cursor - 1); continue; }
                if (mn.cursor < 0 || mn.cursor >= mvis) continue;
                const int idx = mn.visible[static_cast<size_t>(mn.cursor)];
                const std::string& nm = ui.stations[static_cast<size_t>(idx)].name;
                const int slot = preset_slot_for_key(k);
                if (slot >= 0) {
                    auto& p = ui.model.presets;
                    if (p[static_cast<size_t>(slot)] == idx) {             // same key again: clear
                        p[static_cast<size_t>(slot)] = -1;
                        mn.flash = std::string("Preset ") + kPresetKeys[slot] + " cleared";
                    } else {
                        const int old = preset_slot_of(p, idx);
                        if (old >= 0) p[static_cast<size_t>(old)] = -1;      // a station sits in one slot only: it moves
                        p[static_cast<size_t>(slot)] = idx;
                        mn.flash = std::string("Preset ") + kPresetKeys[slot] + " = " + nm
                                 + (old >= 0 ? std::string("  (moved from ") + kPresetKeys[old] + ")" : "");
                    }
                    if (!ui.persist()) mn.flash += "  -- could not write " + presets_path();
                } else if (k == kKeyDelete || k == 127 || k == 8) {
                    if (clear_station_from_presets(ui.model.presets, idx)) {
                        mn.flash = "Removed " + nm + " from the presets";
                        if (!ui.persist()) mn.flash += "  -- could not write " + presets_path();
                    }
                }
                continue;
            }
            if (ui.model.search_focus) {
                // Up/Down walk the results, everything else goes to the text editor (Left/Right = caret,
                // Shift+Left/Right = mark, Home/End, Del, Ctrl+C/X/V, typing). Ctrl+C is copy here, not quit.
                UiModel& um = ui.model;
                const int nl = static_cast<int>(um.lvisible.size());
                if (arrow && (k == 'A' || k == 'B')) {
                    int& cur = um.search_lists ? um.lcursor : um.cursor;
                    const int n = um.search_lists ? nl : nvis;
                    cur = k == 'A' ? std::max(0, cur - 1) : std::min(std::max(0, n - 1), cur + 1);
                } else if (k == 27) {   // clear the box, back to searching stations, leave it
                    um.search.clear(); um.search_edit.reset(); um.search_lists = false;
                    ui.refilter(); ui.refilter_main_lists(); set_search_focus(false);
                } else if (k == 13 || k == 10) {
                    if (um.search_lists) {                // ENTER on a list: its stations fill the STATIONS pane (ESC = all again)
                        if (um.lcursor >= 0 && um.lcursor < nl) {
                            um.active_list = um.lists[static_cast<size_t>(um.lvisible[static_cast<size_t>(um.lcursor)])].name;
                            um.search.clear(); um.search_edit.reset(); um.search_lists = false; um.cursor = 0;
                            ui.refilter(); ui.refilter_main_lists(); set_search_focus(false);
                            tune_visible(0);                   // ENTER also tunes in: the first station of the list
                        }                                  // no list matches: stay in the box, nothing to open
                    } else if (nvis > 0) { set_search_focus(false); tune_visible(um.cursor); }
                    // no hit: stay in the box, so the query can be edited or cleared with ESC
                } else if (edit_text_key(um.search, um.search_edit, k, kTextMax)) {
                    // "p:" typed into the empty box points it at the station lists, "s:" back at the stations
                    // (the older "/p:" and "/s:" spellings work too) -- same switch as the PRESETS menu
                    const std::string lq = ascii_lower_str(um.search);
                    if (lq == "p:" || lq == "s:" || lq == "/p:" || lq == "/s:") {
                        um.search.clear(); um.search_edit.reset();
                        um.search_lists = lq.find('p') != std::string::npos;
                    }
                    um.cursor = 0; um.lcursor = 0;
                    ui.refilter(); ui.refilter_main_lists();
                }
                continue;
            }
            if (ui.model.stov.open) {                                        // the big STATIONS overlay (same state as the main pane)
                UiModel& um = ui.model;
                StationsOverlay& so = um.stov;
                const int nl = static_cast<int>(um.lvisible.size());
                if (k == 27 && !arrow) {
                    if (!um.search.empty() || um.search_lists) { um.search.clear(); um.search_edit.reset(); um.search_lists = false; um.cursor = 0; }
                    else if (!um.active_list.empty()) { um.active_list.clear(); um.cursor = 0; }
                    else { so.open = false; so.flash.clear(); }
                    ui.refilter(); ui.refilter_main_lists();
                    continue;
                }
                if (!arrow && k == key_of(cfg, "Stations")) { so.open = false; so.flash.clear(); continue; }
                if (um.search_lists && arrow) {   // the pane lists the station lists
                    if (k == 'A') um.lcursor = std::max(0, um.lcursor - (last_key_was_shifted() ? 10 : 1));
                    else if (k == 'B') um.lcursor = std::min(std::max(0, nl - 1), um.lcursor + (last_key_was_shifted() ? 10 : 1));
                    continue;
                }
                if (um.search_lists && (k == 13 || k == 10)) {
                    if (um.lcursor >= 0 && um.lcursor < nl) {
                        um.active_list = um.lists[static_cast<size_t>(um.lvisible[static_cast<size_t>(um.lcursor)])].name;
                        um.search.clear(); um.search_edit.reset(); um.search_lists = false; um.cursor = 0;
                        ui.refilter(); ui.refilter_main_lists();
                        tune_visible(0);                       // ENTER also tunes in: the first station of the list
                    }
                    continue;
                }
                if (!arrow && k == 'a') {
                    so.add_open = true; so.add_field = 0; so.add_url.clear(); so.add_name.clear(); so.add_error.clear();
                    so.add_edit[0].reset(); so.add_edit[1].reset(); so.flash.clear(); set_text_entry(true);
                    continue;
                }
                if (!arrow && k == 'C') {
                    if (um.cursor >= 0 && um.cursor < nvis && !um.search_lists) stov_edit_open(um.visible[static_cast<size_t>(um.cursor)]);
                    continue;
                }
                if (!arrow && k == 'U') { if (!um.search_lists) stov_undo_do(); continue; }
                if (k == kKeyDelete) {
                    if (um.cursor >= 0 && um.cursor < nvis && !um.search_lists) { so.del_idx = um.visible[static_cast<size_t>(um.cursor)]; so.flash.clear(); }
                    continue;
                }
                {   // keys that would open another menu are ignored here
                    const int kt = key_translate(cfg, k);
                    if (!arrow && (kt == 's' || kt == 'h' || kt == 'K' || kt == 'S' || kt == 'P' || kt == 'O' || kt == 'V' || kt == 'Z' || kt == 'E')) continue;
                }
            }
            if (k == 27) {   // ESC in the main screen: first clear a search that is still there, then go back from a list to all stations
                UiModel& um = ui.model;
                if (!um.search.empty() || um.search_lists) { um.search.clear(); um.search_edit.reset(); um.search_lists = false; um.cursor = 0; }
                else if (!um.active_list.empty()) { um.active_list.clear(); um.cursor = 0; }
                ui.refilter(); ui.refilter_main_lists();
                continue;
            }
            if (k == kKeyShiftLeft || k == kKeyShiftRight) { ui.step_bank(k == kKeyShiftRight ? +1 : -1); continue; }   // main UI: previous / next preset
            if (arrow) {   // Left / Right do nothing in the main UI any more (channels: n / b / # / presets)
                if (k == 'A') ui.model.cursor = std::max(0, ui.model.cursor - (last_key_was_shifted() ? 10 : 1));   // SHIFT+arrow / PgUp / PgDn: a page
                else if (k == 'B') ui.model.cursor = std::min(std::max(0, nvis - 1), ui.model.cursor + (last_key_was_shifted() ? 10 : 1));
                continue;
            }
            if ((g_mode_key_next != 0 && k == g_mode_key_next) || (g_mode_key_prev != 0 && k == g_mode_key_prev)) {   // SHIFT and the + key ('*' on a German keyboard): the next mode; SHIFT and - ('_'): the previous one (keys: the player's REFERENCE tab)
                const int dir = (g_mode_key_next != 0 && k == g_mode_key_next) ? 1 : -1;
                if (muisc::next_mode_in(muisc::read_mode_config(), 1, dir) == 1) {
                    ui.model.scope_notice = "the radio is the only mode switched on (Settings -> GENERAL -> MODES)";
                    ui.model.scope_notice_until = ui.model.t_sec + 4;
                    continue;
                }
                g_mode_switch_dir = dir;
                switch_mode = true; running = false; continue;
            }
            {   // Timeshift: pause / jump back and forth / back to live. Matched by the bound key itself (the key table's
                // translation maps a key to the first letter of its default, which "SPACE" would turn into 'S').
                auto mmss = [](double sec) {
                    const long long t = std::max(0LL, static_cast<long long>(sec + 0.5));
                    char b[24];
                    if (t >= 3600) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
                    else std::snprintf(b, sizeof b, "%lld:%02lld", t / 60, t % 60);
                    return std::string(b);
                };
                auto show = [&](const std::string& what) {
                    const RadioStatus a = engine.status();
                    std::string where = a.timeshift_paused ? "paused, " + mmss(a.behind_sec) + " behind live"
                                      : a.timeshift_shifted && a.behind_sec >= 2.5 ? mmss(a.behind_sec) + " behind live" : "live";
                    ui.model.scope_notice = what.empty() ? where : what + "  |  " + where;
                    ui.model.scope_notice_until = ui.model.t_sec + 4;
                };
                const RadioStatus ts = engine.status();
                const bool tuned = ts.state != StreamState::Idle && ts.state != StreamState::Failed;
                auto jump_by = [&](double sec) {
                    if (!tuned) { ui.model.notice = "timeshift: tune a station first"; notice_until = ui.model.t_sec + 4; return; }
                    const double moved = engine.jump(sec);
                    char b[48];
                    if (std::fabs(moved) < 0.5) std::snprintf(b, sizeof b, sec < 0 ? "start of the buffer" : "already live");
                    else std::snprintf(b, sizeof b, "%s%s", moved < 0 ? "-" : "+", mmss(std::fabs(moved)).c_str());
                    show(b);
                };
                if (k == key_of(cfg, "Pause")) {
                    if (!tuned) { ui.model.notice = "timeshift: tune a station first"; notice_until = ui.model.t_sec + 4; continue; }
                    engine.toggle_pause();
                    show(engine.status().timeshift_paused ? "PAUSE" : "PLAY");
                    continue;
                }
                if (k == key_of(cfg, "Back30")) { jump_by(-30); continue; }
                if (k == key_of(cfg, "Fwd30")) { jump_by(30); continue; }
                if (k == key_of(cfg, "Back5m")) { jump_by(-300); continue; }
                if (k == key_of(cfg, "GoLive")) {
                    if (tuned) { engine.go_live(); show("LIVE"); }
                    continue;
                }
            }
            switch (key_translate(cfg, k)) {
                case 'q': case 3: case kKeyCtrlC: running = false; break;   // q is free now that the preset keys are 1234567890ertdfg
                case kKeyDegree: adev_open(false); break;   // CHOOSE AUDIO DEVICE ("°" / "&" by default; key_translate() hands its key over as this)
                case 13: case 10: tune_visible(ui.model.cursor); break;
                case 'n': tune_relative(+1); break;       // always the next channel in the list
                case 'b': tune_back(); break;              // previous channel; depends on the S/L mode
                case '#': tune_shuffle(); break;           // always a random next channel
                case 'm': ui.model.shuffle = !ui.model.shuffle; cfg.playback_shuffle = ui.model.shuffle; ui.model.settings.dirty = true; break;      // S (shuffle) <-> L (list)
                case 'p':   // mute / unmute -- but with nothing loaded it tunes the hovered station, like ENTER
                    if (engine.status().state == StreamState::Idle) tune_visible(ui.model.cursor);
                    else engine.set_muted(!engine.status().muted);
                    break;
                case '+': engine.set_volume(engine.status().volume + 5); break;
                case '=':                                  // SHIFT+0 on a German keyboard: the big oscilloscope overlay
                    ui.model.scope_big = !ui.model.scope_big;
                    ui.model.spectro_full = false;
                    ui.model.scope_big_color_edit = false;
                    ui.model.scope_metrics_open = false;
                    out_clear_next = true;
                    break;
                case '-': case '_': engine.set_volume(engine.status().volume - 5); break;
                case 'x': engine.stop(); break;
                case 's': settings_open(); break;      // the RADIO SETTINGS screen
                case 'L': ui.model.stov = StationsOverlay{}; ui.model.stov.open = true; break;   // the big STATIONS overlay
                case ')': toggle_scope_window(); break;     // SHIFT+9: the oscilloscope in its own window
                case '(': toggle_spectro_window(); break;   // SHIFT+8: the spectrogram in its own window
                case '.':                              // the scope block: oscilloscope -> sphere -> spectrogram -> oscilloscope
                    // (what is switched off on the GENERAL tab is skipped; the sphere is always there)
                    for (int n = 0; n < 3; ++n) {
                        cfg.scope_mode = cfg.scope_mode == 0 ? 1 : cfg.scope_mode == 1 ? 3 : 0;
                        if ((cfg.scope_mode == 0 && !cfg.use_osci) || (cfg.scope_mode == 3 && !cfg.use_spectro)) continue;
                        break;
                    }
                    ui.model.settings.dirty = true;
                    break;
                case 'B': ui.model.overlay = 10; ui.model.overlay_row = 0; ui.model.bars_scroll = 0; break;   // Shift+B: spectrum bars options
                case 'I': ui.model.overlay = 6; ui.model.overlay_row = kSrView; break;   // Shift+I: spectrogram options
                case 'U':                              // Shift+U: spectrogram full screen
                    ui.model.spectro_full = !ui.model.spectro_full;
                    if (ui.model.spectro_full) ui.model.scope_big = false;
                    out_clear_next = true;
                    break;
                case 'h': {                            // the LISTENING HISTORY
                    HistoryModel& hm = ui.model.hmenu;
                    hm.open = true; hm.flash.clear(); hm.cursor = 0; hm.top_cursor = 0; hm.scroll = 0;
                    break;
                }
                case 'y': {                            // record the tuned stream: y while recording stops, else the record overlay
                    const RadioStatus rs = engine.status();
                    if (rs.recording) { engine.stop_recording(); ui.model.notice = "recording stopped: converting to MP3 ..."; notice_until = ui.model.t_sec + 6; break; }
                    if (rs.state == StreamState::Idle || rs.state == StreamState::Failed) { ui.model.notice = "nothing to record: tune a station first"; notice_until = ui.model.t_sec + 4; break; }
                    // "From now on" means the moment this overlay opened: that position is fixed here, so no time is lost
                    // while choosing.
                    ui.model.rec_anchor = rs.heard_frame;
                    ui.model.rec_avail_sec = std::max(0.0, static_cast<double>(rs.heard_frame - rs.oldest_frame) / 48000.0);
                    ui.model.overlay = 5;
                    ui.model.overlay_row = 0;
                    break;
                }
                case 'v': cfg.normalize = !cfg.normalize; apply_audio(); ui.model.settings.dirty = true; break;   // loudness normalisation on/off
                case 'V': ui.model.overlay = 2; ui.model.overlay_row = 0; break;   // Shift+V: loudness normalisation overlay
                case 'O': ui.model.overlay = 1; ui.model.overlay_row = 0; break;   // Shift+O: oscilloscope overlay
                case 'E': ui.model.overlay = 4; ui.model.overlay_row = 0; eq_open(cfg, ui.model.eq); break;   // Shift+E: equaliser (the preset key is the lowercase e)
                case 'Z': {                            // Shift+Z: sleep timer
                    ui.model.overlay = 3;
                    ui.model.overlay_row = 0;
                    for (int i = 0; i < 5; ++i) if (sleep_minutes == kSleepChoice[i]) ui.model.overlay_row = i;
                    break;
                }
                case 'R': engine.reconnect(); break;                          // also for a Radio Browser station that is not in the list
                case '/': set_search_focus(true); break;
                case '?': ui.model.cheat_open = true; ui.model.cheat_scroll = 0; break;
                case '<': ui.model.overlay = 7; break;   // the MENUS index: every menu and overlay with its key
                case 'W':                         // Shift+W: the help wizard, as in the music player
                    wizard_open_now();
                    break;
                case 'S': browse_open(); break;   // Shift+S: the RADIO BROWSER search menu
                case 'K': lm_open(); break;       // Shift+K: the STATION LISTS menu
                case '"': {                        // SHIFT+2 on a German keyboard: Latin letters on / off (the same switch as in the music player)
                    const bool on = !muisc::translit_enabled();
                    muisc::set_translit_enabled(on);
                    muisc::write_latin_letters(on);   // config.txt: kept for the next start and for the player
                    ui.model.notice = on ? "Latin letters: on (Japanese kana, Korean and Chinese text in Latin letters)" : "Latin letters: off";
                    notice_until = ui.model.t_sec + 4;
                    break;
                }
                case 'T': ui.toggle_sort(); break; // Shift+T: STATIONS sort, list order <-> name A-Z (search results too)
                case 'P': ui.model.menu.open = true; ui.model.menu.focus = MenuFocus::Search; ui.model.menu.target = SearchTarget::Stations;
                          ui.model.menu.flash.clear(); ui.model.menu.name = NameOverlay{};
                          ui.refilter_menu(); ui.refilter_presets(); ui.cursor_to_active_preset();
                          ui.model.menu.search_edit.to_end(ui.model.menu.search); ui.model.menu.psearch_edit.to_end(ui.model.menu.psearch);
                          set_text_entry(true); break;
                default:
                    const int slot = preset_slot_for_key(k);
                    if (slot >= 0) {
                        const int idx = ui.model.presets[static_cast<size_t>(slot)];
                        if (idx >= 0) tune_to(idx);
                    }
                    break;
            }
        }

        const auto now = Clock::now();
        ui.model.dt = std::chrono::duration<double>(now - last).count();
        ui.model.t_sec = std::chrono::duration<double>(now - t0).count();
        last = now;
        // The wizard's welcome disk: a fixed 0.10, the same speed the player
        // spins it at, and turning whether or not a station is playing.
        if (ui.model.wizard_open)
            ui.model.wizard_angle = std::fmod(ui.model.wizard_angle + (2.0 * 3.14159265358979323846 / 48.0) / 0.035 * 0.10 * ui.model.dt,
                                              2.0 * 3.14159265358979323846);

        const int rows = term.rows(), cols = term.cols();
        std::string out;
        if (rows != last_rows || cols != last_cols || out_clear_next) { out += "\x1b[2J"; last_rows = rows; last_cols = cols; out_clear_next = false; }
        if (cols < kUiCols || rows < kUiRows) {
            last_too_small = true;
            const std::string msg = "Terminal too small: radio mode needs " + std::to_string(kUiCols) + "x" + std::to_string(kUiRows)
                                  + " (this one is " + std::to_string(cols) + "x" + std::to_string(rows) + ")";
            out += "\x1b[H\x1b[2J" + msg;
        } else {
            if (last_too_small) { out += "\x1b[2J"; last_too_small = false; }
            out += "\x1b[H";
            ui.model.cols = cols;
            ui.model.rows = rows;
            {   // cell size in pixels (it changes with the font and the zoom), and whether this frame sends a new picture
                gfx_cell_pixels(cfg.cell_pixels, ui.model.cell_w, ui.model.cell_h);
                ++gfx_tick;
                // how many pictures per second the terminal is sent: Kitty up to 60, Sixel 30 (full) / 60 (half, third)
                const int cap = gfx_picture_cap(gfx, cfg.image_scale);
                const unsigned long every = static_cast<unsigned long>(std::max(1, (cfg.osci().frame_rate + cap / 2) / cap));
                ui.model.gfx_due = (gfx_tick % every) == 0;
            }
            terminal_black_background(cfg.black_background);   // Settings -> GENERAL -> MODES (only writes when it changes)
            terminal_default_text(cfg.list_fg);                 // text without a colour of its own: the list text colour
            auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
            // Lines are joined with "\r\n"; the LAST line gets no newline, so a frame that fills the
            // whole terminal can never scroll. No per-line "\x1b[K" either: a line that fills the last
            // column leaves the cursor in the pending-wrap state there, and "erase to end of line"
            // would wipe that last character.
            {   // only the lines that changed (at 120 / 165 fps the whole screen every frame would be megabytes per second);
                // the whole frame after a clear, when the layout changed, and every 2 s
                const bool cleared = out.find("\x1b[2J") != std::string::npos;
                const bool heal = now - last_full_frame > std::chrono::seconds(2);
                std::string body;
                for (size_t i = 0; i < frame.size(); ++i) { body += frame[i]; if (i + 1 < frame.size()) body += "\r\n"; }
                std::string d;
                if (diff_frame_lines(body, prev_lines, d) && !cleared && !heal) out += d;
                else { out += body; last_full_frame = now; }
            }
            // Sixel: the text skips the picture's cells, so the picture on screen stays and is only sent when there is a new
            // one -- or when it is not on screen yet / the screen was cleared (then the current one is sent at once).
            const int sp_proto = gfx == GfxProto::Kitty ? 1 : gfx == GfxProto::Sixel ? 2 : 0;
            const bool sp_img = ui.model.spectro_area[0] >= 0 && sp_proto != 0;
            if (!ui.model.spectro_used) spectro().set_visible(false);
            if (out.find("\x1b[2J") != std::string::npos) spectro_gfx.invalidate();
            const bool sixel_on = (gfx == GfxProto::Sixel && ui.model.gfx.active) || (sp_img && sp_proto == 2);
            if (sixel_on && (!gfx_shown || out.find("\x1b[2J") != std::string::npos)) { ui.model.gfx.fresh = true; ui.model.gfx2.fresh = true; }
            gfx_fill_holes(out, sixel_on);
            // the image goes after the text (Sixel replaces the cells; a Kitty image sits under the text and is replaced in place)
            if (ui.model.gfx.active && gfx != GfxProto::None) { out += gfx_emit(gfx, ui.model.gfx); gfx_shown = ui.model.gfx.crop < ui.model.gfx.cols; }
            else if (gfx_shown) { out += gfx_clear(gfx); gfx_shown = false; }
            if (gfx == GfxProto::None && gfx_shown) gfx_shown = false;
            if (ui.model.gfx.active && ui.model.gfx2.active && gfx != GfxProto::None) {
                if (ui.model.gfx2.fresh || !gfx2_shown) { out += gfx_emit(gfx, ui.model.gfx2); gfx2_shown = true; }
            } else if (gfx2_shown) {
                out += gfx_clear(gfx, 1); gfx2_shown = false;
                // Sixel cannot delete a picture: the overlay's pictures stay until their cells are written over, so the next
                // frame is a full one and the main picture is sent again
                if (gfx == GfxProto::Sixel) { prev_lines.clear(); gfx_shown = false; }
            }
            if (sp_img) {
                const int* a = ui.model.spectro_area;
                const int* sk = ui.model.spectro_skip;
                out += spectro_gfx.emit(sp_proto, cfg.spectro, spectro(), a[0], a[1], a[2], a[3], std::max(1, ui.model.cell_w),
                                        std::max(1, ui.model.cell_h), true, sk[0], sk[1], sk[2], sk[3]);
            } else if (spectro_gfx.shown()) out = spectro_gfx.clear(sp_proto) + out;   // before the new screen's text
        }
        write_frame(out);
        // frame pacing: one frame every 1 / frame_rate seconds (the render time counts), never a burst to catch up
        next_frame += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / std::clamp(cfg.osci().frame_rate, 30, 165)));
        const auto after = Clock::now();
        if (next_frame < after) next_frame = after;
        std::this_thread::sleep_until(next_frame);
    }
    muisc::remote::stop();
    set_text_entry(false);
    if (ui.model.settings.dirty) settings_save();
    engine.stop();
    rhist.tick(engine.status(), 0.0, static_cast<long long>(std::time(nullptr)));   // closes the live line
    rhist.close_live();
    if (gfx_shown) { write_frame(gfx_clear(gfx)); gfx_shown = false; }
    if (gfx2_shown) { write_frame(gfx_clear(gfx, 1)); gfx2_shown = false; }
    if (spectro_gfx.shown()) write_frame(spectro_gfx.clear(gfx == GfxProto::Kitty ? 1 : 2));   // nothing of it may outlive the radio
    if (switch_mode) terminal_hold_alt_screen();   // the player takes the screen over: no flash of the shell in between
    return switch_mode ? kExitSwitchMode : 0;
}
