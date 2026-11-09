// Radio mode screen: the helpers the radio_ui*.cpp files share (the one big file was split so that it compiles in parallel).
#pragma once

#include "radio_ui.h"
#include "overlay_common.h"
#include "about_text.h"
#include "border_gradient.h"
#include "scope_big.h"
#include "scope_window.h"
#include "keyboard_layout.h"
#include "mode_switch.h"
#include "help_search.h"
#include "text_edit.h"
#include "translit.h"   // shown_text(): Latin letters on / off (the same switch as in the music player)
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <map>
#include <unordered_set>
#include "history.h"
#include "settings.h"
#include "radio_sign.h"
#include "sphere_visualizer.h"
#include "hifi_bars.h"
#include "bars_rows.h"
#include "terminal_ui.h"
#include "disk_art.h"      // the help wizard's welcome page
#include "wizard.h"        // its text, shared with the music player

namespace muisc::radio::detail {


constexpr const char* kReset = "\x1b[0m";

struct Seg { std::string ansi; std::string text; };

// Look of the screen, resolved once per frame from the radio's own RadioSettings.
// Role names say what they colour:  header = captions / empty-pane messages / the sign while off air,
// legend = key command hints, error lines and cheatsheet titles (see radio_settings.h).
struct Style {
    const RadioSettings& c;
    std::string border, border_bottom, legend, legend_cmd, header, focus, key, val, list, list_playing, list_cursor,
                preset_inactive, preset_key, vol_current, vol_possible, freq_line, freq_mhz, search, search_idle;
    explicit Style(const RadioSettings& cfg) : c(cfg) {
        border = ansi_fg(c.border_top);
        border_bottom = ansi_fg(c.border_bottom);
        legend = ansi_fg(c.legend);
        legend_cmd = ansi_fg(c.legend_cmd);
        header = ansi_fg(c.header);
        focus = ansi_fg(c.focus);
        key = c.meta_key.empty() ? ansi_fg(c.list_fg) : ansi_fg(c.meta_key);
        val = c.meta_val.empty() ? ansi_fg(c.list_fg) : ansi_fg(c.meta_val);
        list = ansi_fg(c.list_fg) + ansi_bg(c.list_bg);
        // Search bar input: its own TEXT / BG cells, an empty one follows LIST INACTIVE.
        search = ansi_fg(c.search_fg.empty() ? c.list_fg : c.search_fg) + ansi_bg(c.search_bg.empty() ? c.list_bg : c.search_bg);
        search_idle = ansi_fg(c.search_inactive_fg.empty() ? c.list_fg : c.search_inactive_fg)
                    + ansi_bg(c.search_inactive_bg);   // no BG inherited: only the typed query is highlighted
        list_playing = ansi_fg(c.list_playing_fg) + ansi_bg(c.list_playing_bg);
        list_cursor = ansi_fg(c.list_cursor_fg) + ansi_bg(c.list_cursor_bg);
        if (list_cursor.empty()) list_cursor = "\x1b[7m"; // never let the cursor row be invisible
        preset_inactive = ansi_fg(c.preset_inactive_fg) + ansi_bg(c.preset_inactive_bg);
        preset_key = ansi_fg(c.preset_key_fg);
        vol_current = ansi_fg(c.volume_current);
        vol_possible = ansi_fg(c.volume_possible);
        freq_line = ansi_fg(c.freq_line);
        freq_mhz = ansi_fg(c.freq_mhz);
    }
    // ON AIR sign: upper left -> bottom right (t = 0 .. 1).
    std::string onair_at(float t) const { return gradient_fg(c.on_air_upper_left, c.on_air_bottom_right, t); }
    // FFT spectrum under the station info (3-stop if a centre colour is set).
    std::string viz_at(float t) const { return gradient3_fg(c.viz_left, c.viz_center, c.viz_right, t); }
    // Oscilloscope.
    std::string osci_at(float t) const { return gradient_fg(c.osci_left, c.osci_right, t); }
};

// Cell canvas for the tuner band: per-cell text + colour, flattened into runs.
struct Canvas {
    std::vector<std::string> ch;
    std::vector<std::string> col;
    explicit Canvas(int w) : ch(static_cast<size_t>(w), " "), col(static_cast<size_t>(w)) {}
    int width() const { return static_cast<int>(ch.size()); }
    void put(int x, const std::string& g, const std::string& ansi) {
        if (x < 0 || x >= width()) return;
        ch[static_cast<size_t>(x)] = g; col[static_cast<size_t>(x)] = ansi;
    }
    void text(int x, const std::string& t, const std::string& ansi) {
        // ASCII only (labels, numbers, station names go through utf8_take first)
        for (size_t i = 0; i < t.size(); ++i) put(x + static_cast<int>(i), std::string(1, t[i]), ansi);
    }
    std::string flatten() const {
        std::string o, cur;
        for (size_t i = 0; i < ch.size(); ++i) {
            if (col[i] != cur) { if (!cur.empty()) o += kReset; cur = col[i]; o += cur; }
            o += ch[i];
        }
        if (!cur.empty()) o += kReset;
        return o;
    }
};

// ===========================================================================================
// INFO PANEL (rows: 15 = border + 13 + border). Columns inside the frame:
//    40 sign | 5 separator | 40 station info | 33 XY scope   = 118
// ===========================================================================================
constexpr int kPanelH = 14;
constexpr int kLeftW = kSignW + 1;  // 1 column margin + the sign
constexpr int kSepW = 5;
constexpr int kMetaW = 42;
// (the oscilloscope takes whatever width is left; see render_radio_frame)

// ---------------------------------------------------------------------------------------------
// Circle-burst animation behind the ON AIR sign (from "Circle bust animation.json", 29 frames).
// Four rings centred on the panel: one hairline ring shrinks (frames 0-17), two hairline rings and
// one thick ring grow (frames 5-20, 14-29, 12-27). Drawn with braille dots in the cells the sign
// leaves empty, in the sign's own gradient colours but very dim, so the sign stays the star.
// ---------------------------------------------------------------------------------------------
constexpr float kCircleDim = 0.17f;        // brightness of the rings relative to the gradient (faint)
constexpr float kCircleFrames = 29.0f;     // length of the source animation

struct CircleRing {
    float t0, t1;        // keyframe frames
    float s0, s1;        // layer scale in percent at t0 / t1
    float visible_from;  // layer in-point
    float stroke_px;     // stroke width in comp pixels at 100 % scale
    bool  linear;        // Background4 uses a linear ramp, the others the eased one
};

// Comp 3840x2160, ring path diameter 217.094 * 7.8 = 1693.3 px (radius 846.7 px) at 100 %.
constexpr CircleRing kCircleRings[4] = {
    { 0.0f, 17.0f, 270.0f,   0.0f,  0.0f,  7.8f, true  },   // Background4: shrinks
    { 5.0f, 20.0f,   0.0f, 270.0f,  5.0f,  7.8f, false },   // Background3: grows
    {14.0f, 29.0f,   0.0f, 270.0f, 14.0f,  7.8f, false },   // Background6: grows
    {12.0f, 27.0f,   0.0f, 310.0f, 12.0f, 218.4f, false },  // Background5: grows, thick stroke
};
inline const void* scroll_id(int n) { return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(n)); }

