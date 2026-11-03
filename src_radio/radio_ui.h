#pragma once
#include "audio_backend.h"
// Radio mode -- the 120x30 screen. Pure rendering: takes a model and a status
// snapshot, returns exactly `rows` lines of exactly `cols` visible columns each.
// With UiModel::menu.open the whole frame is the PRESETS menu instead of the radio screen;
// with UiModel::browse.open it is the RADIO BROWSER search menu (Shift+S);
// with UiModel::lmenu.open it is the STATION LISTS menu (Shift+K);
// with UiModel::cheat_open it is the CHEATSHEET (wins over both menus).
// with UiModel::settings.open it is the RADIO SETTINGS screen (key `s`; tabs COLORS and GENERAL).
// Every colour comes from the radio's own radio_config.txt (RadioSettings, radio_settings.h) -- nothing is read from
// the music player's config.txt.
#include "scope_window.h"
#include "wizard.h"
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include "radio_browser.h"
#include "radio_engine.h"
#include "radio_stations.h"
#include "radio_textedit.h"
#include "radio_eq.h"
#include "radio_settings.h"
#include "terminal_gfx.h"
#include "radio_history.h"
#include "radio_yt.h"
#include "remote_server.h"
#include "remote_overlay.h"

namespace muisc::radio {

// Minimum terminal size. The frame is exactly as tall as the terminal (30 lines at 30 rows); its last
// line is written WITHOUT a trailing newline, so it can never scroll. Wider terminals only widen the
// panes (up to kMaxCols); taller ones only give rows to the STATIONS / PRESETS boxes -- the top pane,
// band, buttons and search keep their height.
constexpr int kUiCols = 120;
constexpr int kUiRows = 30;
constexpr int kMaxCols = 200;

// Colour wave over the ON AIR sign. The sign always carries a top-left -> bottom-right gradient of
// the visualizer colours; this adds a travelling wave on top. Meant to become settings later.
struct SignWave {
    float speed = 0.25f;   // wave cycles per second (0 = static gradient)
    float waves = 1.5f;    // wave crests across the sign's diagonal
    float amount = 0.35f;  // 0 = plain gradient, 1 = only the wave
};

// Which pane of the PRESETS menu has the keyboard (TAB cycles SEARCH -> SELECT PRESET -> STATIONS).
enum class MenuFocus { Search, Presets, Stations };

// What the menu's search box filters. "s:" = stations (default), "p:" = presets (typed into the empty box;
// the older "/s:" / "/p:" spellings still work).
enum class SearchTarget { Stations, Presets };

// The small centred overlay that asks for a preset name (Shift+N = new, Shift+C = rename). While it is
// open it owns every key.
struct NameOverlay {
    bool open = false;
    bool rename = false;      // false: creating a new preset, true: renaming `target`
    int target = -1;          // preset index being renamed
    std::string text;
    EditState edit;           // caret + marked range inside `text`
    std::string error;        // shown in the overlay's last row ("name already exists" ...)
};

// State of the big PRESETS menu (opened with P). It has its own searches + cursors, independent of the
// main screen's, so opening and closing it never disturbs the list underneath.
struct MenuModel {
    bool open = false;
    MenuFocus focus = MenuFocus::Search;
    SearchTarget target = SearchTarget::Stations;
    std::string search;           // station search (s:)
    EditState search_edit;        // caret + marked range inside `search`
    std::vector<int> visible;     // station indices passing `search`, in list order
    int cursor = 0;               // position within `visible`
    std::string psearch;          // preset search (p:)
    EditState psearch_edit;       // caret + marked range inside `psearch`
    std::vector<int> pvisible;    // preset (bank) indices passing `psearch`, in list order
    int pcursor = 0;              // position within `pvisible`
    NameOverlay name;
    int del_confirm = -1;         // preset (bank) index waiting for the second SHIFT+d
    std::string flash;            // one status line under the hints ("Preset 3 = ...")
    mutable int marquee_row = -1;
    mutable double marquee_since = 0.0;
};

// Both lower panes of the menu are exactly this many content rows (boxes are two taller).
constexpr int kMenuListRows = 16;

// SELECT PRESET pane: 4 columns, this many content rows (so 8 presets are visible, it scrolls by rows).
constexpr int kPresetPaneCols = 4;
constexpr int kPresetPaneRows = 2;

// ---- RADIO BROWSER menu (Shift+S) ---------------------------------------------------------------------
// Six input panes (two per row) with the search, below them RESULTS and the hovered station's info.
// TAB cycles NAME -> TAGS -> COUNTRY -> STATE -> LANGUAGE -> BITRATE -> RESULTS.
enum class BrowseFocus { Name = 0, Tags, Country, State, Language, Bitrate, Results };
constexpr int kBrowseFields = 6;

struct BrowseModel {
    bool open = false;
    BrowseFocus focus = BrowseFocus::Name;
    std::string text[kBrowseFields];      // what is typed into the six panes (same order as BrowseFocus)
    EditState edit[kBrowseFields];        // caret + marked range of each
    int cursor = 0;                       // hovered result
    // Refreshed from RadioBrowser::snapshot() before every frame.
    BrowseState state = BrowseState::Idle;
    std::string message;                  // error text / "no stations found"
    std::string server;                   // host that answered
    std::shared_ptr<const std::vector<BrowseStation>> results;
    std::string flash;                    // status line ("Added ...")
    mutable int marquee_row = -1;
    mutable double marquee_since = 0.0;
};

// ---- STATION LISTS menu (Shift+K) ---------------------------------------------------------------------
// Laid out like the player's playlist editor, two tabs (Left/Right -- see `typing` below -- or ALT+Left/Right):
//   1: CREATE / EDIT      name field | SEARCH ALL STATIONS | STATIONS (library) beside LIST CONTENTS (what is being built)
//   2: SAVED STATION LISTS  SEARCH STATION LISTS | STATION LISTS (ENTER loads the hovered one into tab 1)
// TAB cycles the panes of the current tab.
enum class ListFocus { Name, Search, Stations, Contents };      // tab 1
enum class ListManageFocus { Search, List };                    // tab 2

struct ListMenuModel {
    bool open = false;
    int tab = 0;                          // 0 = CREATE / EDIT, 1 = SAVED STATION LISTS
    // --- tab 1: the list being built
    ListFocus focus = ListFocus::Name;
    std::string name;                     // the list's name (what CTRL+s saves it under)
    EditState name_edit;
    std::vector<int> items;               // stations of the list being built, in the order they were added
    int item_cursor = 0;
    std::string search;                   // SEARCH ALL STATIONS (name, genre, country)
    EditState search_edit;
    std::vector<int> visible;             // station indices passing `search`, sorted by `sort_az`
    int cursor = 0;                       // position within `visible`
    bool sort_az = false;                 // SHIFT+t in STATIONS: false = list order (as added to stations.txt), true = name A-Z
    bool dirty = false;                   // unsaved changes in tab 1 (ESC asks first)
    // The focused text box (name / search, either tab) is IN USE once something was typed or edited in it: from then
    // on Left/Right move its caret; before that -- and again after ESC / TAB / ENTER -- they switch the tab.
    bool typing = false;
    // --- tab 2: the saved lists
    ListManageFocus mfocus = ListManageFocus::List;
    std::string msearch;
    EditState msearch_edit;
    std::vector<int> mvisible;            // indices into UiModel::lists passing `msearch`
    int mcursor = 0;
    // --- prompts (shown instead of the hint lines, like the player's "Save changes ... before exiting?")
    bool confirm_exit = false;
    bool confirm_delete = false;
    std::string flash;                    // status line under the hints
    mutable int marquee_row = -1;         // hovered row of STATIONS / LIST CONTENTS / STATION LISTS (one pane is hovered at a time)
    mutable double marquee_since = 0.0;
    mutable int marquee2_row = -1;
    mutable double marquee2_since = 0.0;
};

// The RADIO SETTINGS screen (key `s`). The renderer only needs the cursor and the cell being edited; the values
// themselves live in the RadioSettings that is passed to render_radio_frame().
struct SettingsModel {
    bool open = false;
    int tab = 0;                  // 0 = COLORS, 1 = GENERAL, 2 = ANIMA/VIZ, 3 = PATHS
    int row = 0;                  // COLORS: 0 .. kColorRowCount-1, GENERAL: 0 .. kOnOffRowCount-1
    int col = 0;                  // 0 = first cell of the row, 1 = second cell
    bool editing = false;
    std::string buffer;           // text of the cell being edited
    EditState edit;
    std::string status;           // last line under the hints ("Saved ...", "Not a colour: ...")
    bool dirty = false;           // changed since the last save
    // --- colour themes (COLORS tab, last row: THEME | CREATE THEME) ---
    // theme_ui: 0 = the plain tab | 1 = pick a theme to apply | 2 = the CREATE THEME overlay | 3 = pick a template
    int theme_ui = 0;
    muisc::remote_ui::State remote_ov;   // GENERAL -> REMOTE / UPDATES -> Remote App: the REMOTE APP overlay
    // CHOOSE AUDIO DEVICE: from GENERAL -> AUDIO ([ENTER]; drawn over the settings) or from the main screen (its key, overlay 9).
    bool adev_open = false;
    bool adev_from_settings = false;
    int adev_sel = 0;                                 // 0 = System Default, 1.. = adev_list
    std::vector<muisc::AudioOutputDevice> adev_list;  // read when it opens ([r] reads it again)
    std::string adev_err;
    std::string adev_active;                          // the output the engine has open (for the "in use" mark)
    bool theme_peek = false;      // SHIFT+p in the picker: the main screen in the highlighted theme (any key closes it)
    std::vector<themes::Theme> theme_list;
    int theme_sel = 0;
    RadioSettings theme_draft;    // the creator edits a copy
    themes::Theme theme_base;     // the template whose other keys the new theme keeps
    std::string theme_name;       // 25 characters at most
    std::string theme_name_initial;                                       // the NAME the creator opened with ("" new, the theme in use for [e])
    std::vector<std::pair<std::string, std::string>> theme_initial_kv;    // the draft as the creator opened (to see whether anything changed)
    int leave_prompt = 0;         // 1: "unsaved settings" question on ESC, 2: "unsaved theme" question (CREATE THEME), 3: "overwrite the theme?", 4: "delete the theme?" (CHOOSE THEME)
    int theme_row = -1;           // creator: -1 = the NAME / TEMPLATE row, else the COLORS row
    int theme_col = 0;
    // SHIFT+e / SHIFT+i: theme_ui 4 = export the current colours as a theme file, 5 = import theme files
    std::string theme_io_name, theme_io_dir;
    int theme_io_row = 0;         // export: 0 name, 1 folder | import: 0 folder, 1 file list
    int theme_io_sel = 0;
    std::vector<std::filesystem::path> theme_io_files;
};

// ---- LISTENING HISTORY menu (Shift+H): 1 HISTORY | 2 TOP CHANNELS | 3 HABITS ----------------------------------
// On the HISTORY tab `y` opens a small overlay: the hovered entry's "artist title" is searched on YouTube and a result
// can be downloaded into the download folder (the music player's own search + yt-dlp download, see radio_yt.h).
struct HistoryModel {
    bool open = false;
    int tab = 0;                          // 0 = HISTORY, 1 = TOP CHANNELS, 2 = HABITS
    int cursor = 0;                       // HISTORY: hovered line (0 = newest)
    int top_cursor = 0;                   // TOP CHANNELS: hovered row
    bool most_first = true;               // TOP CHANNELS order (r flips it)
    mutable int scroll = 0;               // HABITS: first visible line (clamped while rendering)
    std::string flash;                    // status line
    const RadioHistory* hist = nullptr;   // the store (owned by main)
    long long now = 0;                    // unix seconds, for "today" and the busiest-hour figures
    // --- the YouTube overlay
    bool yt_open = false;
    std::string yt_text;                  // the query (prefilled with "artist title")
    EditState yt_edit;
    bool yt_in_field = true;              // false: the results list has the keys
    int yt_cursor = 0;
    YtSnapshot yt;                        // refreshed from RadioYt::snapshot() before every frame
    std::string yt_dir;                   // where a download goes (shown in the overlay)
};

// ---- the big STATIONS overlay (key L) ----------------------------------------------------------------
// An expansion of the main STATIONS pane: it shows (and edits) the very same state as the main screen -- the same search
// box (`/`, `p:` searches the station lists, ENTER opens one), `visible`, `cursor`, sort and active list -- just with the
// whole screen for the rows and one more column (the PRESET NAME). `a` adds a station by its stream URL, SHIFT+c sets the
// hovered station's second name for the PRESETS pane.
struct StationsOverlay {
    bool open = false;
    std::string flash;
    mutable int marquee_row = -1;
    mutable double marquee_since = 0.0;
    // `a`: add a station by URL (TAB switches URL <-> NAME; an empty NAME becomes the host name)
    bool add_open = false;
    int add_field = 0;
    std::string add_url, add_name, add_error;
    EditState add_edit[2];
    // SHIFT+c: the PRESET NAME of station `alias_idx` (empty = back to the station's own name)
    bool alias_open = false;
    int alias_idx = -1;
    std::string alias_text;
    EditState alias_edit;
};

// The record overlay's choices (y): seconds back from the moment it opened; 0 = from now on, -1 = the whole buffer.
inline constexpr int kRecordBack[] = {0, 60, 300, 600, 900, 1800, -1};

struct UiModel {
    // Terminal graphics (oscilloscope "image" style): what the terminal speaks, its cell size in pixels, and the picture the
    // last frame produced (written by render_radio_frame, sent by the main loop).
    GfxProto gfx_proto = GfxProto::None;
    int cell_w = 10, cell_h = 20;
    bool gfx_due = true;
    mutable GfxFrame gfx;
    mutable GfxFrame gfx2;   // the big oscilloscope overlay shows two pictures (gfx = YT, gfx2 = XY)
    mutable int scope_cut_last[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    mutable int gfx_last_crop = -1;
    const std::vector<Station>* stations = nullptr;
    std::vector<int> presets;     // the ACTIVE preset's slots: kPresetCount station indices (-1 = empty), slot i <-> kPresetKeys[i]
    std::vector<PresetBank> banks;// all presets (names + slots); banks[bank_active].slots is only refreshed from `presets` on save/switch
    int bank_active = 0;
    MenuModel menu;
    BrowseModel browse;
    std::vector<StationList> lists;   // the saved station lists (stationlists.txt)
    ListMenuModel lmenu;
    SettingsModel settings;
    HistoryModel hmenu;
    StationsOverlay stov;
    mutable int pmarquee_row = -1;       // the tuned preset entry whose name scrolls in the PRESETS pane (-1 = none)
    mutable double pmarquee_since = 0.0;
    // Small overlay over the main screen: 1 = oscilloscope tuning (SHIFT+o), 2 = loudness normalisation (SHIFT+v).
    int overlay = 0;                // 8 = "a newer version is out" (update_check.h), 3 = sleep timer (SHIFT+z), 4 = equaliser (SHIFT+e), 5 = record (y), 6 = spectrogram (SHIFT+i), 7 = the MENUS index (<)
    // Spectrogram: full screen (SHIFT+u); where this frame put its picture (row, col, cols, rows; row -1 = none), the
    // cell rectangle an overlay covers, and whether one was drawn at all (else the feed is switched off).
    bool spectro_full = false;
    bool scope_big = false;                // SHIFT+0: the big oscilloscope overlay (YT left, XY right)
    bool scope_metrics_open = false;       // SHIFT+p: the grid metrics window
    int scope_metrics_row = 0;
    bool scope_big_color_edit = false;     // SHIFT+c: the grid colour is being typed
    std::string scope_big_color_buf;
    mutable int spectro_area[4] = {-1, 0, 0, 0};
    mutable int spectro_skip[4] = {0, 0, 0, 0};
    mutable bool spectro_used = false;
    // Record overlay (y): the stream position and the buffered time when it opened, so "from now on" is exactly then.
    long long rec_anchor = 0;
    double rec_avail_sec = 0.0;     // how far back from rec_anchor the buffer reaches
    // Timeshift jumps: a short note at the bottom of the scope block ("-30 s  |  2:30 behind live"), 4 s.
    std::string scope_notice;
    double scope_notice_until = 0.0;
    EqUi eq;                        // the equaliser overlay's own state
    int sleep_running = 0;          // the minute choice that is running (0 = no timer)
    double sleep_left = 0.0;        // seconds until it stops
    double sleep_fade_sec = 0.0;    // length of the fade-out of the running / selected timer (0 = fade off)
    int overlay_row = 0;
    int bars_scroll = 0;            // overlay 10 (spectrum bars): the first row shown
    std::string update_latest, update_current, update_url;   // overlay 8: the new version, the one in use, the release page
    bool sort_az = false;         // SHIFT+t: the main STATIONS pane (and its search results) sorted by name A-Z instead of list order
    bool shuffle = true;          // station-surf mode shown in the small box next to SEARCH: true = "S" (shuffle), false = "L" (list); toggled with M
    std::string notice;           // one status line in the SEARCH box's hint spot ("recording saved ..."); main clears it after a few seconds
    bool cheat_open = false;      // the full-screen CHEATSHEET ('?'); drawn instead of the radio screen / menu
    mutable int cheat_scroll = 0; // in display lines; clamped while rendering (the renderer is const)
    // SHIFT+s in the cheat sheet: search the commands (src/help_search.h), as in the music player.
    bool cheat_searching = false;
    std::string cheat_query;
    size_t cheat_qcaret = 0, cheat_qanchor = 0;
    mutable int cheat_sel = 0, cheat_first = 0;
    mutable std::vector<int> cheat_result_rows;   // the table row of each result, best first (rebuilt by every frame)
    mutable int cheat_jump_row = -1;              // set by ENTER: the next frame scrolls the table to this row
    // The help wizard (SHIFT+w), the same one the music player has: the text, the layout, the page / part / contents
    // state and the key handling come from src/wizard.h, the colours from the PLAYER's config.txt (see
    // player_wizard_palette()), so it looks identical in both modes. Drawn instead of the radio screen, like the cheat
    // sheet -- so every menu and overlay underneath stays exactly as it was and is there again after the wizard closes.
    bool wizard_open = false;
    // the phone remote (src/remote_server.h): 0 its main screen, 1 its STATIONS list; the list's page and status line
    int phone_view = 0;
    int phone_page = 0;
    std::string phone_note;
    mutable wizard::State wizard; // clamped while rendering (the renderer is const)
    double wizard_angle = 0.0;    // the first page's disk, turned at 0.10 by the main loop
    std::vector<int> visible;     // station indices that pass the search filter, in list order
    int cursor = 0;               // position within `visible`
    // Main search box: "p:" typed into the empty box (or "/p:") searches the STATION LISTS instead of the stations; the
    // STATIONS pane then shows the matching lists (`lvisible`, indices into `lists`). ENTER on one shows its stations in
    // the pane (`active_list`) until ESC; "s:" switches the box back to stations.
    bool search_lists = false;
    std::vector<int> lvisible;
    int lcursor = 0;
    std::string active_list;      // name of the station list the STATIONS pane shows ("" = all stations)
    std::string search;
    EditState search_edit;        // caret + marked range inside `search` (only drawn while search_focus)
    bool search_focus = false;
    int cols = kUiCols;           // real terminal size (the frame adapts to it, see above)
    int rows = kUiRows;
    // Marquee state for the hovered station row (same idea as the player's list): which row the scroll
    // follows and when it started, so moving the cursor restarts the scroll from the beginning.
    mutable int marquee_row = -1;
    mutable double marquee_since = 0.0;
    double t_sec = 0.0;           // animation clock (lamp blink)
    SignWave wave;
    double dt = 0.033;            // seconds since the previous frame (visualizer motion)
};

// The scope window's settings, colours and title for the radio (src/scope_window.h).
void fill_scope_window_config(const RadioSettings& cfg, const RadioStatus& st, ScopeWinConfig& c);
// The spectrogram window's settings, colours and title for the radio.
void fill_spectro_window_config(const RadioSettings& cfg, const RadioStatus& st, SpectroWinConfig& c);

std::vector<std::string> render_radio_frame(const UiModel& m, const RadioStatus& st,
                                            RadioEngine& engine, const RadioSettings& cfg);

// The remote app's phone screen (src/remote_server.h): the ON AIR sign, the station, working dummy buttons and boxes with
// the radio's main commands, in the radio's colours. `zones`: where a tap sends which command.
std::vector<std::string> build_phone_screen(const UiModel& m, const RadioStatus& st, const RadioSettings& cfg, int W, int H,
                                            std::vector<muisc::remote::net::Zone>& zones);

// The MENUS index overlay (key "<", UiModel::overlay == 7): every menu and overlay by category with its key.
// `id` is the key action of the radio's REFERENCE table (kKeyActions), so a rebinding is followed.
struct MenusEntry { const char* cat; const char* title; const char* id; };
extern const MenusEntry kMenusEntries[];
extern const int kMenusCount;
// The entry to select after UP / DOWN / LEFT / RIGHT (dir 0 / 1 / 2 / 3) from entry `sel`.
int menus_move(int sel, int dir);

// Name of the active preset ("Default" when none is loaded).
std::string active_preset_name(const UiModel& m);

// Station index -> first preset slot holding it, or -1.
int preset_slot_of(const std::vector<int>& presets, int station_index);

} // namespace muisc::radio
