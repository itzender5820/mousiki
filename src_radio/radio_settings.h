// Radio settings (see radio_settings.cpp): the RadioSettings struct and the functions that read and
// write it.
#pragma once
#include "bars_presets.h"
#include "themes.h"
// Radio mode -- its OWN settings, separate from the music player's `Settings` / config.txt.
//
// The radio reads and writes `radio_config.txt` (next to stations.txt and presets.txt in ~/.config/mousiki/, or
// ./radio_config.txt) and uses nothing from src/settings.*. Colours are plain decimal 256-colour palette indices
// as strings ("1".."255"); "0" or an empty string means "no colour" (the terminal's own). Every number
// is a palette colour in every field, like the player.
//
// Every colour field is named after WHAT it colours (not after the player element it was once borrowed from):
//   on_air_*      the ON AIR sign's diagonal gradient (and the needle / lamp / LIVE text, which use its first colour)
//   viz_*         the FFT spectrum under the station info
//   osci_*        the oscilloscope (X-Y scope, top right)
//   freq_line / freq_mhz   the line + ticks of the frequency band / its numbers and the small dots
//   preset_*      the PRESETS pane (unassigned slots, the key letters)
//   volume_*      the "#" and "-" of the volume bar
//   list_*        the STATIONS list rows (also used for the station names in the PRESETS pane)
//   header        secondary text: captions ("off air"), the ON AIR sign while nothing is live, empty-pane messages, status lines
//   legend        key command legends: the "[TAB] Switch pane | ..." hint lines, hint tails, error lines, cheatsheet titles
#include "scope_metrics.h"
#include "viz_prefs.h"
#include "volume_bar.h"
#include "spectrogram.h"
#include <filesystem>
#include "equalizer.h"
#include <map>
#include <vector>
#include <string>

namespace muisc {
// (src/settings.cpp) the player's oscilloscope / spectrogram settings in config.txt, for Share Profiles
bool read_player_viz(VizPrefs& out);
bool write_player_viz(const VizPrefs& v);
// (settings.cpp) the Remote App lines of config.txt, and the upgrade backup of the config files
#ifndef MOUSIKI_REMOTE_APP_CONFIG
#define MOUSIKI_REMOTE_APP_CONFIG
struct RemoteAppConfig { bool enabled = false; int port = 4747; std::string pin; bool cli = false; std::string hosts; };
#endif
RemoteAppConfig read_remote_app();
bool write_remote_app(RemoteAppConfig& rc);
std::string config_stamp_line();
bool backup_config_if_older(const std::filesystem::path& p);
}

namespace muisc::radio {

struct RadioSettings : VizPrefs {
    // --- colours ----------------------------------------------------------------------------------
    std::string border_top = "141";
    std::string border_bottom = "212";          // the frame fades from border_top (top) to border_bottom (bottom)
    bool border_smooth = true;                  // BorderGradient=smooth|rough: smooth fades the frame line by line; rough = only the bottom border line takes border_bottom

    std::string on_air_upper_left = "141";      // ON AIR sign: gradient from the upper left ...
    std::string on_air_bottom_right = "212";     // ... to the bottom right (plus the travelling colour wave)

    std::string meta_key = "117";               // station info labels ("Station", "Title" ...)
    std::string meta_val = "231";               // station info values (also the station name next to the needle)

    std::string viz_left = "141";               // FFT spectrum, left edge
    std::string viz_center;                     // optional middle stop (config file only); empty = plain left -> right
    std::string viz_right = "212";               // FFT spectrum, right edge

    std::string osci_left = "141";              // oscilloscope, left edge
    std::string osci_right = "212";              // oscilloscope, right edge

    std::string freq_line = "141";              // the frequency line and its ticks
    std::string freq_mhz = "246";                 // the numbers (88 92 96 ...) and the small dots for stations

    std::string preset_inactive_fg = "246";       // PRESETS pane: key slots that have no station ("5 -")
    std::string preset_inactive_bg;
    std::string preset_key_fg = "253";          // PRESETS pane: the short-cut key letters (1 2 ... e r t d f g)

    std::string volume_current = "212";         // the "#" of the volume bar
    std::string volume_possible = "60";        // the "-" of the volume bar