// ===========================================================================================
// TUNER BAND (5 rows, 84 wide) + buttons / volume (gap 1 + 35)
// ===========================================================================================
constexpr int kSideW = 33;   // right of the band: buttons 3 x 11 = 33, volume line 33
constexpr int kBandGap = 1;  // blank column between the band's right border and the buttons / volume bar
constexpr int kBtnW = 11;                            // three buttons = 33 columns
constexpr int kVolBar = 15;                          // volume bar cells
constexpr double kBandLo = 87.5, kBandHi = 108.0;

// The small name box (same construction as the music player's Oscilloscope overlay: titled box, plain
// rows, key legend in the bottom edge).
constexpr int kNameOverlayW = 44;

// ===========================================================================================
// RADIO BROWSER menu (Shift+S): six input panes (3 rows x 2), RESULTS | STATION INFO underneath.
// ===========================================================================================
const char* const kBrowseTitles[kBrowseFields] = {
    "NAME", "TAGS (comma separated, all must match)", "COUNTRY (name or code, e.g. DE)",
    "STATE / REGION", "LANGUAGE (e.g. german)", "BITRATE (kbps: e.g. 128 or range via 64-192)"};

struct CheatRow { const char* header; const char* key; const char* desc; };
const CheatRow kCheatRows[] = {
    // --- System ---
    {"SYSTEM (MAIN UI)", "@SWITCHKEY", "Switch to the next mode (music player, radio, audio books: the order and the modes that are on are set in Settings -> GENERAL -> MODES; the radio is closed completely first)"},
    {nullptr, "@SWITCHBACK", "Switch to the previous mode"},
    {nullptr, "?", "This cheatsheet (toggle)"},
    {nullptr, "<", "The MENUS index: all menus and overlays as an organigram (Main, categories, menus) with their keys (LEFT/RIGHT move between boxes, DOWN goes in, UP goes back, ENTER opens, SHIFT+T switches to the classic view and back, ESC closes). SHIFT+, on an English keyboard"},
    {nullptr, "q / CTRL+C", "Quit (CTRL+C copies instead while you type in a text field)"},
    {nullptr, "ESC", "Close cheatsheet / overlay / menu; clear and leave the search box"},
    {nullptr, "ENTER", "Confirm / select (tunes the hovered station; on a station list or preset it opens it and tunes in)"},
    {nullptr, "ARROW KEYS", "Navigate (context-dependent); in a text field LEFT / RIGHT move the caret"},
    // --- Playback ---
    {"PLAYBACK (MAIN UI)", "ENTER", "Tune the hovered station"},
    {nullptr, "n", "Next channel in the list (always, whatever the mode)"},
    {nullptr, "#", "Shuffle: tune a random next channel (always, whatever the mode)"},
    {nullptr, "b", "Previous channel, follows the mode: S = the one played before, L = the one before it in the list"},
    {nullptr, "m", "Switch the mode: S (shuffle) <-> L (list); shown in the box next to SEARCH (only affects B)"},
    {nullptr, "p", "Mute / unmute (box shows MUTE / TUNE); with no channel loaded it tunes the hovered one instead"},
    {nullptr, "x", "Stop the stream"},
    {nullptr, "SHIFT+r", "Reconnect the tuned station"},
    {nullptr, "+ / -", "Volume up / down (5 % steps)"},
    {"SOUND (MAIN UI)", "v", "Toggle loudness normalisation"},
    {nullptr, "SHIFT+v", "Loudness normalisation overlay (target level, max boost)"},
    {nullptr, "SHIFT+e", "Equalizer overlay (the player's 10 bands + presets; the preset key e is lowercase and unaffected)"},
    {nullptr, "</>/UP/DOWN", "Equalizer: select band (LEFT / RIGHT) / gain +1 / -1 dB"},
    {nullptr, ",/./TAB/0/R", "Equalizer: previous / next preset / zero the band / reset to Flat (SPACE on/off)"},
    {nullptr, "S/DEL/X", "Equalizer: save the curve as a custom preset (ENTER saves, ESC cancels) / delete the selected one (press twice)"},
    {"VISUALIZATIONS (MAIN UI)", ".", "Cycle the scope block: oscilloscope / sphere / spectrogram, like . in the player (rebindable; what is off on the GENERAL tab is skipped)"},
    {nullptr, "SHIFT+i", "Spectrogram options overlay (view: spectrogram / waterfall, style, motion, scale, frequencies, gain, range, window, colors, channels, time span, the waterfall's graphs; R = Audacity's defaults)"},
    {nullptr, "SHIFT+b", "Spectrum bars overlay (under the upper pane, scrolls on a low terminal): style, Hi-Fi resolution / channels / gap / height / adapt / frequency scale / gain / range / colour, bar motion (toggle; R = defaults)"},
    {nullptr, "SHIFT+u", "Spectrogram full screen with frequency labels and time ruler (toggle; ESC closes)"},
    {nullptr, "SHIFT+0", "Oscilloscope full screen: YT mode left, XY mode right, values in the border (toggle; ESC closes; the SHIFT+o options open on top of it). The key is the character = (SHIFT+0 on a German keyboard)"},
    {nullptr, "SHIFT+a / v / c / p", "Oscilloscope full screen: axes / grid on or off   axis values on or off   grid colour (small window: type 0-255, empty = colour of inactive list elements)   grid metrics (small window: time unit, grid divisions, steady readouts in the picture names: RMS level, peak hold, L/R correlation, L/R balance)"},
    {nullptr, "SHIFT+o", "Oscilloscope overlay (osci music mode, style, frame rate up to 165 -- all per style --, image protocol, image resolution, afterglow, dot threshold, tail, Line/Vec. Interpol., Z-Axis, Z depth / source, trace length, rotation, mono phase portrait; R resets)"},
    {nullptr, ")", "Scope window: the oscilloscope in its own window, drawn by the graphics card, braille or image style as set in the SHIFT+o overlay (toggle; needs SDL2). In it: F / F11 / double-click fullscreen, T always on top, ESC closes"},
    {nullptr, "(", "Spectrogram window: the spectrogram in its own window, scrolling smoothly at the monitor's refresh rate (toggle; needs SDL2; same keys inside it)"},
    {"RECORD AND SLEEP TIMER (MAIN UI)", "SHIFT+z", "Sleep timer: 15 / 30 / 60 / 90 / 120 min, stops the stream (optionally with a fade-out of the volume)"},
    {nullptr, "y", "Record: opens the RECORD overlay (from now on = the moment y was pressed, the last 1 / 5 / 10 / 15 / 30 min, everything buffered); y again stops and saves the MP3"},
    {nullptr, "UP/DOWN/ENTER", "Record overlay: choose the start / start recording (y does the same; ESC cancels)"},
    {"OVERLAYS AND MENUS (MAIN UI)", "h", "Open the LISTENING HISTORY: HISTORY / TOP CHANNELS / HABITS"},
    {nullptr, "SHIFT+l", "Open the big STATIONS overlay (the STATIONS pane with the whole screen)"},
    {nullptr, "s", "Open the RADIO SETTINGS (colours, modes and switches, animation and visualizer, paths, keys, about; saved to radio_config.txt)"},
    {nullptr, "SHIFT+s", "Open the RADIO BROWSER menu: search stations worldwide (radio-browser.info)"},
    {nullptr, "SHIFT+k", "Open the STATION LISTS menu: build, save and load named station lists"},
    // --- Timeshift ---
    {"TIMESHIFT (MAIN UI, RADIO BUFFER 5-60 MIN)", "SPACE", "Pause / resume the station (the stream keeps buffering, you catch up later)"},
    {nullptr, "[ / ]", "Jump back / forward 30 seconds (shown for 4 s at the bottom of the scope block)"},
    {nullptr, "{", "Jump back 5 minutes"},
    {nullptr, "}", "Back to live"},
    // --- Navigation ---
    {"NAVIGATION (MAIN UI)", "UP / DOWN", "Move the cursor in the STATIONS list"},
    {nullptr, "SHIFT+t", "Toggle the STATIONS sort: list order <-> name A-Z (also for the search results)"},
    {nullptr, "\"", "Latin letters on / off: Japanese kana, Korean and Chinese station names and titles shown in Latin letters (the same switch as in the music player; SHIFT+' on a US keyboard)"},
    // --- Presets ---
    {"PRESETS (MAIN UI)", "1 2 3 4 5 6 7 8 9 0", "Tune slot 1-10 of the active preset"},
    {nullptr, "E R T D F G", "Tune slot 11-16 of the active preset (lowercase)"},
    {nullptr, "SHIFT+LEFT/RIGHT", "Previous / next preset (wraps; saved right away)"},
    {nullptr, "SHIFT+p", "Open the PRESETS menu"},
    // --- Search ---
    {"SEARCH (MAIN UI)", "/", "Search stations (matches name, genre and country)"},
    {nullptr, "TYPE", "Filter the list while you type; BACKSPACE deletes"},
    {nullptr, "UP / DOWN", "Search box: move through the results"},
    {nullptr, "ENTER", "Search box: tune the hovered result and leave the box"},
    {nullptr, "ESC", "Search box: clear the search and leave the box. Main screen: clear the search, then leave a list"},
    {nullptr, "P:", "Typed into the empty box (or /p:): search the STATION LISTS; ENTER shows the hovered list in STATIONS and tunes its first station"},
    {nullptr, "S:", "Typed into the empty box: back to searching the stations"},
    // --- Text fields ---
    {"TEXT FIELDS (SEARCH BOXES, STATION EDITOR, RADIO BROWSER PANES)", "LEFT / RIGHT", "Move the caret"},
    {nullptr, "SHIFT+LEFT/RIGHT", "Mark text (extend the selection); typing or pasting replaces it"},
    {nullptr, "HOME / END", "Caret to the start / end of the text"},
    {nullptr, "BACKSPACE / DEL", "Delete the marked text, else the character before / under the caret"},
    {nullptr, "CTRL+C", "Copy the marked text (nothing marked: the whole field)"},
    {nullptr, "CTRL+X", "Cut the marked text"},
    {nullptr, "CTRL+V", "Paste at the caret (replaces the marked text)"},
    {nullptr, "CTRL+z / CTRL+y", "Undo the last change in a text box (a typed run, a deletion or a paste is one step)"},
    // --- Presets menu ---
    {"PRESETS MENU", "TAB", "Cycle focus: SEARCH -> SELECT PRESET -> STATIONS"},
    {nullptr, "ARROW KEYS", "Navigate the focused pane"},
    {nullptr, "ENTER", "STATIONS: tune the hovered station / SELECT PRESET: open the preset and tune its first filled slot"},
    {nullptr, "/", "Search stations (type p: into the empty box to search presets, s: for stations again)"},
    {nullptr, "SHIFT+LEFT/RIGHT", "Previous / next preset, from any pane except the search box (there it marks text)"},
    {nullptr, "ESC", "Clear the search / close the menu"},
    {nullptr, "?", "This cheatsheet (not while typing in the search box)"},
    {nullptr, "CTRL+C", "Quit (in the search box it copies instead)"},
    {"STATION LISTS MENU (SHIFT+k)", "LEFT/RIGHT", "Switch tab: CREATE / EDIT <-> SAVED STATION LISTS; in a name / search box after typing: move the caret"},
    {nullptr, MUISC_ALT_NAME_UC "+LEFT/RIGHT", "Switch tab from every pane, also while typing"},
    {nullptr, "TAB", "Cycle focus. Tab 1: name, search, STATIONS, LIST CONTENTS. Tab 2: search, list"},
    {nullptr, "ENTER", "Name: on to the search / search, STATIONS: add hovered station / LIST CONTENTS: tune it"},
    {nullptr, "ENTER", "Tab 2: search -> the list / STATION LISTS: load the hovered list into tab 1"},
    {nullptr, "CTRL+s", "Save the list under its name (from every pane of tab 1). Same name overwrites"},
    {nullptr, "SHIFT+t", "STATIONS: toggle the sort, list order <-> name A-Z (also for the search results)"},
    {nullptr, "DEL / BACKSPACE / D", "LIST CONTENTS: remove the hovered station from the list"},
    {nullptr, "4 / 5", "LIST CONTENTS: move the hovered station up / down"},
    {nullptr, "DEL", "Tab 2, STATION LISTS: delete the hovered list (asks first)"},
    {nullptr, "/", "Back to the search box of the tab (not while typing)"},
    {nullptr, "ESC", "Leave the box you typed in (LEFT/RIGHT switch tabs again). Then: a search with text is cleared, else the menu closes (unsaved changes: asks Y / N / ESC)"},
    {nullptr, "J / K", "Move the cursor down / up in a list pane"},
    {"BIG STATIONS OVERLAY (SHIFT+l)", "/", "Search like in the main screen (typo tolerant; p: searches the station lists, ENTER opens one and tunes its first station, s: back to stations)"},
    {nullptr, "UP / DOWN", "Move the cursor; ENTER tunes the hovered station; n / b / # / p / x / + / - work as in the main screen"},
    {nullptr, "A", "Add a station by its stream URL (TAB: URL <-> NAME; an empty NAME becomes the host name); saved to stations.txt"},
    {nullptr, "SHIFT+c", "Edit entry: name, PRESET NAME (a shorter second name for the PRESETS pane), URL, genre, country, codec, kbps (TAB / UP / DOWN: field, ENTER saves)"},
    {nullptr, "DEL", "Delete the hovered station (a warning asks Y / N first); it leaves stations.txt, the presets and the station lists"},
    {nullptr, "SHIFT+u", "Undo the last deletion or edit of a station (up to 20 steps)"},
    {nullptr, "SHIFT+t", "Sort: list order <-> name A-Z"},
    {nullptr, "ESC / SHIFT+l", "Clear the search, leave a list, then close the overlay"},
    {"LISTENING HISTORY MENU (h)", "LEFT / RIGHT / TAB / 1 2 3", "Switch tab: HISTORY / TOP CHANNELS / HABITS"},
    {nullptr, "UP / DOWN / J / K", "Move the cursor (HISTORY, TOP CHANNELS) or scroll (HABITS); HOME / END (or G / SHIFT+g) jump to the ends"},
    {nullptr, "R", "TOP CHANNELS: most <-> least listened first"},
    {nullptr, "Y", "HISTORY: search the hovered line (artist + title) on YouTube in a small overlay"},
    {nullptr, "ENTER / TAB / ESC", "YouTube overlay: search, or download the hovered result / edit the query <-> results / close"},
    {nullptr, "ESC / q / h", "Close the history"},
    {"RADIO BROWSER MENU (SHIFT+s)", "TAB", "Cycle focus: NAME -> TAGS -> COUNTRY -> STATE -> LANGUAGE -> BITRATE -> RESULTS"},
    {nullptr, "ENTER", "In a pane: run the search (focus jumps to RESULTS) / in RESULTS: tune the hovered station"},
    {nullptr, "UP / DOWN", "In a pane: jump to the pane above / below (down from the last row: RESULTS) / in RESULTS: move the cursor"},
    {nullptr, "LEFT / RIGHT", "In a pane: move the caret (text-field keys above)"},
    {nullptr, "A", "In RESULTS: add the hovered station to your station list (saved to stations.txt, marked with a dot)"},
    {nullptr, "/", "In RESULTS: back to the NAME pane"},
    {nullptr, "ESC", "In a pane: clear it; empty pane or RESULTS: close the menu"},
    {nullptr, "?", "This cheatsheet (RESULTS only; in a pane the key is typed)"},
    {nullptr, "CTRL+C", "Quit (in a pane it copies instead)"},
    {"RADIO BROWSER MENU: THE PANES", "NAME", "Part of the station name"},
    {nullptr, "TAGS", "Comma separated, every tag has to match: rock, 80s"},
    {nullptr, "COUNTRY", "A name (Germany) or a 2-letter code (DE)"},
    {nullptr, "STATE", "Region inside the country (part of the name is enough)"},
    {nullptr, "LANGUAGE", "As Radio Browser spells it: german, english"},
    {nullptr, "BITRATE", "kbps: 128 = at least 128, 64-192 = range, -192 = at most 192"},
    {"PRESETS MENU: STATIONS PANE", "1-0 / E R T D F G", "Set the hovered station as that slot (same key again clears it)"},
    {nullptr, "DEL / BACKSPACE", "Remove the hovered station from the active preset"},
    {nullptr, "J / K", "Move the cursor down / up"},
    {"PRESETS MENU: SELECT PRESET PANE", "ARROWS / H J K L", "Move in the 4-column grid"},
    {nullptr, "ENTER", "Open the hovered preset and tune its first filled slot"},
    {nullptr, "SHIFT+n", "New preset (opens the name box)"},
    {nullptr, "SHIFT+c", "Rename the hovered preset"},
    {nullptr, "SHIFT+d", "Delete the hovered preset (press twice; the last preset cannot be deleted)"},
    {nullptr, "ENTER / ESC", "Name box: confirm / cancel"},
    {"RADIO SETTINGS (s)", "TAB", "Next tab: COLORS / GENERAL / ANIMA/VIZ / PATHS / REFERENCE / ABOUT APP"},
    {nullptr, "UP / DOWN", "Move through the rows (LEFT / RIGHT or ENTER change a switch; ENTER edits a colour, a folder or a key)"},
    {nullptr, "LEFT / RIGHT (editing)", "Move the caret in the colour, folder or key being edited; TAB and the arrows stay in the field until ENTER (apply) or ESC (cancel)"},
    {nullptr, "ENTER (THEME row)", "COLORS tab: ENTER opens CHOOSE THEME (scrollable, ENTER applies, SHIFT+d deletes a theme of your own); the CREATE THEME button builds your own theme"},
    {nullptr, "e", "COLORS tab: edit the theme in use in the theme creator (a theme of your own can be overwritten, a built-in one needs a new name)"},
    {nullptr, "SHIFT+e / SHIFT+i", "COLORS tab: export the current colors as a theme file / import theme files from a folder"},
    {nullptr, "s", "Save radio_config.txt and close (ESC / q close without saving; with unsaved changes it asks first)"},
    {nullptr, "ESC / q", "Discard the changes made on the screen and close"},
    {nullptr, "GENERAL: MARQUEE SCROLLING", "A station name that is too long for its column scrolls; off: it is cut off with ..."},
    {nullptr, "GENERAL: TUNING NOISE", "Radio static that fades in when you tune another station and fades out when the stream plays"},
    {nullptr, "REFERENCE: ENTER", "Change the key of the hovered command; a key that is already taken is refused with the name of its owner"},
    {nullptr, "REFERENCE: DEL", "Restore the default key of the hovered command"},
    {nullptr, "REFERENCE: RESET", "The first row under the note: every key back to its default"},
    {nullptr, "CTRL+SHIFT+u", "Undo the last key change or reset (up to 5, newest first); the cursor jumps to the key that came back"},
};

