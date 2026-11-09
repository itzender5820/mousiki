// The radio's main loop, as a class: what radio_main() once kept in local variables and lambdas (the settings, the engine, the overlays' state,
// the tuning, the stations overlay, the browser, the settings screen ...) are members and methods now, and the keys are handled by one method per
// overlay or menu (radio_app_keys*.cpp). Behaviour is the same as before the split; see radio_main.cpp for the entry point.
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
#include "radio_main_support.h"
#include <map>
#include <random>
#include "radio_main_support.h"

// What the loop of the keys passes on to the key methods: the key (a method may change it), whether it was an arrow, how many stations are
// visible, and the key that a menu hands back to be handled next (as if it had just been typed).
struct KeyCtx {
    int& k;
    bool arrow;
    int nvis;
    int& menus_inject;
};

struct RadioApp {
    // ---- the state (in the order the locals of radio_main() were made)
    std::string cfg_source;
    RadioSettings cfg;
    Ui ui;
    std::string source;
    StoragePaths storage;
    RadioEngine engine;
    std::string timeshift_file;
    int timeshift_applied = -1;
    RadioHistory rhist;
    RadioYt yt;
    RadioBrowser browser;
    bool yt_jump = false;                 // after a YouTube search from the overlay: move to the results when they arrive
    double notice_until = 0.0;
    bool update_pending = false;   // a newer version was found: the overlay opens when the main screen is showing
    bool scope_win_was = false;
    std::vector<std::string> prev_lines;               // the screen's lines as last sent (diff_frame_lines)
    bool out_clear_next = false;                       // the next frame clears the screen (full-screen spectrogram on / off)
    SpectroGfx spectro_gfx;                            // the spectrogram picture (tiles)
    Clock::time_point last_full_frame{};   // the window may have been opened in the player already
    unsigned long scope_win_tick = 0;
    bool spectro_win_was = false;
    int record_serial_seen = 0;
    static constexpr int kSleepChoice[5] = {15, 30, 60, 90, 120};
    int sleep_minutes = 0;
    Clock::time_point sleep_deadline{};
    double sleep_fade_len = 0.0;
    double sleep_unit = 60.0;   // (MOUSIKI_RADIO_SLEEP_UNIT = seconds per "minute", 60 normally; the tests set it to 1.)
    GfxProto gfx = GfxProto::None;
    bool gfx_shown = false;
    bool gfx2_shown = false;   // the second picture of the big oscilloscope overlay (Kitty image id 1)
    Clock::time_point next_frame{};
    unsigned long gfx_tick = 0;
    bool running = true;
    bool switch_mode = false;
    bool last_too_small = false;
    int last_rows = 0, last_cols = 0;
    Clock::time_point t0{};
    Clock::time_point last{};
    std::vector<int> history;
    std::mt19937 rng;
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
    unsigned browse_state_seen = 0;           // last BrowseState we reacted to (as int)
    unsigned browse_gen_seen = 0;
    bool browse_jump_to_results = false;      // ENTER in a pane: when the results arrive, move to RESULTS
    struct KeyUndo { std::map<std::string, std::string> keys; int row; };
    std::vector<KeyUndo> key_undo;
    RadioSettings settings_snapshot;
    bool settings_snapshot_valid = false;

    // ---- the whole run
    RadioApp();                                   // loads the settings and the stations; nothing is shown yet
    int headless(int argc, char** argv, const std::string& mode);   // --dump, --selftest ...: -1 when `mode` is none of them
    // (one per mode of headless(): the dump of a screen, the selftest ...)
    int headless_dump(int argc, char** argv);
    int headless_dump_menu(int argc, char** argv);
    int headless_dump_lists(int argc, char** argv);
    int headless_dump_stations(int argc, char** argv);
    int headless_dump_history(int argc, char** argv);
    int headless_dump_settings(int argc, char** argv);
    int headless_dump_overlay(int argc, char** argv);
    int headless_dump_cheat(int argc, char** argv);
    int headless_dump_browse(int argc, char** argv);
    int headless_rb_search(int argc, char** argv);
    int headless_selftest(int argc, char** argv);
    int interactive();                            // the screen: the loop of frames and keys
    void interactive_setup();                     // what the loop needs before the first frame
    void frame_tick();                            // the remote app, the notices, the windows, the timers (once per frame)

    // ---- the work (radio_app_core.cpp, radio_app_stations.cpp, radio_app_settings.cpp)
    bool lm_in_text();
    bool settings_changed();
    bool theme_changed();
    static std::filesystem::path theme_dir_path(std::string dir);
    static std::string n_stations(size_t n);
    std::string stov_persist_all();
    std::string window_key_label(const char* id);
    std::vector<int> surf_pool();
    void adev_close();
    void adev_open(bool from_settings);
    void adev_pick();
    void apply_audio();
    void apply_timeshift();
    void browse_add(const BrowseStation& b);
    void browse_close();
    void browse_focus(BrowseFocus f);
    void browse_open();
    void browse_search();
    void browse_tune(const BrowseStation& b);
    void key_change_begin(int row);
    void key_change_end(bool changed);
    void lm_add();
    void lm_close();
    void lm_delete();
    void lm_focus(ListFocus f);
    void lm_load();
    void lm_mfocus(ListManageFocus f);
    void lm_move(int dir);
    void lm_open();
    void lm_remove();
    void lm_save(bool leave);
    void lm_sync_entry();
    void load_storage();
    void remote_apply(const muisc::remote::Command& c);
    void set_search_focus(bool on);
    void settings_close(bool save);
    void settings_open();
    void settings_save();
    void sleep_cancel();
    void sleep_start(int minutes);
    void start_record(long long from_frame, double seconds_before_now);
    void storage_changed();
    void stov_add_commit();
    void stov_delete(int idx);
    void stov_edit_commit();
    void stov_edit_open(int idx);
    void stov_refresh();
    void stov_resync_tuned();
    void stov_snapshot(const std::string& what);
    void stov_undo_do();
    void theme_io_rescan();
    void theme_open_creator(bool edit_current);
    void theme_save_now();
    void theme_try_save();
    void toggle_scope_window();
    void toggle_spectro_window();
    void tune_back();
    void tune_relative(int delta);
    void tune_shuffle();
    void tune_to(int idx, bool record = true);
    void tune_visible(int vis_pos);
    void wizard_open_now();
    void wizard_seen_save();

    // ---- the keys (radio_app_keys*.cpp): true when the key was used
    bool key_wizard(KeyCtx& kc);
    bool key_cheat(KeyCtx& kc);
    bool key_adev(KeyCtx& kc);
    bool key_settings(KeyCtx& kc);
    bool key_settings_theme(KeyCtx& kc);   // (a part of key_settings: the THEME picker / CREATE THEME overlay)
    bool key_settings_edit(KeyCtx& kc);    // (a part of key_settings: a field is being edited)
    bool key_history(KeyCtx& kc);
    bool key_stations_dialogs(KeyCtx& kc);
    bool key_overlays_a(KeyCtx& kc);
    bool key_overlays_b(KeyCtx& kc);
    bool key_browse(KeyCtx& kc);
    bool key_lists_menu(KeyCtx& kc);
    bool key_presets_menu(KeyCtx& kc);
    bool key_search(KeyCtx& kc);
    bool key_stations_overlay(KeyCtx& kc);
    bool key_main(KeyCtx& kc);
};