    // Search bar input (the "/s:" box and the other "Search:" rows). Empty = follow LIST INACTIVE FG/BG,
    // which is what these bars always looked like; "0" = explicit terminal default.
    std::string search_fg;                      // typed query: FG / BG, behind the query text only
    std::string search_bg;
    std::string search_inactive_fg;             // the rest of the bar (prefix, empty field)
    std::string search_inactive_bg;

    std::string list_fg = "253";                // STATIONS rows (and the station names in PRESETS)
    std::string list_bg;
    std::string list_playing_fg = "84";        // the tuned station
    std::string list_playing_bg;
    std::string list_cursor_fg;                 // the hovered row
    std::string list_cursor_bg = "60";

    std::string tab_current = "212";             // settings screen: the current tab name "[GENERAL]" and the "< ↔ >" hint
    std::string tab_other = "246";                // settings screen: the other tab names

    std::string focus = "246";                    // FOCUS: the pane focus indicator "\u25c0" after a pane's title (default = header)
    std::string header = "246";                   // secondary text (see the top of this file)
    std::string legend = "141";                  // key command legends (see the top of this file)
    std::string legend_cmd = "213";              // the descriptions behind the keys of those legends ("Close" in "[ESC] Close")

    // --- looks / tuning (config file only for now; the settings screen has the colours only) ----------
    // --- the GENERAL tab ---------------------------------------------------------------------------
    bool on_air_ascii = true;                   // the ON AIR sign (with its colour wave)
    bool pulse_wave = true;                     // the faint circle burst behind the sign
    bool dummy_buttons = true;                  // the <<< MUTE >>> boxes next to the frequency band
    bool volume_gradient = false;               // "gradient" right of the Volume indicator: the filled bar takes the oscilloscope colours (osci_left -> osci_right)
    int volume_indicator = 4;                   // what the VOLUME BAR is drawn with: 0 "#", 1 "⠶", 2 "⣿", 3 "⡾⣽", 4 "disk" (volume_bar.h; the default: the animation)
    int scope_mode = 0;                         // right of the station info: 0 = oscilloscope, 1 = sphere, 2 = off, 3 = spectrogram; default: the oscilloscope
    bool element_visualizer = true;             // the FFT spectrum under the station info
    bool stereo = true;                         // false = fold left + right to mono
    // Which mode Mousiki starts in ("player" / "radio"): the same setting as the music player's, kept in ITS config.txt
    // (StartMode=); the radio only reads and writes that one line (player_start_mode() / set_player_start_mode()).
    std::string start_mode = "player";   // "player" | "radio" | "audiobook"
    // Mode switch (SHIFT and + / SHIFT and -): which modes are on and in which order they follow each other. The same values as
    // the player's (UseModePlayer / UseModeRadio / UseModeAudiobook / ModeOrder in ITS config.txt); one mode is always on.
    bool use_mode[3] = {true, true, true};      // player, radio, audio book
    int mode_order[3] = {0, 1, 2};              // a permutation of 0..2
    // AudioBookPath= of the player's config.txt (shown in this program's PATHS tab so it is reachable from every mode).
    std::string audiobook_path;
    // Black Background: the terminal's background is black while Mousiki runs. One value with the player, in ITS config.txt
    // (BlackBackground=); read / written like the start mode.
    bool black_background = true;
    // Share Profiles (ANIMA/VIZ -> OSCI/SPECTRO SETTINGS; ShareProfiles= in the player's config.txt, one value for both): with it on, the oscilloscope and
    // spectrogram settings (the VizPrefs base) are the player's. `own_viz` keeps the radio's own values meanwhile (they stay in
    // radio_config.txt and come back when it is turned off). Colours / themes are never shared.
    bool share_profiles = false;
    VizPrefs own_viz;
    // The sound output of all modes (GENERAL -> AUDIO "Choose Audio Device"; AudioDevice= in the player's config.txt, "" = the
    // system default). Read when the radio starts; the CHOOSE AUDIO DEVICE overlay writes that line at once.
    std::string audio_device;
    // The terminal's font in Mousiki's window: GENERAL -> TEXT -> "Terminal default font" (TerminalDefaultFont= in the player's config.txt,
    // one value for all modes); terminal_font = TerminalFont= there (no row of its own).
    bool terminal_default_font = false;
    std::string terminal_font = "Cascadia Mono";
    // Remote App (GENERAL -> REMOTE / UPDATES; RemoteApp= / RemoteAppPort= / RemoteAppPin= in the player's config.txt, one value for both):
    // the web server of the phone remote (src/remote_server.h). Read when the radio starts, written by save_radio_settings().
    bool remote_app = false;
    int remote_port = 4747;
    std::string remote_pin;
    std::string remote_hosts;      // config.txt RemoteAppHosts=
    bool remote_cli = false;      // config.txt RemoteAppCli=true: /api/cli
    bool use_osci = true;                       // GENERAL "Use oscilloscope": false = never in the scope block / "." cycle
    bool use_spectro = true;                    // GENERAL "Use spectrogram": the same for the spectrogram
    // The help wizard (SHIFT+w): false means it has never been shown here, so
    // the first start of the radio after an install opens it by itself. Set
    // and written the first time it is closed, exactly like the player's
    // WizardSeen -- the two modes count separately, so each greets once.
    bool wizard_seen = false;
    bool normalize = true;                      // loudness normalisation (SHIFT+v overlay, `v` toggles)
    double normalize_target_lufs = -16.0;       // -40 .. 0
    double normalize_max_boost_db = 9.0;        // 0 .. 24
    // Equaliser (SHIFT+e overlay): the player's 10-band EQ with its presets; own values, saved in radio_config.txt.
    bool eq_enabled = false;
    EqGains eq_gains = {};
    std::vector<EqCustomPreset> eq_custom_presets;
    double gradient_wave_speed = 0.25;          // ON AIR sign colour wave, cycles per second
    double pulse_wave_speed = 0.125;            // circle burst behind the sign, loops per second (independent of the colour wave)
    bool playback_shuffle = true;               // station surf mode: true = shuffle (S), false = list (L); `m` toggles
    // --- the PATHS tab (empty = the default; see StoragePaths below) -----------------------------------
    std::string stations_path;                  // station lists: <path>/stations/stations.txt + <path>/station_lists/stationlists.txt
    std::string presets_path;                   // <path>/presets/presets.txt
    std::string download_path;                  // recordings (y) + YouTube downloads go to <path>/radio_downloads
    bool download_same_as_player = true;        // true: use the music player's download folder instead
    std::string history_path;                   // <path>/radio_history/history_radio.txt
    // --- sleep timer (SHIFT+z overlay) and the REFERENCE tab ------------------------------------------
    bool marquee = true;                        // GENERAL tab: a station name that is too long scrolls; false = cut off with "..."
    bool tune_noise = false;                    // GENERAL tab: static that fades in / out when another station is tuned
    // GENERAL tab: how much of the tuned station is kept for pause / rewind / recording from the past, as an index into
    // kTimeshiftMinutes (5 / 15 / 30 / 45 / 60 min). Kept on disk: 11.5 MB per minute.
    int timeshift_idx = 2;
    bool menu_classic = false;                  // the "<" menus overlay shows the classic category boxes instead of the tree (SHIFT+t there)
    bool sleep_fade = true;                     // the sleep timer fades the volume out before it stops the stream
    std::map<std::string, std::string> keys;    // rebound keys by action id (kKeyActions); absent = the default
    int visualizer_fluidity = 10;               // 1-10, higher = smoother / slower rise
    int visualizer_degradation_speed = 8;       // 1 (slow fade) - 10 (near-instant)
    int visualizer_viscosity = 3;               // 0 (bars independent) - 10 (heavy neighbour blending)
    int bars_style = 1;                         // 0 classic, 1 Hi-Fi (hifi_bars.h)
    int hifi_resolution = 0;                    // Hi-Fi: 0 fine, 1 rough
    int hifi_channels = 0;                      // Hi-Fi: 0 stereo, 1 mono
    int hifi_fluidity = 10;                     // the Hi-Fi style keeps its own three bar values (visualizer_* are the Classic ones)
    int hifi_degradation = 8;
    int hifi_viscosity = 3;
    int& bar_fluidity() { return bars_style == 1 ? hifi_fluidity : visualizer_fluidity; }
    int& bar_degradation() { return bars_style == 1 ? hifi_degradation : visualizer_degradation_speed; }
    int& bar_viscosity() { return bars_style == 1 ? hifi_viscosity : visualizer_viscosity; }
    int bar_fluidity() const { return bars_style == 1 ? hifi_fluidity : visualizer_fluidity; }
    int bar_degradation() const { return bars_style == 1 ? hifi_degradation : visualizer_degradation_speed; }
    int bar_viscosity() const { return bars_style == 1 ? hifi_viscosity : visualizer_viscosity; }
    int hifi_adapt = 1;                         // Hi-Fi: the height adapts to the song
    int hifi_split = 1;                         // Hi-Fi: an empty dot column between the channels
    int hifi_scale = 2;                         // Hi-Fi height: 0 full, 1 medium, 2 low
    int hifi_fscale = 2;                        // Hi-Fi: the frequency scale (0 linear, 1 logarithmic, 2 mel, 3 bark, 4 erb)
    int hifi_gain = -2;                         // Hi-Fi: dB, -20 .. 20
    int hifi_range = 68;                        // Hi-Fi: dB from the bottom to the top, 40 .. 100
    int hifi_color = 0;                         // Hi-Fi: 0 gradient, 1 corresponding (the spectrogram's colours by height)
    // (the oscilloscope / spectrogram settings -- osci_set, osci_style, osci_mode, gfx_protocol, spectro ... -- are the VizPrefs base)