// ===========================================================================================
// The big STATIONS overlay (key L): the main STATIONS pane with the whole screen for its rows.
// ===========================================================================================
constexpr int kAddOverlayW = 76;
constexpr int kEditOverlayW = 76;
constexpr int kDelOverlayW = 64;

// ===========================================================================================
// LISTENING HISTORY (Shift+H) -- laid out like the music player's history screen: a tab strip, one panel, a legend and a
// status line. The YouTube overlay (`y` on the HISTORY tab) is drawn over the finished screen.
// ===========================================================================================
constexpr int kYtOverlayW = 80;
constexpr int kYtResultRows = 8;

// ===========================================================================================
// RADIO SETTINGS (key `s`) -- same look as the music player's settings screen: square frame in the gradient colours,
// the tab strip ("[GENERAL]" = current tab), one row per setting. COLORS: colour cells with a live preview on the
// right; GENERAL: toggles.
// ===========================================================================================
constexpr const char* kSettingsTabs[6] = {"COLORS", "GENERAL", "ANIMA/VIZ", "PATHS", "REFERENCE", "ABOUT APP"};

// CHOOSE AUDIO DEVICE (overlay 9 over the main screen, or over the settings): the sound outputs of this computer, the one in use
// marked; keys in radio_main.cpp. The same rows as the music player's overlay (src/app_audiodevice.cpp).
constexpr int kAdevOverlayW = 64;

// SPECTRUM BARS OPTIONS (overlay 10, SHIFT+b): the rows of the music player's overlay (bars_rows.h). It sits under the upper pane
// (so the bars stay in view) and gets a scroll bar when the terminal is too low for all rows.
constexpr int kBarsOverlayW = 52;

// ===========================================================================================
// Overlays over the main screen: SHIFT+o oscilloscope tuning, SHIFT+v loudness normalisation (same rows, ranges and
// keys as the music player's).
// ===========================================================================================
constexpr int kOsciOverlayW = 50;
constexpr int kNormOverlayW = 54;
constexpr int kSleepOverlayW = 52;
constexpr int kUpdateOverlayW = 56;
constexpr int kRecordOverlayW = 56;
constexpr int kEqOverlayW = 58;
constexpr int kSpectroOverlayW = 52;
constexpr int kSleepMinutes[5] = {15, 30, 60, 90, 120};

// ---- the functions (defined in radio_ui_a.cpp, radio_ui_b.cpp and radio_ui.cpp) ----
std::vector<Seg> legend_segs(const std::string& key, const std::string& cmd, const std::string& text);
std::string seg_line(const std::vector<Seg>& segs, int width);
std::string spaces(int n);
std::string repeat(const std::string& s, int n);
std::vector<Seg> volume_fill_segs(const Style& s, int hashes);
std::string box_edge(const Style& s, bool top, int w, const std::vector<Seg>& label, const std::string& ansi);
std::string box_top(const Style& s, int w, const std::string& label, const std::string& ansi);
std::string box_bottom(const Style& s, int w, const std::string& label, const std::string& ansi);
std::string box_line(const Style& s, const std::string& content, const std::string& ansi);
std::string field_row(const std::string& base, const std::string& prefix, const std::string& text, const EditState& e,
                      bool focused, int inner, const std::string& tail, const std::string& tail_ansi, bool required,
                      const std::string& query_ansi = "");