    std::string box_upper_left = "╭";      // ╭
    std::string box_upper_right = "╮";     // ╮
    std::string box_lower_left = "╰";      // ╰
    std::string box_lower_right = "╯";     // ╯
    std::string box_vertical = "│";        // │
    std::string box_horizontal = "─";      // ─
    std::string list_separator = "|";           // between the columns of the lists
};

// ---- the COLORS tab of the settings screen -------------------------------------------------------------
// One row of the tab: a group name, one or two colour cells (member pointers into RadioSettings). A row with
// no second cell (label2 == nullptr) is foreground-only.
// ---- colour themes (see src/themes.h) -------------------------------------------------------------------
void theme_apply(RadioSettings& s, const themes::Theme& t);
themes::Theme theme_capture(const RadioSettings& s, themes::Theme base);
bool theme_matches(const RadioSettings& s, const themes::Theme& t);
int theme_index(const RadioSettings& s, const std::vector<themes::Theme>& all);   // the theme `s` matches exactly, or -1

struct ColorRowSpec {
    const char* group;                          // "" = continuation of the group above
    const char* label1; std::string RadioSettings::* field1;
    const char* label2; std::string RadioSettings::* field2;
};
extern const ColorRowSpec kColorRows[];
extern const int kColorRowCount;
// Rows before this index are drawn above the divider line of the tab, the rest below it.
constexpr int kColorRowsAboveDivider = 7;

// The COLORS tab, top to bottom: row 0 = THEME | CREATE THEME, row 1 = the BORDER_COLOR row (colour row 0), row 2 =
// SMOOTHNESS (rough / smooth), rows 3.. = the other colour rows. color_field() and the theme creator count colour rows alone.
inline int color_row_of_tab(int t) { return t == 1 ? 0 : (t >= 3 ? t - 2 : -1); }
inline int tab_row_of_color(int c) { return c == 0 ? 1 : c + 2; }
inline int color_tab_row_count() { return kColorRowCount + 2; }   // + THEME + SMOOTHNESS

// Cell (row, col 0/1) of the COLORS tab, or nullptr if that row has no such cell.
std::string* color_field(RadioSettings& s, int row, int col);
const std::string* color_field(const RadioSettings& s, int row, int col);

// ---- the GENERAL and ANIMA/VIZ tabs of the settings screen ----------------------------------------------------------
// Both tabs are lists of category headers and rows (like the music player's): tab 1 GENERAL = MODES (Playback, Start Mode)
// | MODES ON/OFF AND ORDER | TERMINAL | REMOTE / UPDATES | AUDIO (the switches); tab 2 ANIMA/VIZ = ON AIR SIGN |
// SPECTRUM BARS | OSCI/SPECTRO SETTINGS (Share Profile, Osci/sphere, Osci style). The cursor
// (SettingsModel::row) counts the rows only; rs_row_line() gives the line it is drawn on.
struct SettingsLine { bool header; int id; const char* text; };       // a category header, or a row (id + label)
const std::vector<SettingsLine>& rs_layout(int tab);                   // tab 1 or 2
void rs_set_hifi_rows(bool on);                                       // the Hi-Fi rows of SPECTRUM BARS are listed only while the style is Hi-Fi
int rs_row_count(int tab);                                              // selectable rows of the tab
int rs_row_line(int tab, int row);                                      // line (headers counted) of a row
const char* rs_row_label(int tab, int row);
std::string rs_row_value(const RadioSettings& s, int tab, int row);    // "true" / "false", "osci" / "sphere" / "off" / "spectro", ...
void rs_row_change(RadioSettings& s, int tab, int row, int dir);        // left (-1) / right (+1) / ENTER (0 = next)
extern const int kOnOffRowCount;                                        // rows of tab 1
extern const int kAnimRowCount;                                         // rows of tab 2
inline constexpr int kTimeshiftMinutes[5] = {5, 15, 30, 45, 60};
inline int timeshift_minutes(const RadioSettings& s) { return kTimeshiftMinutes[s.timeshift_idx < 0 ? 0 : s.timeshift_idx > 4 ? 4 : s.timeshift_idx]; }
std::string scope_mode_name(const RadioSettings& s);                    // "osci" | "sphere" | "off" | "spectro"
// Start Mode lives in the music player's config.txt (StartMode=player|radio): read it / write just that line.
std::string player_start_mode();
bool set_player_start_mode(const std::string& mode);
// The mode switch and the audio book path live there as well: read them into / write them from a RadioSettings.
void load_player_mode_settings(RadioSettings& s);
void save_player_mode_settings(const RadioSettings& s);
// Black Background lives there too (BlackBackground=true|false, true when missing).
bool player_black_background();
bool player_reduce_motion();
bool set_player_black_background(bool on);
// Share Profiles lives there too (ShareProfiles=true|false, false when missing).
bool player_share_profiles();
bool set_player_share_profiles(bool on);
// Turns Share Profiles on (the radio takes over the player's values, its own are kept) or off (its own come back).
void radio_share_profiles(RadioSettings& s, bool on);

// ---- the REFERENCE tab: the keys that can be changed ------------------------------------------------------
// A key is written like the music player writes it: one character (case matters: S and s are different keys), or
// SPACE / TAB / BACKSPACE. Enter, Esc and the arrows keep their jobs and cannot be bound. The preset slot keys
// (1234567890ertdfg) and j / k are fixed; they only take part in the conflict check.
struct KeyAction { const char* header; const char* id; const char* label; const char* def; };
extern const KeyAction kKeyActions[];
extern const int kKeyActionCount;
std::string key_binding(const RadioSettings& s, int action);                 // current key string (the default when unset)
int key_code(const std::string& key);                                        // key string -> code, 0 = not a valid key
// "" when `key` is free, else what it is already used for ("NEXT CHANNEL", "PRESET SLOT KEY" ...). `except` = the action being edited.
std::string key_conflict(const RadioSettings& s, const std::string& key, int except);
// Translates a typed key into the key the main screen's switch handles (the default key of the action the typed key is
// bound to); 0 for a default key that was rebound away. Keys that are not bound to anything pass through unchanged.
int key_translate(const RadioSettings& s, int typed);
// The code of the key an action is bound to right now.
int key_of(const RadioSettings& s, const char* id);
// the ABOUT tab text is about_app_text() (about_text.h), shared with the music player

// ---- the PATHS tab -----------------------------------------------------------------------------------------
// Rows: 0 STATION LISTS PATH | 1 PRESETS PATH | 2 DOWNLOAD PATH | 3 "Same folder as music player" (bool) | 4 HISTORY PATH.
extern const int kPathRowCount;
extern const char* const kPathRows[];
constexpr bool path_row_is_bool(int row) { return row == 3; }
std::string* path_row_text(RadioSettings& s, int row);              // nullptr for the bool row
const std::string* path_row_text(const RadioSettings& s, int row);

// Where everything ends up for the current settings.
struct StoragePaths {
    std::filesystem::path stations_file, lists_file, presets_file;   // the three data files
    std::filesystem::path history_dir;                               // holds history_radio.txt + archive_radio.txt
    std::filesystem::path download_dir;                              // recordings + downloads
    bool player_download = false;                                    // download_dir is the music player's folder
    std::string download_note;                                       // "" or why it is what it is
};
StoragePaths resolve_storage_paths(const RadioSettings& s);
// The music player's DownloadFolder (read from its config.txt, `DownloadFolder=`); "" = the player's built-in default.
std::string player_download_folder();
std::filesystem::path radio_default_dir();                           // ~/.config/mousiki

// The help wizard (SHIFT+w) is one feature, so it looks the same in both
// modes: the radio draws it in the MUSIC PLAYER's colours rather than its
// own. These are read straight out of the player's config.txt, the same
// read-only peek player_download_folder() does -- the radio has no other
// connection to the player's settings. Every field keeps the player's own
// default when config.txt is missing or does not set that row, so the wizard
// is always drawn in sensible colours even on a first start.
struct WizardPalette {
    std::string border      = "8";    // ColorBorderTop
    std::string border_bot  = "8";    // ColorBorderBottom
    std::string header      = "10";   // ColorHeader   -- headings
    std::string legend      = "8";    // ColorLegend   -- key hints
    std::string list;                 // ColorListInactiveFg -- body text ("" = the terminal's own)
    std::string cursor_fg;            // ColorListCursorFg   -- the contents cursor
    std::string cursor_bg   = "238";  // ColorListCursorBg
    std::string accent;               // ColorLyricsActiveWordFg, falling back to the cursor / header colour
    std::string disk_top    = "36";   // ColorDiskTop    -- the welcome page's disk
    std::string disk_bottom = "34";   // ColorDiskBottom
};
WizardPalette player_wizard_palette();

// ---- the SHIFT+o (oscilloscope) and SHIFT+v (loudness) overlays: same knobs and ranges as the music player's ----
struct Knob { const char* label; double lo, hi, step; };
constexpr Knob kOsciKnobs[3] = {{"Decay", 0.00, 0.99, 0.01}, {"Dot threshold", 0.01, 1.00, 0.01}, {"Tail", 0.00, 1.00, 0.02}};
// (the overlay rows, osci_visible_rows(), osci_reset() ... are the player's: viz_prefs.h; the radio adds only its "Display" row)
constexpr Knob kNormKnobs[3] = {{"Normalize", 0.0, 1.0, 1.0}, {"Target level", -40.0, 0.0, 1.0}, {"Max boost", 0.0, 24.0, 1.0}};
std::string osci_row_value(const RadioSettings& s, int row);   // adds kOrDisplay (osci / sphere / spectro)
void osci_adjust(RadioSettings& s, int row, int dir);     // dir -1 / +1
// "Use oscilloscope / spectrogram" off: what is switched off never stays in the scope block.
void radio_enforce_visuals(RadioSettings& s);
void norm_adjust(RadioSettings& s, int row, int dir);     // row 0 = on/off (left off, right on)
void norm_reset(RadioSettings& s);                        // target + boost back to the defaults; on/off is left alone

// ---- config file -------------------------------------------------------------------------------------
std::filesystem::path radio_config_path();               // where radio_config.txt is read from / written to
RadioSettings load_radio_settings(std::string* source_out = nullptr);   // defaults when there is no file
bool save_radio_settings(const RadioSettings& s, std::string* err = nullptr);
std::string radio_settings_text(const RadioSettings& s);   // what save_radio_settings() would write (compare two to see a change)

// ---- colour helpers (the radio's own; same value rules as above) -------------------------------------------
bool color_rgb(const std::string& color, int& r, int& g, int& b);   // 0-255 colour number -> RGB (false: none)
std::string ansi_fg(const std::string& color);           // "\x1b[38;5;Nm" ... or "" for none
std::string ansi_bg(const std::string& color);
std::string sgr_params(const std::string& color);        // "38;5;N" without the escape, or ""
std::string gradient_fg(const std::string& start, const std::string& end, float t);                       // truecolor between two palette colours
std::string gradient3_fg(const std::string& left, const std::string& center, const std::string& right, float t);   // 3 stops; no centre: 2 stops
std::string gradient_bar(const std::string& start, const std::string& end, int width);                    // a strip of blocks (settings preview)
// True for "", "0" or a number 0..255 (what a colour cell accepts).
bool valid_color_value(const std::string& v);

} // namespace muisc::radio