std::string dim_ansi(const std::string& ansi, float k);
std::string fmt_hms(double sec);
std::string fmt_mhz(double f);
float bezier_ease(float x);
std::vector<unsigned char> circle_burst_mask(int cols, int rows, float f);
void append_braille(std::string& out, unsigned char bits);
std::vector<std::string> build_left(const Style& s, const UiModel& m, const RadioStatus& st);
std::vector<std::string> build_meta(const Style& s, const RadioStatus& st, const std::vector<int>* viz_bars,
                                    const std::vector<std::string>* hifi_lines = nullptr);
int follow_scroll(const void* key, int cursor, int rows, int total);
bool parse_rgb(const std::string& ansi, int& r, int& g, int& b);
void temperature_rgb(float t, int& r, int& g, int& b);
void palette_rgb(const Style& s, int id, float t, int& r, int& g, int& b);
std::string rgb_seq(int r, int g, int b, float k);
std::string utf8_cp(int cp);
RadioScope::Params scope_params(const RadioSettings& c, double dt, int style = -1);
bool build_scope_image(const Style& s, const UiModel& m, RadioEngine& engine, int scope_w);
std::vector<std::string> build_scope(const Style& s, const UiModel& m, RadioEngine& engine, bool idle, int scope_w);
std::vector<std::string> build_satellite(const Style& s, double t_sec, int w);
std::vector<std::string> build_sphere(const Style& s, const std::vector<int>& bars, double dt, int w);
int dial_x(double mhz, int w);
std::vector<std::string> build_tuner(const Style& s, const UiModel& m, const RadioStatus& st, int tuner_w);
std::vector<std::string> build_side(const Style& s, const RadioStatus& st);
std::vector<std::string> build_search(const Style& s, const UiModel& m, const RadioStatus& st, int W);
std::string marquee_or_truncate(int& marquee_row, double& marquee_since, double now, const std::string& text, int width, int row_id);
std::vector<std::string> build_lists(const Style& s, const UiModel& m, const RadioStatus& st, int W, int list_rows);
std::string pane_title(const Style& s, const std::string& t, bool focused);
std::vector<std::string> build_preset_pane(const Style& s, const UiModel& m, int W);
std::string splice_line(const std::string& base, int x, const std::string& panel, int pw);
std::vector<std::string> build_name_overlay(const Style& s, const NameOverlay& n);
void push_legend(std::vector<std::string>& out, const Style& s, const std::vector<LegendGroup>& groups, int W, int max_rows = 0);
std::vector<std::string> build_menu(const Style& s, const UiModel& m, const RadioStatus& st, int W, int rows);
std::vector<std::string> menu_tab_strip(const Style& s, int W, const std::string& title, const std::vector<std::string>& labels,
                                        int cur, const std::string& top_ansi, const std::string& bot_ansi,
                                        int min_inner = 0, const std::vector<Seg>& line2 = {}, int line2_cols = 0);
std::vector<std::string> lists_station_pane(const Style& s, const UiModel& m, const RadioStatus& st, int w, int L,
                                            const std::string& title, const std::vector<int>& rows, int cursor,
                                            bool hover, bool numbers, const std::vector<int>* marks,
                                            int& mq_row, double& mq_since, const std::string& empty_msg);
std::vector<std::string> build_lists_menu(const Style& s, const UiModel& m, const RadioStatus& st, int W, int rows);
std::vector<std::string> build_browse(const Style& s, const UiModel& m, const RadioStatus& st, int W, int rows);
std::string pretty_key(const std::string& k);
std::vector<std::string> wrap_words(const std::string& text, int width);
std::vector<std::string> build_wizard(const Style& s, const UiModel& m, int W, int rows);
std::vector<std::string> build_cheatsheet(const Style& s, const UiModel& m, int W, int rows);
std::vector<LegendGroup> stations_legend_groups(const StationsOverlay& so);
std::vector<std::string> build_stations_overlay(const Style& s, const UiModel& m, const RadioStatus& st, int W, int rows);
std::string fmt_clock_len(double sec);
std::vector<std::string> build_history_menu(const Style& s, const UiModel& m, int W, int rows);
std::vector<std::string> build_theme_picker(const RadioSettings& c, const SettingsModel& sm, int W, int rows);
std::vector<std::string> build_theme_io(const RadioSettings& c, const SettingsModel& sm, int W, int rows);
std::vector<std::string> build_settings_impl(const RadioSettings& c, const SettingsModel& sm, int W, int rows, const SettingsModel* cr);
std::vector<std::string> build_adev_overlay(const RadioSettings& c, const SettingsModel& sm);
std::vector<std::string> build_bars_overlay(const RadioSettings& c, const UiModel& m);
std::vector<std::string> build_settings(const RadioSettings& c, const SettingsModel& sm, int W, int rows);
std::string fmt_loudness(double v);
std::string menus_key_label(const RadioSettings& c, const char* id);
std::string box_bottom_centred(const Style& s, int w, const std::string& text, const std::string& ansi);
void push_legend_block(const Style& s, std::vector<std::string>& lines, int W, const std::vector<std::string>& rows);
std::vector<std::string> build_update_overlay(const Style& s, const UiModel& m);
std::vector<std::string> build_overlay(const Style& s, const UiModel& m, const RadioStatus& st);

} // namespace muisc::radio::detail
