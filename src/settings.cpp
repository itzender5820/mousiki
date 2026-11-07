// Settings: reads and writes config.txt, converts colour names to terminal codes, and holds the
// defaults and the key bindings of the program.
#include "settings.h"
#include "utf8_util.h"
#include "string_util.h"
#include "keyboard_layout.h"
#include "translit.h"
#include "lyrics_fetcher.h"
#include "path_utf8.h"
#include "themes.h"
#include "version.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace muisc {

// =====================================================================
// Preset ANSI codes
// =====================================================================

// Migrates a color value from the old named-preset scheme ("cyan",
// "white", ...) to the new bare-number scheme, so a config.txt saved by
// an older build keeps working (and importantly, keeps *displaying* as a
// number in the settings panel) instead of silently going colorless the
// first time ansi_for() can't std::stoi() it. Anything already numeric,
// empty, or unrecognized passes through unchanged.
std::string normalize_color_value(const std::string& value) {
    static const std::unordered_map<std::string, std::string> legacy = {
        {"black", "0"}, {"red", "9"}, {"green", "10"}, {"yellow", "11"},
        {"blue", "12"}, {"magenta", "13"}, {"cyan", "14"}, {"white", "15"}, {"gray", "8"}, {"grey", "8"},
    };
    auto it = legacy.find(value);
    return (it != legacy.end()) ? it->second : value;
}

// Every colour number is a 256-colour palette number in every field (text, background, gradient ends alike): "104" is
// palette colour 104 wherever it is typed. 0 / empty = no colour at all (the terminal's own). Numbers above 255 are not
// clamped -- the terminal decides. config.txt files from before "ColorCodes=palette" used 30-37 / 90-97 (text) and
// 40-47 / 100-107 (background) as plain 16-colour terminal codes; load_settings() converts those once (see
// migrate_legacy_colors()).
std::string ansi_for(const std::string& color_name, bool /*bold*/) {
    if (color_name.empty()) return "";
    int v = 0;
    try { v = std::stoi(color_name); } catch (...) { return ""; }
    if (v <= 0) return "";
    return "\x1b[38;5;" + std::to_string(v) + "m";
}

std::string bg_ansi_for(const std::string& color_name) {
    if (color_name.empty()) return "";
    int v = 0;
    try { v = std::stoi(color_name); } catch (...) { return ""; }
    if (v <= 0) return "";
    return "\x1b[48;5;" + std::to_string(v) + "m";
}

std::string sgr_params_for(const std::string& color_name) {
    if (color_name.empty()) return "";
    int v = 0;
    try { v = std::stoi(color_name); } catch (...) { return ""; }
    if (v <= 0) return "";
    return "38;5;" + std::to_string(v);
}

// =====================================================================
// RGB extraction & brightness
// =====================================================================

// Approximate RGB table for ANSI 256-color indices 0-255.
namespace {
struct RGB { int r, g, b; };

RGB ansi256_to_rgb(int idx) {
    // Standard 16 colors (0-15)
    static const RGB basic16[16] = {
        {0,0,0}, {128,0,0}, {0,128,0}, {128,128,0},
        {0,0,128}, {128,0,128}, {0,128,128}, {192,192,192},
        {128,128,128}, {255,0,0}, {0,255,0}, {255,255,0},
        {0,0,255}, {255,0,255}, {0,255,255}, {255,255,255}
    };
    if (idx >= 0 && idx < 16) return basic16[idx];
    // 216-color cube (indices 16-231): 6×6×6
    if (idx >= 16 && idx <= 231) {
        int ci = idx - 16;
        int bi = ci % 6;
        int gi = (ci / 6) % 6;
        int ri = ci / 36;
        auto v = [](int c) -> int { return c == 0 ? 0 : 55 + 40 * c; };
        return {v(ri), v(gi), v(bi)};
    }
    // Grayscale (indices 232-255)
    if (idx >= 232 && idx <= 255) {
        int g = 8 + (idx - 232) * 10;
        return {g, g, g};
    }
    return {255, 255, 255};
}
} // namespace

// The config files are written to "<name>.tmp" first and then renamed over the real one: a crash or a power cut in the middle
// of a save leaves the old file, never a cut-off one. (When the rename fails -- a file system without it -- it is written in place.)
bool write_text_atomic(const fs::path& p, const std::string& text) {
    fs::path tmp = p;
    tmp += ".tmp";
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) return false;
        out << text;
        out.flush();
        if (!out.good()) { out.close(); fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, p, ec);
    if (!ec) return true;
    fs::remove(tmp, ec);
    std::ofstream direct(p, std::ios::trunc);
    if (!direct.is_open()) return false;
    direct << text;
    return direct.good();
}

bool try_parse_rgb(const std::string& color_value, int& r, int& g, int& b) {
    if (color_value.empty()) return false;
    int idx;
    try { idx = std::stoi(color_value); } catch (...) { return false; }
    if (idx <= 0 || idx > 255) return false; // 0/"none" has no RGB -- caller (gradient_ansi) already special-cases that
    RGB c = ansi256_to_rgb(idx);
    r = c.r; g = c.g; b = c.b;
    return true;
}

int brightness_percent(const std::string& color_value) {
    int r, g, b;
    if (!try_parse_rgb(color_value, r, g, b)) return -1;
    double lum = 0.299 * r + 0.587 * g + 0.114 * b;
    return static_cast<int>(std::clamp(lum / 255.0 * 100.0, 0.0, 100.0));
}

// =====================================================================
// Gradient interpolation
// =====================================================================

std::string gradient_ansi(const std::string& start, const std::string& end, float t, bool bold) {
    if (end.empty()) return ansi_for(start, bold);
    int r0, g0, b0, r1, g1, b1;
    if (!try_parse_rgb(start, r0, g0, b0) || !try_parse_rgb(end, r1, g1, b1)) {
        return ansi_for(start, bold);
    }
    t = std::clamp(t, 0.0f, 1.0f);
    int r = static_cast<int>(r0 + (r1 - r0) * t);
    int g = static_cast<int>(g0 + (g1 - g0) * t);
    int b = static_cast<int>(b0 + (b1 - b0) * t);
    return "\x1b[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

std::string multi_stop_gradient_ansi(const std::string& left, const std::string& center,
                                      const std::string& right, float t) {
    if (center.empty()) return gradient_ansi(left, right, t);
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= 0.5f) {
        return gradient_ansi(left, center, t * 2.0f);
    } else {
        return gradient_ansi(center, right, (t - 0.5f) * 2.0f);
    }
}

// A horizontal strip of `width` filled blocks, smoothly interpolated
// between `start` and `end` -- used for the small preview swatches in the
// settings panel (e.g. Border/Disk/Viz's Top-Bottom color pair).
std::string gradient_preview_bar(const std::string& start, const std::string& end, int width) {
    std::string out;
    for (int i = 0; i < width; ++i) {
        float t = (width > 1) ? static_cast<float>(i) / (width - 1) : 0.0f;
        out += gradient_ansi(start, end, t, false) + "\u2588";
    }
    return out + "\x1b[0m";
}

// =====================================================================
// ANSI 256-color + brightness parsing
// =====================================================================

std::string parse_ansi256_brightness(const std::string& value) {
    // Legacy-settings.txt-migration format: "index,brightness" (e.g.
    // "39,100"). The rest of the app now only understands bare 256-color
    // indices ("0".."255"), so brightness scaling can't be preserved here
    // the way a truecolor SGR string would (that form is not a valid stored value) -- this just recovers the
    // base index and drops the brightness scaling.
    auto comma = value.find(',');
    if (comma == std::string::npos) return value;
    try {
        int idx = std::stoi(value.substr(0, comma));
        if (idx >= 0 && idx <= 255) return std::to_string(idx);
    } catch (...) {}
    return value;
}

// =====================================================================
// Font map
// =====================================================================

std::string apply_font_map(const std::string& text_in,
                            const std::unordered_map<char, std::pair<std::string, std::string>>& font_map) {
    // the Latin letters switch (HKeyTranslit). (Done once: kanji that stay in the result must not be converted again -- that recursed forever.)
    const std::string converted = (translit_enabled() && translit_possible(text_in)) ? transliterate(text_in) : std::string();
    const std::string& text = converted.empty() ? text_in : converted;
    if (font_map.empty()) return text;
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) { 
            out += c; 
            continue; 
        } // pass multi-byte UTF-8 through untouched
        
        char key = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        auto it = font_map.find(key);
        if (it != font_map.end()) {
            bool upper = std::isupper(static_cast<unsigned char>(c));
            out += upper ? it->second.first : it->second.second;
        } else {
            out += c;
        }
    }
    return out;
}

// =====================================================================
// Play mode
// =====================================================================

const char* play_mode_name(int mode) {
    switch (mode) {
        case 1: return "loop";
        case 2: return "shuffle";
        case 3: return "stop";
        case 4: return "queue then stop";
        default: return "list";
    }
}

// =====================================================================
// Themes
// =====================================================================

const std::vector<std::string>& theme_names() {
    static const std::vector<std::string> names = {"default", "neon", "mono", "sunset", "forest"};
    return names;
}

void apply_theme(Settings& s, const std::string& theme_name) {
    if (theme_name == "neon") {
        s.disk_color = "13"; s.disk_color_end = "14";
        s.border_color = "13";
        s.active_line_color = "15"; s.active_word_color = "10"; s.inactive_line_color = "8";
        s.progress_remaining_color = "8"; s.progress_played_color = "10";
        s.list_color = "15"; s.list_playing_color = "10"; s.list_cursor_color = "13";
        s.queue_color = "15"; s.queue_playing_color = "10";
        s.button_color = "13";
        s.visualizer_color = "13"; s.visualizer_color_end = "10";
    } else if (theme_name == "mono") {
        s.disk_color = "15"; s.disk_color_end.clear();
        s.border_color = "8";
        s.active_line_color = "15"; s.active_word_color = "15"; s.inactive_line_color = "8";
        s.progress_remaining_color = "8"; s.progress_played_color = "15";
        s.list_color = "15"; s.list_playing_color = "15"; s.list_cursor_color = "8";
        s.queue_color = "15"; s.queue_playing_color = "15";
        s.button_color = "15";
        s.visualizer_color = "15"; s.visualizer_color_end.clear();
    } else if (theme_name == "sunset") {
        s.disk_color = "11"; s.disk_color_end = "9";
        s.border_color = "9";
        s.active_line_color = "11"; s.active_word_color = "9"; s.inactive_line_color = "8";
        s.progress_remaining_color = "8"; s.progress_played_color = "11";
        s.list_color = "15"; s.list_playing_color = "11"; s.list_cursor_color = "9";
        s.queue_color = "15"; s.queue_playing_color = "11";
        s.button_color = "9";
        s.visualizer_color = "11"; s.visualizer_color_end = "9";
    } else if (theme_name == "forest") {
        s.disk_color = "10"; s.disk_color_end = "11";
        s.border_color = "10";
        s.active_line_color = "15"; s.active_word_color = "10"; s.inactive_line_color = "8";
        s.progress_remaining_color = "8"; s.progress_played_color = "10";
        s.list_color = "15"; s.list_playing_color = "10"; s.list_cursor_color = "10";
        s.queue_color = "15"; s.queue_playing_color = "10";
        s.button_color = "10";
        s.visualizer_color = "10"; s.visualizer_color_end = "11";
    } else { // "default"
        s.disk_color = "14"; s.disk_color_end = "12";
        s.border_color = "8";
        s.active_line_color = "15"; s.active_word_color = "14"; s.inactive_line_color = "8";
        s.progress_remaining_color = "8"; s.progress_played_color = "14";
        s.list_color = "15"; s.list_playing_color = "14"; s.list_cursor_color = "14";
        s.queue_color = "15"; s.queue_playing_color = "14";
        s.button_color = "15";
        s.visualizer_color = "14"; s.visualizer_color_end = "13";
    }
    s.theme_name = theme_name;
}

// =====================================================================
// Default hotkeys
// =====================================================================

// SHIFT+b: the bookmark in the audio book mode, the tag fetch inside the meta editor, the spectrum bars overlay everywhere else.
bool shares_shift_b(const std::string& a, const std::string& b) {
    auto in = [](const std::string& x) { return x == "HKeyBookmark" || x == "HKeyFetchTags" || x == "HKeyBarsMenu"; };
    return a != b && in(a) && in(b);
}

void apply_default_hotkeys(Settings& s) {
    // Merged per-key (not "only if the whole map is empty") so that a
    // config.txt which only sets SOME hotkeys -- e.g. an older config
    // saved before new actions like HKeyConsole/HKeyToggleMute existed,
    // or one where the user deliberately left a line out/blank -- still
    // gets sane defaults for whatever it didn't specify, instead of
    // those actions silently having no key bound at all.
    {
        static const std::unordered_map<std::string, std::string> defaults = {
            {"HKeySetting",                     "s"},
            {"HKeyNavigateUp",                  "ARROW_KEY_UP"},
            {"HKeyNavigateDown",                "ARROW_KEY_DOWN"},
            {"HKeyPlay",                        "ENTER"},
            {"HKeySearch",                      "/"},
            {"HKeyPlayNextSong",                "n"},
            {"HKeyPlayPreviousSong",            "b"},
            {"HKeySeekForward",                 "ARROW_KEY_RIGHT"},
            {"HKeySeekBackward",                "ARROW_KEY_LEFT"},
            // "+" / "-" = volume, the same keys as in the radio mode. Older configs
            // (volume on "1" / "2", lyrics cycle on "+") are migrated below.
            {"HKeyIncreaseVolume",              "+"},
            {"HKeyDecreaseVolume",              "-"},
            {"HKeyAddHoveringSongToQueue",      "a"},
            {"HKeyRemoveHoveringSongFromQueue", "d"},
            {"HKeySwitchBetweenCards",          "TAB"},
            {"HKeyTogglePlayPause",             "p"},
            {"HKeyCyclePlayMode",               "m"},
            {"HKeyFilterForFolder",             "f"},
            {"HKeyClearFilter",                 "c"},
            {"HKeyQuit",                        "q"},
            {"HKeyDownloadStream",              "y"},
            // "k" opens the karaoke overlay; Refresh UI is on "r" (migrated below).
            {"HKeyRefreshUi",                   "r"},
            {"HKeyKaraoke",                     "k"},
            {"HKeyVideoFxBack",                 "h"},   // the previous video effect for this track (the other direction of "g")
            {"HKeyVideoFx",                     "g"},   // the next video effect for this track (the picture in the small field)
            {"HKeyVideoFull",                   "v"},   // the full screen video (the loudness normalization key moved to the SHIFT+v overlay)
            {"HKeyVideo",                       default_video_key()},   // "&" (SHIFT+6) on German / Austrian / Swiss keyboards, else "Y" (SHIFT+y)
            // The mode switch (SHIFT and + / SHIFT and - on a German keyboard: the characters '*' and '_') and the karaoke
            // overlay's lyrics size keys. The two pairs share their characters on purpose: they work in different places
            // (the size keys only inside the karaoke overlay).
            {"HKeyModeNext",                    default_size_mode_key(true)},    // "*" / "_", on a US / UK keyboard "'" / ";"
            {"HKeyModePrev",                    default_size_mode_key(false)},
            {"HKeyCoverPrev",                   "CTRL+T"},   // the cover look one step back / forward (Eliment Disk / Cover on)
            {"HKeyCoverNext",                   "CTRL+Z"},
            {"HKeyKaraokeBigger",               default_size_mode_key(true)},
            {"HKeyKaraokeSmaller",              default_size_mode_key(false)},
            {"HKeyConsole",                     "t"},
            {"HKeyAudioDevice",                 default_audio_device_key()},   // "°" (SHIFT+^) on German / Austrian / Swiss keyboards, else "&"
            {"HKeyToggleMute",                  "x"},
            {"HKeyCheatsheet",                  "?"},
            {"HKeyMenus",                       "<"},   // the index of all menus and overlays
            // The help wizard. "W" (SHIFT+w) is free: the scope window uses ")" (see the migration below),
            // and plain "w" stays Toggle Waveform.
            {"HKeyWizard",                      "W"},
            {"HKeyRetryLyrics",                 "l"},
            {"HKeyShuffleNext",                 "#"},
            {"HKeyToggleLyrics",                "."},
            {"HKeyToggleLyricsBack",            ","},
            {"HKeyQueueMoveUp",                 "4"},
            {"HKeyQueueMoveDown",               "5"},
            {"HKeyToggleWaveform",              "w"},
            {"HKeyCycleSortMode",               "T"},
            {"HKeyPlaylist",                    "P"},
            {"HKeyToggleNormalize",             ""},    // unbound by default (SHIFT+v opens the normalization overlay, which switches it); "v" is the full screen video
            // Shift+N: deliberately the UPPERCASE letter, because plain
            // "n" is already HKeyPlayNextSong and resolve_hotkey_action()
            // only falls back to the other case when a key resolves to
            // nothing (app.cpp handle_key) -- so "N" gives a dedicated
            // Shift+N binding while "n" keeps meaning "next track".
            {"HKeyToggleMetaOnly",              "N"},
            // Shift+M: the meta/tag editor overlay. Same uppercase trick as
            // HKeyToggleMetaOnly above -- plain "m" stays HKeyCyclePlayMode.
            // (SHIFT+b, "fetch metadata for the hovered title", is NOT a
            // hotkey: "B" is indistinguishable from the Down-arrow's
            // collapsed 'B' code by value alone, so it is matched directly
            // in handle_key() together with last_key_was_arrow() instead.)
            {"HKeyMetaEditor",                  "M"},
            // Shift+H: the listening-history overlay (HISTORY / TOP TRACKS
            // / HABITS). Same uppercase convention as M and P above --
            // nothing claimed plain "h", but SHIFT+h is what the overlay is
            // entered with.
            {"HKeyHistory",                     "H"},
            // Shift+X: clear the whole queue (after a Yes/No confirmation).
            // Uppercase on purpose -- plain "x" is HKeyToggleMute.
            {"HKeyClearQueue",                  "X"},
            // Shift+L: the big list overlay (a larger LOCAL AUDIO FILES pane
            // floated over the main UI). Uppercase on purpose -- plain "l" is
            // HKeyRetryLyrics.
            {"HKeyListOverlay",                 "L"},
            // Shift+F: the big folder overlay (a tree of the library's folders;
            // `f` inside it shows a folder in the list). Uppercase on purpose --
            // plain "f" is HKeyFilterForFolder.
            {"HKeyFolderOverlay",               "F"},
            // Shift+C: the file conversion overlay (high resolution files -> MP3 and other lighter formats).
            // Uppercase on purpose: a lowercase "c" would collide with the arrow-key codes (Shift+c was read as Right =
            // seek forward). See App::resolve_hotkey_action().
            {"HKeyConvertOverlay",              "C"},
            // Shift+K: the big queue overlay (a larger QUEUE pane floated
            // over the main UI). Uppercase on purpose -- plain "k" is
            // HKeyRefreshUi, and "q" stays Quit so the overlay can't quit
            // the app by accident.
            {"HKeyQueueOverlay",                "K"},
            // Shift+O: the oscilloscope tuning overlay (decay / dot
            // threshold / tail brightness, live). Uppercase on purpose, same
            // convention as the other SHIFT+letter overlays above.
            {"HKeyOscMenu",                     "O"},
            // The oscilloscope / the spectrogram in their own windows (drawn by the graphics card, src/scope_window.h):
            // the characters of SHIFT+9 / SHIFT+8 on a German keyboard (on a US keyboard SHIFT+0 / SHIFT+9).
            {"HKeyScopeWindow",                 ")"},
            {"HKeySpectroWindow",               "("},
            // Shift+I / Shift+U: spectrogram options overlay / spectrogram full screen.
            {"HKeySpectroMenu",                 "I"},
            {"HKeySpectroFull",                 "U"},
            // The big oscilloscope overlay (YT left, XY right, optional grid): the character of SHIFT+0 on a German keyboard
            // ("=" -- on a US keyboard the unshifted key next to backspace; ")" is the scope window).
            {"HKeyScopeBig",                    "="},
            // Shift+V: the loudness normalisation overlay (on/off, target
            // level, max boost, live). Uppercase on purpose -- plain "v" is
            // HKeyToggleNormalize, same convention as the other overlays.
            {"HKeyNormMenu",                    "V"},
            {"HKeyBarsMenu",                    "B"},   // SHIFT+b: the spectrum bars' options (in the audio book mode the bookmark has this key)
            // Shift+E: the equaliser overlay (10 bands + presets). Uppercase
            // on purpose -- plain "e" is HKeyQueueAddEnd.
            {"HKeyEqualizer",                   "E"},
            // Shift+Z: the sleep timer overlay (15/30/60/90/120 min, stop after
            // the current song). Uppercase on purpose, same convention as the
            // other SHIFT+letter overlays. (Ctrl+Shift+Z is the separate "undo
            // clear queue" key and never reaches the hotkey table.)
            {"HKeySleepTimer",                  "Z"},
            // ^: the creator overlay (text / picture -> oscilloscope music or spectrogram sound). On a German keyboard
            // ^ is a dead key: press it, then SPACE.
            {"HKeyCreator",                     "^"},
            // Queue additions. "e" = add the hovering track to the END of the
            // queue ("a" adds it as NEXT, see App::queue_add_selected()); "e"
            // Formerly the never-wired HKeyResetPreference; that setting is migrated
            // away below. "!" locks the queue (played tracks stay in it).
            // "$" / "%" are SHIFT+4 / SHIFT+5 on both US and German layouts:
            // move the hovering queue item to the top / bottom.
            {"HKeyQueueAddEnd",                 "e"},
            {"HKeyQueueLock",                   "!"},
            {"HKeyQueueMoveTop",                "$"},
            {"HKeyQueueMoveBottom",             "%"},
            // Keys that were fixed in the code and are now rebindable (Settings -> REFERENCE). The
            // modifier combinations are written as names (ALT+L, CTRL+L, CTRL+SHIFT+Z, CTRL+SHIFT+U); on a Mac
            // "ALT" is the OPTION key.
            {"HKeyRescan",                      "R"},             // SHIFT+r: look for new files
            {"HKeyFetchTags",                   "B"},             // SHIFT+b inside the meta data editor: AcoustID lookup of the hovered title
            {"HKeyBookmark",                    "B"},             // SHIFT+b in the audio book mode: a bookmark (the spectrum bars overlay has the key in the other modes)
#if defined(__APPLE__)
            {"HKeyLyricsTiming",                "J"},             // lyrics timing overlay (SHIFT+j on a Mac: no OPTION key, which types special characters there)
#else
            {"HKeyLyricsTiming",                "ALT+L"},         // lyrics timing overlay
#endif
            {"HKeyTranslit",                    "\""},           // SHIFT+' (US keyboard): Japanese / Korean / Chinese text in Latin letters on / off
#if defined(__APPLE__)
            {"HKeyCoverFetch",                  "Q"},             // cover fetch overlay (SHIFT+q on a Mac: no OPTION key, which types special characters there)
#else
            {"HKeyCoverFetch",                  "ALT+C"},         // cover fetch overlay (MusicBrainz / Cover Art Archive)
#endif
#if defined(__APPLE__)
            {"HKeyVideoFetch",                  "D"},             // fetch videos overlay (SHIFT+d on a Mac: no OPTION key, which types special characters there)
#else
            {"HKeyVideoFetch",                  "ALT+V"},         // fetch videos overlay (yt-dlp)
#endif
            {"HKeyStyleOptions",                "S"},             // SHIFT+s: the Style Option overlay (cover look, contrast, brightness, colors) of the playing track
            {"HKeyReadLyrics",                  "CTRL+L"},        // READ LYRICS overlay
            {"HKeyQueueUndoClear",              "CTRL+SHIFT+Z"},  // bring back the queue that was just cleared
            {"HKeyQueueToPlaylist",             "CTRL+SHIFT+U"},  // save the queue as a playlist
            {"HKeyAddToPlaylist",               "A"},             // SHIFT+a: the highlighted library track -> a playlist
            {"HKeyBulkAdd",                     "G"},             // SHIFT+g: bulk add (a playlist link into the queue), from any pane
        };
        // HKeyResetPreference was defined but nothing ever read it; configs
        // written by older builds still carry it (as "e", which would now
        // collide with HKeyQueueAddEnd -- resolve_hotkey_action() would pick
        // one of the two nondeterministically). Dropped whatever its key is,
        // and no longer written back (see save_settings()'s hkey_order), so
        // the next save removes the line from config.txt for good.
        s.hotkeys.erase("HKeyResetPreference");
        // The three search prefixes (/s: /p: /f:) were listed as hotkeys but typing them was never configurable (the
        // search box parses s: p: f: r: a: sc: b: itself); the lines are dropped so config.txt does not promise anything.
        s.hotkeys.erase("HKeySearchOnline");
        s.hotkeys.erase("HKeySearchPlaylist");
        s.hotkeys.erase("HKeySearchFolder");
        // Migration: volume moved from "1" / "2" to "+" / "-", and the lyrics
        // area cycle from "+" to ".". A config that still carries the old
        // trio exactly as it was shipped is moved over as a whole; a user
        // who rebound any of the three keeps their own keys. The move is
        // skipped if "-" or "." is already taken by another action.
        {
            auto up = s.hotkeys.find("HKeyIncreaseVolume");
            auto down = s.hotkeys.find("HKeyDecreaseVolume");
            auto lyr = s.hotkeys.find("HKeyToggleLyrics");
            if (up != s.hotkeys.end() && down != s.hotkeys.end() && lyr != s.hotkeys.end() &&
                up->second == "1" && down->second == "2" && lyr->second == "+") {
                bool taken = false;
                for (const auto& [action, key] : s.hotkeys) {
                    if (action == "HKeyIncreaseVolume" || action == "HKeyDecreaseVolume" || action == "HKeyToggleLyrics") continue;
                    if (key == "-" || key == ".") { taken = true; break; }
                }
                if (!taken) { up->second = "+"; down->second = "-"; lyr->second = "."; }
            }
        }
        // Migration: "k" is the karaoke overlay and Refresh UI is on "r".
        // A config from before (Refresh UI still on "k", no karaoke key yet)
        // is moved over -- unless the user has put something else on "r".
        if (s.hotkeys.find("HKeyKaraoke") == s.hotkeys.end()) {
            auto ref = s.hotkeys.find("HKeyRefreshUi");
            if (ref != s.hotkeys.end() && ref->second == "k") {
                bool r_taken = false;
                for (const auto& [action, key] : s.hotkeys)
                    if (action != "HKeyRefreshUi" && key == "r") { r_taken = true; break; }
                if (!r_taken) ref->second = "r";
                else s.hotkeys["HKeyKaraoke"] = "";   // no free default: left unbound (rebind it under Settings -> REFERENCE)
            }
        }
        // Migration: the scope window moved from SHIFT+w ("W") to SHIFT+9 (")"), next to the spectrogram window on SHIFT+8.
        {
            auto sw = s.hotkeys.find("HKeyScopeWindow");
            if (sw != s.hotkeys.end() && sw->second == "W") {
                bool taken = false;
                for (const auto& [action, key] : s.hotkeys) if (action != "HKeyScopeWindow" && key == ")") taken = true;
                if (!taken) sw->second = ")";
            }
        }
        // Migration: SHIFT+b opens the spectrum bars overlay (the meta editor keeps it for the tag fetch, the audio book for the bookmark);
        // the audio book bookmark got a command of its own (it was HKeyFetchTags); the user's own binding is kept.
        {
            auto ft = s.hotkeys.find("HKeyFetchTags");   // (it was the audio book's bookmark key too: that command has a key of its own now)
            if (ft != s.hotkeys.end() && s.hotkeys.find("HKeyBookmark") == s.hotkeys.end()) s.hotkeys["HKeyBookmark"] = ft->second;
            auto bm = s.hotkeys.find("HKeyBarsMenu");
            if (bm != s.hotkeys.end() && bm->second == "Y") bm->second = "B";
        }
        // Migration: "v" opens the full screen video; the loudness normalization is switched in the SHIFT+v overlay (or Settings). A config that
        // still has the old default keeps no second command on that key: normalization is left unbound, the video gets the key.
        {
            auto vf = s.hotkeys.find("HKeyVideoFull");
            auto nm = s.hotkeys.find("HKeyToggleNormalize");
            if (nm != s.hotkeys.end() && nm->second == "v" && (vf == s.hotkeys.end() || vf->second == ">" || vf->second == "v")) {
                nm->second = "";
                s.hotkeys["HKeyVideoFull"] = "v";
            }
        }
        std::vector<std::string> added_now;   // commands that came with a newer version
        for (const auto& [action, key] : defaults) {
            // Only fill actions that are entirely absent from the config.
            // A key explicitly set to "" (user unbound it on purpose) is
            // left alone rather than silently re-bound.
            if (s.hotkeys.try_emplace(action, key).second) added_now.push_back(action);
        }
        // The wizard's "W" is the one default that can land on an occupied
        // key: the migration above leaves an old HKeyScopeWindow on "W" when
        // ")" is already taken. Two actions on one key would make
        // resolve_hotkey_action() pick between them at random, so the wizard
        // gives way and is left unbound (rebind it under Settings ->
        // REFERENCE). Only a config written before the wizard existed can
        // reach this.
        // The same goes for the keys that became rebindable later (their old fixed key may have been given to another
        // action in the meantime, which could never fire: the fixed key was caught first).
        // Any command that is new to this config gets the same treatment: the user's own binding on that key wins.
        std::vector<std::string> check = {"HKeyWizard", "HKeyRescan", "HKeyFetchTags", "HKeyBookmark", "HKeyLyricsTiming", "HKeyReadLyrics",
                                          "HKeyQueueUndoClear", "HKeyQueueToPlaylist", "HKeyBulkAdd", "HKeyAddToPlaylist"};
        check.insert(check.end(), added_now.begin(), added_now.end());
        for (const std::string& name : check) {
            auto wiz = s.hotkeys.find(name);
            if (wiz != s.hotkeys.end() && !wiz->second.empty()) {
                for (const auto& [action, key] : s.hotkeys) {
                    if (action == name || key != wiz->second) continue;
                    if (shares_shift_b(action, name)) continue;   // one key, three places (the audio book, the meta editor, everywhere else)
                    // two commands that are both new keep their defaults (a fresh config); only the user's own key wins
                    if (std::find(added_now.begin(), added_now.end(), action) != added_now.end() &&
                        std::find(added_now.begin(), added_now.end(), name) != added_now.end()) continue;
                    wiz->second = ""; break;
                }
            }
        }
    }
}

// =====================================================================
// Config file path
// =====================================================================

fs::path config_path() {
    const char* home = std::getenv("HOME");
    // HOME is stored as UTF-8 by win_bootstrap_env() (it comes out of
    // GetEnvironmentVariableW, which is UTF-16). fs::path(std::string)
    // would re-read those bytes as ANSI, so a user profile with a
    // non-ASCII name produced a garbled base directory.
    fs::path base = home ? path_from_utf8(home) : fs::path(".");
    return base / ".config" / "mousiki" / "config.txt";
}

// Legacy path for migration
static fs::path legacy_settings_path() {
    const char* home = std::getenv("HOME");
    // HOME is stored as UTF-8 by win_bootstrap_env() (it comes out of
    // GetEnvironmentVariableW, which is UTF-16). fs::path(std::string)
    // would re-read those bytes as ANSI, so a user profile with a
    // non-ASCII name produced a garbled base directory.
    fs::path base = home ? path_from_utf8(home) : fs::path(".");
    return base / ".config" / "mousiki" / "settings.txt";
}

// =====================================================================
// Parsing helpers
// =====================================================================


// Strip surrounding quotes: "value" -> value
static std::string unquote(const std::string& s) {
    std::string t = trim_ws(s);
    if (!t.empty() && t.back() == ';') {
        t.pop_back();
        t = trim_ws(t);
    }
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        return t.substr(1, t.size() - 2);
    }
    return t;
}

static const char* viz_default_name(int v) { return v == 0 ? "sphere" : v == 1 ? "osci" : v == 2 ? "spectro" : v == 3 ? "lyrics" : v == 5 ? "video" : "last"; }

static bool parse_bool(const std::string& v) {
    std::string s = trim_ws(v);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s == "yes" || s == "true" || s == "1";
}

int mode_index(const std::string& name) {
    std::string n = trim_ws(name);
    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n == "audio book" || n == "audio-book" || n == "book") n = "audiobook";
    if (n == "music" || n == "music player") n = "player";
    for (int i = 0; i < 3; ++i) if (n == kModeNames[i]) return i;
    return -1;
}

std::string mode_order_text(const int order[3]) {
    std::string t;
    for (int i = 0; i < 3; ++i) t += std::string(i ? "," : "") + kModeNames[std::clamp(order[i], 0, 2)];
    return t;
}

// "player,radio,audiobook" in any order. Names that are missing are appended in their standard place; repeats and
// unknown names are ignored, so the result is always a permutation of 0..2.
static void parse_mode_order(const std::string& value, int order[3]) {
    int out[3], n = 0;
    bool seen[3] = {false, false, false};
    std::string cur;
    const std::string v = unquote(value) + ",";
    for (char c : v) {
        if (c != ',') { cur += c; continue; }
        const int m = mode_index(cur);
        cur.clear();
        if (m >= 0 && !seen[m] && n < 3) { seen[m] = true; out[n++] = m; }
    }
    for (int m = 0; m < 3; ++m) if (!seen[m]) out[n++] = m;
    for (int i = 0; i < 3; ++i) order[i] = out[i];
}

void Settings::mode_normalize() {
    bool seen[3] = {false, false, false};
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        if (mode_order[i] < 0 || mode_order[i] > 2 || seen[mode_order[i]]) ok = false;
        else seen[mode_order[i]] = true;
    }
    if (!ok) { mode_order[0] = 0; mode_order[1] = 1; mode_order[2] = 2; }
    if (!use_mode[0] && !use_mode[1] && !use_mode[2]) use_mode[0] = true;   // one mode is always on
    int sm = mode_index(start_mode);
    if (sm < 0) sm = 0;
    if (!use_mode[sm]) for (int i = 0; i < 3; ++i) if (use_mode[mode_order[i]]) { sm = mode_order[i]; break; }
    start_mode = kModeNames[sm];
}

bool same_folder_path(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    auto norm = [](const std::string& s) {
        std::string n = path_utf8(path_from_utf8(s).lexically_normal());
        for (char& c : n) {
            if (c == '\\') c = '/';
#ifdef _WIN32
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
        }
        while (n.size() > 1 && n.back() == '/') n.pop_back();
        return n;
    };
    return norm(a) == norm(b);
}

bool book_path_conflicts(const std::vector<std::string>& music_paths, const std::string& book) {
    for (const std::string& m : music_paths) if (same_folder_path(m, book)) return true;
    return false;
}

std::string read_audiobook_path() {
    std::ifstream in(config_path());
    std::string line;
    std::string path;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
        if (trim_ws(line.substr(0, eq)) != "AudioBookPath") continue;
        path = trim_ws(unquote(line.substr(eq + 1)));
        if (!path.empty() && path[0] == '~') {
            const char* home = std::getenv("HOME");
            if (home) path = std::string(home) + path.substr(1);
        }
    }
    return path;
}

ModeConfig read_mode_config() {
    ModeConfig mc;
    mc.key_next = static_cast<unsigned char>(default_size_mode_key(true)[0]);
    mc.key_prev = static_cast<unsigned char>(default_size_mode_key(false)[0]);
    std::ifstream in(config_path());
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        const std::string key = trim_ws(line.substr(0, eq)), val = line.substr(eq + 1);
        if (key == "UseModePlayer") mc.use[0] = parse_bool(val);
        else if (key == "UseModeRadio") mc.use[1] = parse_bool(val);
        else if (key == "UseModeAudiobook") mc.use[2] = parse_bool(val);
        else if (key == "ModeOrder") parse_mode_order(val, mc.order);
        else if (key == "HKeyModeNext" || key == "HKeyModePrev") {   // one typed character (empty: no key)
            const std::string v = trim_ws(unquote(val));
            if (v.size() <= 1) (key == "HKeyModeNext" ? mc.key_next : mc.key_prev) = v.empty() ? 0 : static_cast<unsigned char>(v[0]);
        }
    }
    if (!mc.use[0] && !mc.use[1] && !mc.use[2]) mc.use[0] = true;
    return mc;
}

int next_mode_in(const ModeConfig& mc, int cur, int dir) {
    int pos = 0;
    for (int i = 0; i < 3; ++i) if (mc.order[i] == cur) pos = i;
    for (int k = 1; k <= 3; ++k) {
        const int c = mc.order[((pos + (dir >= 0 ? k : -k)) % 3 + 3) % 3];
        if (c != cur && mc.use[c]) return c;
    }
    return cur;
}

// Parse font_en block: font_en={ A={A,a}, B={B,b}, ... };
static void parse_font_block(std::ifstream& in, Settings& s) {
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim_ws(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.find("};") != std::string::npos || t == "}") break;
        // Expected: A={A,a},  or  A={𝓐,𝓪},
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string letter_str = trim_ws(t.substr(0, eq));
        if (letter_str.empty()) continue;
        char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter_str[0])));
        // Parse {upper,lower}
        size_t ob = t.find('{', eq);
        size_t cb = t.find('}', ob != std::string::npos ? ob : 0);
        if (ob == std::string::npos || cb == std::string::npos) continue;
        std::string inner = t.substr(ob + 1, cb - ob - 1);
        size_t comma = inner.find(',');
        if (comma == std::string::npos) continue;
        std::string upper_glyph = trim_ws(inner.substr(0, comma));
        std::string lower_glyph = trim_ws(inner.substr(comma + 1));
        if (!upper_glyph.empty() && !lower_glyph.empty()) {
            s.font_map[letter] = {upper_glyph, lower_glyph};
        }
    }
}

// The ABOUT APP text is hard coded (about_text.h). Older config files still carry it as a
// ClassTextAboutApp={...}; block, which is skipped here and no longer written back.
static void skip_about_app_block(std::ifstream& in) {
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim_ws(line);
        if (t == "};" || t == "}") break;
    }
}

// =====================================================================
// Load settings from config.txt
// =====================================================================

static Settings load_from_config(const fs::path& path) {
    Settings s;
    bool have_viz_default = false;  // no VizDefault= line (a config from before): start with the view it was left on
    bool focus_color_set = false;   // no ColorFocus= line: the focus indicator follows the header color
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return s;

    // Skip a UTF-8 byte-order mark if one is present. Several Windows
    // editors (older Notepad, and some "Save As" dialogs even in current
    // versions) write one when saving as UTF-8, which would otherwise
    // land as three bytes glued onto the front of the first key on the
    // first line (e.g. "\xEF\xBB\xBFElimentDisk=true") and make it fail
    // to match anything below.
    {
        unsigned char bom[3] = {0, 0, 0};
        in.read(reinterpret_cast<char*>(bom), 3);
        std::streamsize got = in.gcount();
        // UTF-16 (either byte order) is the other thing a Windows editor's
        // "Save"/"Save As" can silently pick -- and unlike a UTF-8 BOM,
        // there's no way to transparently paper over it here: every ASCII
        // byte below is now followed by a NUL, so nothing would match and
        // the file would parse as entirely empty with no indication why.
        // Fail loudly and specifically instead.
        if (got >= 2 && ((bom[0] == 0xFF && bom[1] == 0xFE) || (bom[0] == 0xFE && bom[1] == 0xFF))) {
            throw std::runtime_error(
                "this file is saved as UTF-16, not UTF-8 -- re-save it as UTF-8 "
                "(in Notepad: File > Save As > Encoding: UTF-8)");
        }
        if (!(got == 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)) {
            in.clear();
            in.seekg(0);
        }
    }

    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim_ws(line);
        if (t.empty() || t[0] == '#') continue;

        // Font block
        if (t.find("font_en") != std::string::npos && t.find('{') != std::string::npos) {
            parse_font_block(in, s);
            continue;
        }
        // About App text block
        if (t.find("ClassTextAboutApp") != std::string::npos && t.find('{') != std::string::npos) {
            skip_about_app_block(in);
            continue;
        }

        // Try "key : value" (element toggles)
        size_t colon = t.find(':');
        // Try "key == value" (colors, hotkeys, viz)
        size_t eq2 = t.find("==");
        // Try 'key="value"' (border chars)
        size_t eq1 = t.find('=');

        std::string key, value;

        if (eq2 != std::string::npos && (colon == std::string::npos || eq2 < colon)) {
            // "key == value" format
            key = trim_ws(t.substr(0, eq2));
            value = trim_ws(t.substr(eq2 + 2));
        } else if (colon != std::string::npos && (eq1 == std::string::npos || colon < eq1)) {
            // "key : value" format
            key = trim_ws(t.substr(0, colon));
            value = trim_ws(t.substr(colon + 1));
        } else if (eq1 != std::string::npos) {
            // "key=value" or 'key="value"' format
            key = trim_ws(t.substr(0, eq1));
            value = trim_ws(t.substr(eq1 + 1));
        } else {
            continue;
        }

        // --- New CamelCase config key names (current config.txt format) --
        // map straight onto the existing internal keys below, so the rest
        // of this loop doesn't need to change at all. Old-format keys
        // (still handled further down) keep working too, for anyone with
        // an existing config file.
        static const std::unordered_map<std::string, std::string> kKeyAliases = {
            {"ColorBorderTop", "border_color"}, {"ColorBorderBottom", "border_color_bottom"},
            {"ColorDiskTop", "disk_color"}, {"ColorDiskBottom", "disk_color_end"},
            {"ColorMetadataKey", "meta_key_color"}, {"ColorMetadataVal", "meta_val_color"},
            {"ColorVizLeft", "visualizer_color"}, {"ColorVizRight", "visualizer_color_end"},
            {"ColorProgressBarPlayed", "progress_played_color"}, {"ColorProgressBarPending", "progress_remaining_color"},
            {"ColorSearchFg", "search_text_color"}, {"ColorSearchBg", "search_bg_color"},
            {"ColorSearchInactiveFg", "search_inactive_text_color"}, {"ColorSearchInactiveBg", "search_inactive_bg_color"},
            {"ColorListInactiveFg", "list_color"}, {"ColorListInactiveBg", "list_inactive_bg_color"},
            {"ColorListPlayingFg", "list_playing_color"}, {"ColorListPlayingBg", "list_playing_bg_color"},
            {"ColorListCursorFg", "list_cursor_color"}, {"ColorListCursorBg", "list_cursor_bg_color"},
            {"ColorQueueInactiveFg", "queue_color"}, {"ColorQueueInactiveBg", "queue_inactive_bg_color"},
            {"ColorQueuePlayingFg", "queue_playing_color"}, {"ColorQueuePlayingBg", "queue_playing_bg_color"},
            {"ColorQueueCursorFg", "queue_cursor_color"}, {"ColorQueueCursorBg", "queue_cursor_bg_color"},
            {"ColorLyricsInactiveFg", "inactive_line_color"}, {"ColorLyricsInactiveBg", "inactive_line_bg_color"},
            {"ColorLyricsActiveLineFg", "active_line_color"}, {"ColorLyricsActiveLineBg", "active_line_bg_color"},
            {"ColorLyricsActiveWordFg", "active_word_color"}, {"ColorLyricsActiveWordBg", "active_word_bg_color"},
            {"ColorHeader", "header_color"}, {"ColorLegend", "legend_color"}, {"ColorLegendCommand", "legend_cmd_color"}, {"ColorTabCurrent", "tab_current_color"}, {"ColorTabOther", "tab_other_color"},
            {"ColorChangeFg", "change_text_color"}, {"ColorChangeBg", "change_bg_color"},
            {"ColorFocus", "focus_color"},
            {"MetaDataOnly", "meta_only"},
            {"ElimentDisk", "Eliment_disk"}, {"ElimentDummyButtons", "Element_dummy_buttons"},
            {"ElimentQueue", "Eliment_queue"}, {"ElimentWaveForm", "Eliment_waveform_progress_bar"},
            {"ElimentLyrics", "Eliment_lyrics"}, {"VizLyricsField", "Eliment_vizfield"}, {"LyricsPlaceholderBall", "Eliment_lyrics_placeholder_ball"},
            {"LyricViz", "lyric_viz"}, {"NoLyricsViz", "nolyrics_viz"}, {"VizDefault", "viz_default"}, {"NoVideoViz", "novideo_viz"}, {"VideoMode", "video_mode"}, {"VideoLyrics", "video_lyrics"}, {"VideoAuto", "video_auto"}, {"VideoCover", "video_cover"}, {"VideoSize", "video_size"}, {"VideoFx", "video_fx"}, {"VideoResolution", "video_resolution"}, {"VideoContrast", "video_contrast"}, {"VideoColors", "video_colors"}, {"VideoChroma", "video_chroma"}, {"VideoRound", "video_round"}, {"CoverChroma", "cover_chroma"}, {"VideoBuffer", "video_buffer"}, {"VideoMp3", "video_mp3"}, {"VideoFullLyrics", "video_full_lyrics"}, {"VideoDownHeight", "video_down_height"}, {"VideoView", "video_view"},
            {"Visualizer", "Eliment_visualizer"},
            {"VisualizerFluidity", "visualizer_fluidity"}, {"DiskRotationSpeed", "disk_rotation_speed"},
            {"VisualizerDegradationSpeed", "visualizer_degradation_speed"}, {"VisualizerViscosity", "visualizer_viscosity"},
            {"VisualizerStyle", "bars_style"}, {"HifiResolution", "hifi_resolution"}, {"HifiChannels", "hifi_channels"}, {"HifiColor", "hifi_color"}, {"HifiScale", "hifi_scale"}, {"HifiAdapt", "hifi_adapt"}, {"HifiFluidity", "hifi_fluidity"}, {"HifiDegradationSpeed", "hifi_degradation"}, {"HifiViscosity", "hifi_viscosity"}, {"HifiFrequencyScale", "hifi_fscale"}, {"HifiGain", "hifi_gain"}, {"HifiRange", "hifi_range"}, {"HifiSplit", "hifi_split"},
            {"LyricsAlignment", "lyrics_alignment"}, {"LyricsAnimation", "lyrics_animation"},
            {"UpperLeftCorner", "upper_left_corner"}, {"UpperRightCorner", "upper_right_corner"},
            {"BottomLeftCorner", "bottom_left_corner"}, {"LowerRightCorner", "lower_right_corner"},
            {"Vertical", "vertical"}, {"Horizontal", "horizontal"},
            {"Seprator", "seprator"}, {"ListSeparator", "list_separator"},
        };
        if (osci_config_key(s, key, value)) continue;
        if (spectro_profile_key(s, key, value)) continue;
        if (spectro_config_key(s.spectro, key, value)) continue;
        {
            auto it = kKeyAliases.find(key);
            if (it != kKeyAliases.end()) key = it->second;
        }
        // These two don't map onto an existing key 1:1 (string enum ->
        // bool / int), so they're handled directly instead of aliased.
        if (key == "ColorCodes") { s.colors_palette = trim_ws(unquote(value)) == "palette"; continue; }   // the marker: colour numbers are palette numbers
        if (key == "VolumeGradient") { s.volume_gradient = parse_bool(value); continue; }
        if (key == "VolumeIndicator") {   // "#", "⠶", "⣿" or "⡾⣽"; anything else keeps the default
            const int st = volume_style_from_name(trim_ws(unquote(value)));
            if (st >= 0) s.volume_indicator = st;
            continue;
        }
        if (key == "WaveformStyle") { s.waveform_smooth = (value == "smooth"); continue; }
        if (key == "BlackBackground") {
            std::string v = trim_ws(unquote(value));
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s.black_background = !(v == "false" || v == "0" || v == "off" || v == "no");
            continue;
        }
        if (key == "ShareProfiles") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.share_profiles = (v == "true" || v == "1" || v == "on" || v == "yes");
            continue;
        }
        if (key == "BookmarkAsk") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.book_ask_bookmark = !(v == "false" || v == "0" || v == "off" || v == "no");
            continue;
        }
        if (key == "TerminalDefaultFont") { s.terminal_default_font = parse_bool(value); continue; }
        if (key == "TerminalFont") { const std::string v = trim_ws(unquote(value)); if (!v.empty()) s.terminal_font = v; continue; }
        if (key == "AudioDevice") { s.audio_device = trim_ws(unquote(value)); continue; }
        if (key == "BookSpeed") {   // 0.5 .. 3.0, rounded to the 0.05 steps of the setting
            try {
                const double v = std::stod(trim_ws(unquote(value)));
                s.book_speed = std::clamp(std::round(v / Settings::kBookSpeedStep) * Settings::kBookSpeedStep, 0.5, 3.0);
            } catch (...) {}
            continue;
        }
        if (key == "BookCoverWave") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.book_cover_wave = !(v == "false" || v == "0" || v == "off" || v == "no");
            continue;
        }
        if (key == "LyricsNameVariants") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.lyrics_name_variants = !(v == "false" || v == "0" || v == "off" || v == "no");
            set_lyrics_name_variants(s.lyrics_name_variants);
            continue;
        }
        if (key == "LatinMacrons" || key == "LatinWo") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const bool on = (v == "true" || v == "1" || v == "on" || v == "yes");
            (key == "LatinMacrons" ? s.latin_macrons : s.latin_wo) = on;
            translit_set_style(s.latin_macrons, s.latin_wo);
            continue;
        }
        if (key == "LatinNameLookup") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.latin_lookup = (v == "true" || v == "1" || v == "on" || v == "yes");
            continue;
        }
        if (key == "LatinHanziDefault") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.latin_hanzi = (v == "chinese" || v == "pinyin") ? 1 : (v == "japanese" || v == "kanji") ? 2 : 0;
            translit_set_default_hanzi(s.latin_hanzi);
            continue;
        }
        if (key == "LatinLetters") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.latin_letters = (v == "true" || v == "1" || v == "on" || v == "yes");
            set_translit_enabled(s.latin_letters);
            continue;
        }
        if (key == "AlbumCover") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.album_cover = (v == "true" || v == "1" || v == "on" || v == "yes");
            continue;
        }
        if (key == "CoverMode") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (v == "1" || v == "cover" || v == "just cover") s.cover_mode = 0;
            else if (v == "2" || v == "gradient" || v == "cover with gradient") s.cover_mode = 1;
            else if (v == "3" || v == "disk" || v == "behind" || v == "cover behind disk") s.cover_mode = 2;
            else if (v == "4" || v == "disk2" || v == "v2" || v == "cover behind disk v2") s.cover_mode = 3;
            else if (v == "6" || v == "ascii" || v == "cover ascii") s.cover_mode = 5;
            else if (v == "7" || v == "ascii gradient" || v == "ascii2" || v == "cover ascii with gradient") s.cover_mode = 6;
            else if (v == "5" || v == "full" || v == "full anima" || v == "cover behind disk (full anima)") s.cover_mode = 4;
            continue;
        }
        if (key == "CoverBrightness" || key == "CoverColors") {
            std::string v = trim_ws(unquote(value));
            if (!v.empty() && v.back() == '%') v.pop_back();
            (key == "CoverBrightness" ? s.cover_brightness : s.cover_colors) = std::clamp((std::atoi(v.c_str()) + 2) / 5 * 5, 0, 100);
            continue;
        }
        if (key == "CoverContrast") {
            std::string v = trim_ws(unquote(value));
            if (!v.empty() && v.back() == '%') v.pop_back();
            s.cover_contrast = std::clamp((std::atoi(v.c_str()) + 2) / 5 * 5, 0, 100);
            continue;
        }
        if (key == "CoverSearch") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (v == "default") s.cover_search = 0;
            else if (v == "folder first") s.cover_search = 1;
            else if (v == "file first" || v == "within file first") s.cover_search = 2;
            else if (v == "folder only" || v == "image in folder only") s.cover_search = 3;
            else if (v == "file only" || v == "within file only") s.cover_search = 4;
            continue;
        }
        if (key == "BookCover") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.book_cover = (v == "true" || v == "1" || v == "on" || v == "yes");
            continue;
        }
        if (key == "UpdateCheck") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.update_check = !(v == "false" || v == "0" || v == "off" || v == "no");
            continue;
        }
        if (key == "TrackBufferMB") {
            try { s.track_buffer_mb = std::clamp(std::stoi(trim_ws(unquote(value))), Settings::kTrackBufferMinMB, Settings::kTrackBufferMaxMB); } catch (...) {}
            continue;
        }
        if (key == "TerminalCols" || key == "TerminalRows") {
            try { (key == "TerminalCols" ? s.terminal_cols : s.terminal_rows) = std::max(0, std::min(2000, std::stoi(trim_ws(unquote(value))))); } catch (...) {}
            continue;
        }
        if (key == "FullScreen" || key == "UseTerminalSettings") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            (key == "FullScreen" ? s.full_screen : s.use_terminal_settings) = (v == "true" || v == "1" || v == "on" || v == "yes");
            continue;
        }
        if (key == "RateCap96") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.rate_cap_96 = !(v == "false" || v == "0" || v == "off" || v == "no");
            continue;
        }
        if (key == "MediaKeys") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.media_keys = !(v == "false" || v == "0" || v == "off" || v == "no");
            continue;
        }
        if (key == "RemoteApp") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.remote_app = (v == "true" || v == "1" || v == "on" || v == "yes");
            continue;
        }
        if (key == "RemoteAppPort") { const int pt = std::atoi(trim_ws(unquote(value)).c_str()); if (pt >= 1024 && pt <= 65535) s.remote_port = pt; continue; }
        if (key == "RemoteAppPin") { s.remote_pin = trim_ws(unquote(value)); continue; }
        if (key == "StartMode") {
            std::string v = trim_ws(unquote(value));
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s.start_mode = (v == "radio") ? "radio" : (v == "audiobook") ? "audiobook" : "player";
            continue;
        }
        if (key == "BookAnimation") continue;   // an older file: the animations are braille only now
        if (key == "UseModePlayer") { s.use_mode[0] = parse_bool(value); continue; }
        if (key == "UseModeRadio") { s.use_mode[1] = parse_bool(value); continue; }
        if (key == "UseModeAudiobook") { s.use_mode[2] = parse_bool(value); continue; }
        if (key == "ModeOrder") { parse_mode_order(value, s.mode_order); continue; }
        if (key == "AudioBookPath") {
            std::string path = trim_ws(unquote(value));
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path = std::string(home) + path.substr(1);
            }
            s.audiobook_path = path;
            continue;
        }
        if (key == "PlaybackMode") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "loop") s.play_mode = 1;
            else if (v == "shuffle") s.play_mode = 2;
            else if (v == "stop") s.play_mode = 3;
            else if (v == "queue then stop" || v == "queue stop" || v == "repeat queue") s.play_mode = 4; // "repeat queue" = what older versions called it
            else s.play_mode = 0; // "list" or anything unrecognized
            continue;
        }

        // --- Element toggles ---
        if (key == "Eliment_disk" || key == "Element_disk") { s.element_disk = parse_bool(value); continue; }
        if (key == "Element_dummy_buttons") { s.element_dummy_buttons = parse_bool(value); continue; }
        if (key == "Eliment_queue" || key == "Element_queue") { s.element_queue = parse_bool(value); continue; }
        if (key == "Eliment_waveform_progress_bar" || key == "Element_waveform") { s.element_waveform = parse_bool(value); continue; }
        if (key == "Eliment_lyrics" || key == "Element_lyrics") { s.element_lyrics = parse_bool(value); continue; }
        if (key == "Eliment_vizfield") { s.element_vizfield = parse_bool(value); continue; }
        if (key == "video_mode") { s.video_mode = parse_bool(value); continue; }
        if (key == "video_view") { s.video_view = parse_bool(value); continue; }
        if (key == "video_lyrics") { s.video_lyrics = parse_bool(value); continue; }
        if (key == "video_auto") { s.video_auto = parse_bool(value); continue; }
        if (key == "video_cover") { std::string v = ascii_lower_str(trim_ws(unquote(value))); s.video_cover = (v == "snapshot" || v == "thumbnail" || v == "1" || v == "true") ? 1 : (v == "disk" || v == "2") ? 2 : 0; continue; }
        if (key == "video_size") { std::string v = ascii_lower_str(trim_ws(unquote(value))); s.video_size = (v == "cropped" || v == "crop" || v == "1") ? 1 : (v == "full" || v == "2") ? 2 : (v == "framed" || v == "3") ? 3 : 0; continue; }
        if (key == "video_contrast") { s.video_contrast = std::clamp((std::atoi(trim_ws(unquote(value)).c_str()) + 2) / 5 * 5, 0, 100); continue; }
        if (key == "video_colors") { s.video_colors = std::clamp((std::atoi(trim_ws(unquote(value)).c_str()) + 2) / 5 * 5, 0, 100); continue; }
        if (key == "video_mp3") { s.video_mp3 = parse_bool(value) || ascii_lower_str(trim_ws(unquote(value))) == "on"; continue; }
        if (key == "video_round") { s.video_round = parse_bool(value) || ascii_lower_str(trim_ws(unquote(value))) == "on"; continue; }
        if (key == "video_chroma") { s.video_chroma = chroma_from(ascii_lower_str(trim_ws(unquote(value)))); continue; }
        if (key == "cover_chroma") { s.cover_chroma = chroma_from(ascii_lower_str(trim_ws(unquote(value)))); continue; }
        if (key == "video_resolution") { s.video_res = video_res_from(ascii_lower_str(trim_ws(unquote(value)))); continue; }
        if (key == "video_buffer") { int mb = std::atoi(trim_ws(unquote(value)).c_str()); int best = 32; for (int c : kVideoBufferChoices) if (mb >= c) best = c; if (mb < kVideoBufferChoices[0]) best = kVideoBufferChoices[0]; s.video_buffer = best; continue; }
        if (key == "video_fx") { std::string v = ascii_lower_str(trim_ws(unquote(value))); s.video_fx = video_fx_from(v); continue; }
        if (key == "video_full_lyrics") { s.video_full_lyrics = parse_bool(value); continue; }
        if (key == "VideoFullTranslation") { s.video_full_translation = parse_bool(value); continue; }
        if (key == "VideoLayout") { try { s.video_layout = std::clamp(std::stoi(value), 0, 3); } catch (...) {} continue; }
        if (key == "VideoLayoutOsciStyle" || key == "VideoLayoutSpectroStyle" || key == "VideoLayoutOsciMode" || key == "VideoLayoutSpectroView") {
            const std::string v = ascii_lower_str(trim_ws(unquote(value)));
            const int n = (v == "braille" || v == "xy" || v == "spectrogram" || v == "0") ? 0 : (v == "image" || v == "yt" || v == "waterfall" || v == "1") ? 1 : -1;
            (key == "VideoLayoutOsciStyle" ? s.vl_osci_style : key == "VideoLayoutSpectroStyle" ? s.vl_spec_style : key == "VideoLayoutOsciMode" ? s.vl_osci_mode : s.vl_spec_view) = n;
            continue;
        }
        if (key == "VideoLayoutScales") { s.vl_scales = parse_bool(value); continue; }
        if (key == "video_down_height") { try { s.video_down_height = std::clamp(std::stoi(value), 144, 2160); } catch (...) {} continue; }
        if (key == "UseLyrics") { s.use_lyrics = parse_bool(value); continue; }
        if (key == "UseOscilloscope") { s.use_osci = parse_bool(value); continue; }
        if (key == "UseSpectrogram") { s.use_spectro = parse_bool(value); continue; }
        if (key == "UseSphere") { s.use_sphere = parse_bool(value); continue; }
        if (key == "VizOrder") {
            // "sphere,osci,spectro,lyrics" in any order; names that are missing are appended in the default order, so
            // a short or damaged list never loses a view.
            std::vector<int> order;
            std::string item;
            std::string v = trim_ws(unquote(value)) + ",";
            for (char ch : v) {
                if (ch != ',' && ch != ' ' && ch != '>' && ch != ';') { item += static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); continue; }
                if (item.empty()) continue;
                int id = (item == "sphere") ? 0 : (item == "osci" || item == "oscilloscope") ? 1
                       : (item == "spectro" || item == "spectrogram") ? 2 : (item == "lyrics") ? 3 : (item == "video") ? 4 : -1;
                if (id >= 0 && std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
                item.clear();
            }
            // an older list without "video": the video view comes right after the lyrics, where it always was
            if (std::find(order.begin(), order.end(), 4) == order.end()) {
                auto it = std::find(order.begin(), order.end(), 3);
                order.insert(it == order.end() ? order.end() : it + 1, 4);
            }
            for (int id = 0; id < 5; ++id) if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
            for (int i = 0; i < 5; ++i) s.viz_order[i] = order[static_cast<size_t>(i)];
            continue;
        }
        if (key == "Eliment_lyrics_placeholder_ball" || key == "Element_lyrics_placeholder_ball") {
            // Legacy "Lyric Ball" on/off switch, now a two-way pick
            // (LyricViz=sphere|osci) with no "off" state: true keeps the
            // ball, false -- "not the ball" -- picks the oscilloscope, so
            // an old config never gets the ball it had turned off.
            s.lyric_viz = parse_bool(value) ? 0 : 1;
            continue;
        }
        if (key == "lyric_viz") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "osci" || v == "oscilloscope") s.lyric_viz = 1;
            else if (v == "sphere") s.lyric_viz = 0;
            else if (v == "spectro" || v == "spectrogram") s.lyric_viz = 2;
            continue; // anything unrecognized keeps the current value
        }
        if (key == "viz_default") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "sphere") s.viz_default = 0;
            else if (v == "osci" || v == "oscilloscope") s.viz_default = 1;
            else if (v == "spectro" || v == "spectrogram") s.viz_default = 2;
            else if (v == "lyrics") s.viz_default = 3;
            else if (v == "video") s.viz_default = 5;
            else if (v == "last" || v == "last used" || v == "last_used" || v == "lastused") s.viz_default = 4;
            have_viz_default = true;
            continue;
        }
        if (key == "nolyrics_viz") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "osci" || v == "oscilloscope") s.nolyrics_viz = 1;
            else if (v == "sphere") s.nolyrics_viz = 0;
            else if (v == "spectro" || v == "spectrogram") s.nolyrics_viz = 2;
            else if (v == "none" || v == "off" || v == "nothing") s.nolyrics_viz = 3;
            continue;
        }
        if (key == "novideo_viz") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "osci" || v == "oscilloscope") s.novideo_viz = 1;
            else if (v == "sphere") s.novideo_viz = 0;
            else if (v == "spectro" || v == "spectrogram") s.novideo_viz = 2;
            else if (v == "none" || v == "off" || v == "nothing") s.novideo_viz = 3;
            else if (v == "lyrics") s.novideo_viz = 4;
            continue;
        }
        if (key == "osci_stereo") continue; // retired setting: old configs may still contain it, it is ignored
        if (key == "lyrics_alignment") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "left") s.lyrics_alignment = 1;
            else if (v == "right") s.lyrics_alignment = 2;
            else s.lyrics_alignment = 0; // "center" or anything unrecognized
            continue;
        }
        if (key == "lyrics_animation") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "word_by_word" || v == "word by word") s.lyrics_animation = 1;
            else if (v == "letter_by_letter" || v == "letter by letter") s.lyrics_animation = 2;
            else if (v == "only_active_line" || v == "active line only") s.lyrics_animation = 3;
            else if (v == "only_active_word" || v == "active word only") s.lyrics_animation = 4;
            else if (v == "line_by_line" || v == "line by line") s.lyrics_animation = 5;
            else s.lyrics_animation = 0; // "full" or anything unrecognized
            continue;
        }
        if (key == "Eliment_visualizer" || key == "Element_visualizer") { s.element_visualizer = parse_bool(value); continue; }
        if (key == "meta_only") { s.meta_only = parse_bool(value); continue; }

        // --- Border characters ---
        if (key == "upper_left_corner") { s.box_upper_left = unquote(value); continue; }
        if (key == "upper_right_corner") { s.box_upper_right = unquote(value); continue; }
        if (key == "bottom_left_corner") { s.box_lower_left = unquote(value); continue; }
        if (key == "lower_right_corner") { s.box_lower_right = unquote(value); continue; }
        if (key == "vertical") { s.box_vertical = unquote(value); continue; }
        if (key == "horizontal") { s.box_horizontal = unquote(value); continue; }
        if (key == "seprator" || key == "separator") { s.meta_separator = unquote(value); continue; }
        if (key == "list_separator") { s.list_separator = unquote(value); continue; }

        // --- Visualizer tuning ---
        if (key == "viz_style") { try { s.viz_style = std::stoi(value); } catch (...) {} continue; }
        if (key == "viz_bands") { try { s.viz_bands = std::clamp(std::stoi(value), 8, 64); } catch (...) {} continue; }
        if (key == "viz_density") { try { s.viz_density = std::clamp(std::stoi(value), 1, 5); } catch (...) {} continue; }

        // --- Colors (ANSI 256,brightness format) ---
        auto set_color = [&](const std::string& k, const std::string& v, std::string& field) {
            if (key == k) { field = parse_ansi256_brightness(unquote(v)); return true; }
            return false;
        };
        if (set_color("border", value, s.border_color)) continue;
        if (set_color("title", value, s.meta_val_color)) continue;
        if (set_color("meta_key", value, s.meta_key_color)) continue;
        if (set_color("meta_val", value, s.meta_val_color)) continue;
        if (set_color("header", value, s.header_color)) continue;
        if (set_color("progress", value, s.progress_played_color)) continue;
        if (set_color("progress_bg", value, s.progress_remaining_color)) continue;
        if (set_color("status", value, s.status_color)) continue;
        if (set_color("playlist", value, s.list_color)) continue;
        if (set_color("pl_active_fg", value, s.list_cursor_color)) continue;
        if (set_color("pl_active_bg", value, s.pl_active_bg_color)) continue;
        if (set_color("viz", value, s.visualizer_color)) continue;
        if (set_color("viz_left", value, s.viz_left_color)) continue;
        if (set_color("viz_center", value, s.viz_center_color)) continue;
        if (set_color("viz_right", value, s.viz_right_color)) continue;
        if (set_color("lyr_inactive", value, s.lyr_inactive_color)) continue;
        if (set_color("lyr_active_line", value, s.lyr_active_line_color)) continue;
        if (set_color("lyr_active_word", value, s.lyr_active_word_color)) continue;

        // --- Extended color fields (direct name=value for compatibility) ---
        if (key == "disk_color") { if (!value.empty()) s.disk_color = normalize_color_value(value); continue; }
        if (key == "disk_color_end") { if (!value.empty()) s.disk_color_end = normalize_color_value(value); continue; }
        if (key == "disk_color_gradient") { s.disk_color_gradient = parse_bool(value); continue; }
        if (key == "disk_rotation_speed") { try { s.disk_rotation_speed = std::clamp(std::stod(value), 0.01, 1.00); } catch (...) {} continue; }
        if (key == "border_color") { if (!value.empty()) s.border_color = normalize_color_value(value); continue; }
        if (key == "border_color_bottom") { if (!value.empty()) s.border_color_bottom = normalize_color_value(value); continue; }
        if (key == "header_color") { if (!value.empty()) s.header_color = normalize_color_value(value); continue; }
        if (key == "tab_current_color") { if (!value.empty()) s.tab_current_color = normalize_color_value(value); continue; }
        if (key == "tab_other_color") { if (!value.empty()) s.tab_other_color = normalize_color_value(value); continue; }
        if (key == "legend_color") { if (!value.empty()) s.legend_color = normalize_color_value(value); continue; }
        if (key == "legend_cmd_color") { if (!value.empty()) s.legend_cmd_color = normalize_color_value(value); continue; }
        if (key == "focus_color") { if (!value.empty()) { s.focus_color = normalize_color_value(value); focus_color_set = true; } continue; }
        if (key == "change_text_color") { if (!value.empty()) s.change_text_color = normalize_color_value(value); continue; }
        if (key == "change_bg_color") { if (!value.empty()) s.change_bg_color = normalize_color_value(value); continue; }
        if (key == "active_line_color") { if (!value.empty()) s.active_line_color = normalize_color_value(value); continue; }
        if (key == "active_line_bg_color") { if (!value.empty()) s.active_line_bg_color = normalize_color_value(value); continue; }
        if (key == "active_word_color") { if (!value.empty()) s.active_word_color = normalize_color_value(value); continue; }
        if (key == "BorderGradient" || key == "border_gradient") { { std::string v = value; for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); s.border_smooth = (v != "rough"); } continue; }
        if (key == "active_word_bg_color") { if (!value.empty()) s.active_word_bg_color = normalize_color_value(value); continue; }
        if (key == "inactive_line_color") { if (!value.empty()) s.inactive_line_color = normalize_color_value(value); continue; }
        if (key == "inactive_line_bg_color") { if (!value.empty()) s.inactive_line_bg_color = normalize_color_value(value); continue; }
        if (key == "progress_remaining_color") { if (!value.empty()) s.progress_remaining_color = normalize_color_value(value); continue; }
        if (key == "progress_played_color") { if (!value.empty()) s.progress_played_color = normalize_color_value(value); continue; }
        if (key == "progress_timestamp_color") { if (!value.empty()) s.progress_timestamp_color = normalize_color_value(value); continue; }
        if (key == "meta_key_color") { if (!value.empty()) s.meta_key_color = normalize_color_value(value); continue; }
        if (key == "meta_val_color") { if (!value.empty()) s.meta_val_color = normalize_color_value(value); continue; }
        if (key == "search_text_color") { if (!value.empty()) s.search_text_color = normalize_color_value(value); continue; }
        if (key == "search_bg_color") { s.search_bg_color = value.empty() ? std::string() : normalize_color_value(value); continue; }   // empty = no colour (a theme may have none)
        if (key == "search_inactive_text_color") { if (!value.empty()) s.search_inactive_text_color = normalize_color_value(value); continue; }
        if (key == "search_inactive_bg_color") { if (!value.empty()) s.search_inactive_bg_color = normalize_color_value(value); continue; }
        if (key == "list_color") { if (!value.empty()) s.list_color = normalize_color_value(value); continue; }
        if (key == "list_inactive_bg_color") { if (!value.empty()) s.list_inactive_bg_color = normalize_color_value(value); continue; }
        if (key == "list_playing_color") { if (!value.empty()) s.list_playing_color = normalize_color_value(value); continue; }
        if (key == "list_cursor_color") { if (!value.empty()) s.list_cursor_color = normalize_color_value(value); continue; }
        if (key == "list_playing_bg_color") { s.list_playing_bg_color = value.empty() ? std::string() : normalize_color_value(value); continue; }   // empty = no colour (a theme may have none)
        if (key == "list_cursor_bg_color") { if (!value.empty()) s.list_cursor_bg_color = normalize_color_value(value); continue; }
        if (key == "queue_color") { if (!value.empty()) s.queue_color = normalize_color_value(value); continue; }
        if (key == "queue_inactive_bg_color") { if (!value.empty()) s.queue_inactive_bg_color = normalize_color_value(value); continue; }
        if (key == "queue_playing_color") { if (!value.empty()) s.queue_playing_color = normalize_color_value(value); continue; }
        if (key == "queue_playing_bg_color") { s.queue_playing_bg_color = value.empty() ? std::string() : normalize_color_value(value); continue; }   // empty = no colour (a theme may have none)
        if (key == "queue_cursor_color") { if (!value.empty()) s.queue_cursor_color = normalize_color_value(value); continue; }
        if (key == "queue_cursor_bg_color") { if (!value.empty()) s.queue_cursor_bg_color = normalize_color_value(value); continue; }
        if (key == "button_color") { if (!value.empty()) s.button_color = normalize_color_value(value); continue; }
        if (key == "visualizer_color") { if (!value.empty()) s.visualizer_color = normalize_color_value(value); continue; }
        if (key == "visualizer_color_end") { if (!value.empty()) s.visualizer_color_end = normalize_color_value(value); continue; }
        if (key == "visualizer_fluidity") { try { s.visualizer_fluidity = std::clamp(std::stoi(value), 1, 10); } catch (...) {} continue; }
        if (key == "visualizer_degradation_speed") { try { s.visualizer_degradation_speed = std::clamp(std::stoi(value), 1, 10); } catch (...) {} continue; }
        if (key == "bars_style") { const std::string v = trim_ws(unquote(value)); s.bars_style = (v == "hifi" || v == "hi-fi" || v == "1") ? 1 : 0; continue; }
        if (key == "hifi_resolution") { const std::string v = trim_ws(unquote(value)); s.hifi_resolution = (v == "rough" || v == "1") ? 1 : 0; continue; }
        if (key == "hifi_channels") { const std::string v = trim_ws(unquote(value)); s.hifi_channels = (v == "mono" || v == "1") ? 1 : 0; continue; }
        if (key == "hifi_scale") { const std::string v = trim_ws(unquote(value)); s.hifi_scale = (v == "full" || v == "0") ? 0 : (v == "low" || v == "2") ? 2 : (v == "very low" || v == "3") ? 3 : 1; continue; }
        if (key == "hifi_fscale") { const std::string v = trim_ws(unquote(value)); s.hifi_fscale = v == "linear" ? 0 : (v == "logarithmic" || v == "log") ? 1 : v == "bark" ? 3 : v == "erb" ? 4 : 2; continue; }
        if (key == "hifi_gain") { try { s.hifi_gain = std::clamp(std::stoi(trim_ws(unquote(value))), -20, 20); } catch (...) {} continue; }
        if (key == "hifi_range") { try { s.hifi_range = std::clamp(std::stoi(trim_ws(unquote(value))), 40, 100); } catch (...) {} continue; }
        if (key == "hifi_fluidity") { try { s.hifi_fluidity = std::clamp(std::stoi(value), 1, 10); } catch (...) {} continue; }
        if (key == "hifi_degradation") { try { s.hifi_degradation = std::clamp(std::stoi(value), 1, 10); } catch (...) {} continue; }
        if (key == "hifi_viscosity") { try { s.hifi_viscosity = std::clamp(std::stoi(value), 0, 10); } catch (...) {} continue; }
        if (key == "hifi_adapt") { const std::string v = trim_ws(unquote(value)); s.hifi_adapt = (v == "on" || v == "1") ? 1 : 0; continue; }
        if (key == "hifi_split") { const std::string v = trim_ws(unquote(value)); s.hifi_split = (v == "remove" || v == "0") ? 0 : 1; continue; }
        if (key == "hifi_color") { const std::string v = trim_ws(unquote(value)); s.hifi_color = (v == "corresponding" || v == "1") ? 1 : 0; continue; }
        if (key == "visualizer_viscosity") { try { s.visualizer_viscosity = std::clamp(std::stoi(value), 0, 10); } catch (...) {} continue; }
        if (key == "theme_name") { s.theme_name = value; continue; }
        if (key == "play_mode") { try { s.play_mode = std::clamp(std::stoi(value), 0, 4); } catch (...) {} continue; }
        if (key == "waveform_smooth") { s.waveform_smooth = parse_bool(value); continue; }

        // --- Console logging -------------------------------------------
        if (key == "ConsoleVerbosity") {
            std::string v = unquote(value);
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s.console_verbosity = (v == "verbose") ? 1 : 0;
            continue;
        }

        // --- Emoji handling ---------------------------------------------
        if (key == "ReplaceEmoji") { std::string lv = value; for (char& ch : lv) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); s.emoji_mode = lv == "safe" ? 1 : (parse_bool(value) ? 2 : 0); continue; }
        if (key == "MarqueeScrolling") { s.marquee = parse_bool(value); continue; }

        // --- Stereo ------------------------------------------------------
        if (key == "StereoPlayback") { s.stereo = parse_bool(value); continue; }

        // --- Loudness normalisation -------------------------------------
        if (key == "NormalizeVolume") { s.normalize = parse_bool(value); continue; }
        if (key == "NormalizeTargetLufs") { try { s.normalize_target_lufs = std::clamp(std::stod(value), -40.0, 0.0); } catch (...) {} continue; }
        if (key == "NormalizeMaxBoostDb") { try { s.normalize_max_boost_db = std::clamp(std::stod(value), 0.0, 24.0); } catch (...) {} continue; }

        // --- Equaliser ---------------------------------------------------
        if (key == "EqualizerEnabled") { s.eq_enabled = parse_bool(value); continue; }
        if (key == "EqualizerBands") {
            // "0,3,-2,..." -- ten comma-separated dB values. Anything short or
            // malformed is ignored as a whole rather than half-applied.
            EqGains g{};
            size_t pos = 0; int n = 0; bool ok = true;
            while (pos <= value.size() && n < kEqBands) {
                size_t comma = value.find(',', pos);
                std::string tok = value.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                try { g[n++] = std::clamp(std::stof(tok), kEqMinDb, kEqMaxDb); } catch (...) { ok = false; break; }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (ok && n == kEqBands) s.eq_gains = g;
            continue;
        }
        if (key == "EqualizerPreset") {
            // "<ten comma-separated gains>|<name>" -- one line per custom
            // preset. The gains come first so the name may contain any
            // character (a '|' included). A malformed line, an empty name,
            // a name that a built-in preset already uses, a repeated name
            // and anything past the limit are skipped, never half-applied.
            const size_t bar = value.find('|');
            if (bar == std::string::npos) continue;
            const std::string gains_part = value.substr(0, bar);
            std::string name = trim_ws(value.substr(bar + 1));
            if (name.empty() || eq_name_reserved(name)) continue;
            if (s.eq_custom_presets.size() >= kEqMaxCustomPresets) continue;
            bool dup = false;
            for (const auto& cp : s.eq_custom_presets) if (eq_name_equal(cp.name, name)) { dup = true; break; }
            if (dup) continue;
            EqCustomPreset cp;
            size_t pos = 0; int n = 0; bool ok = true;
            while (pos <= gains_part.size() && n < kEqBands) {
                size_t comma = gains_part.find(',', pos);
                std::string tok = gains_part.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                try { cp.gains[n++] = std::clamp(std::stof(tok), kEqMinDb, kEqMaxDb); } catch (...) { ok = false; break; }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (ok && n == kEqBands) {
                cp.name = std::move(name);
                s.eq_custom_presets.push_back(std::move(cp));
            }
            continue;
        }

        // --- Autosave / session snapshot --------------------------------
        if (key == "MenuIndexClassic") { s.menu_index_classic = parse_bool(value); continue; }
        if (key == "SleepFade") { s.sleep_fade = parse_bool(value); continue; }
        if (key == "WizardSeen") { s.wizard_seen = parse_bool(value); continue; }
        if (key == "SidPlayLength") {
            try { s.sid_play_length = std::clamp(std::stoi(trim_ws(unquote(value))), 10, 1800); } catch (...) {}
            continue;
        }
        if (key == "KaraokeLyricsSize") {
            try { s.karaoke_lyrics_size = std::clamp(std::stoi(trim_ws(unquote(value))), 1, 5); } catch (...) {}
            continue;
        }
        if (key == "TranslateService") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (v == "google" || v == "mymemory") s.translate_service = v;   // (LibreTranslate was dropped: it needs a paid API key)
            continue;
        }
        if (key == "TranslateTo" || key == "TranslateFrom") {
            std::string v = trim_ws(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (!v.empty() && v.size() <= 8) (key == "TranslateTo" ? s.translate_to : s.translate_from) = v;
            if (s.translate_to == "auto") s.translate_to = "en";
            continue;
        }
        if (key == "TranslateUrl" || key == "TranslateKey") continue;   // retired with LibreTranslate: old configs may still hold them
        if (key == "AutoSave") { s.autosave_enabled = parse_bool(value); continue; }
        if (key == "AutoSaveIndicator") { s.autosave_indicator = parse_bool(value); continue; }
        if (key == "AutoSaveDelayInSec") { try { s.autosave_delay_sec = std::max(1, std::stoi(value)); } catch (...) {} continue; }
        if (key == "AutoSaveChr") {
            // Only one character allowed: keep just the first UTF-8
            // codepoint of whatever was typed there, not the first byte
            // (a multi-byte glyph like "•" would otherwise get sliced).
            std::string v = unquote(value);
            if (!v.empty()) {
                unsigned char c0 = static_cast<unsigned char>(v[0]);
                size_t len = 1;
                if ((c0 & 0x80) == 0x00) len = 1;
                else if ((c0 & 0xE0) == 0xC0) len = 2;
                else if ((c0 & 0xF0) == 0xE0) len = 3;
                else if ((c0 & 0xF8) == 0xF0) len = 4;
                s.autosave_chr = v.substr(0, std::min(len, v.size()));
            }
            continue;
        }
        if (key == "AutoSaveIndicatorType") {
            std::string v = unquote(value);
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s.autosave_indicator_type = (v == "color") ? 1 : 0; // "blink" or anything unrecognized
            continue;
        }
        if (key == "AutoSaveC1") { if (!unquote(value).empty()) s.autosave_c1 = normalize_color_value(unquote(value)); continue; }
        if (key == "AutoSaveC2") { if (!unquote(value).empty()) s.autosave_c2 = normalize_color_value(unquote(value)); continue; }

        // --- Local music library paths ---
        // Each LocalMusicPath= line appends one directory.
        // A leading ~ is expanded to $HOME so users can write:
        //   LocalMusicPath=~/Music/Rock
        if (key == "LocalMusicPath" || key == "local_music_path") {
            std::string path = trim_ws(unquote(value));
            if (!path.empty()) {
                if (path[0] == '~') {
                    const char* home = std::getenv("HOME");
                    if (home) path = std::string(home) + path.substr(1);
                }
                s.local_music_paths.push_back(path);
            }
            continue;
        }

        // --- Playlists folder override ---
        // Each PlaylistsPath= line adds one folder, independent of
        // local_music_paths[0] (the first one is where playlists get
        // saved/deleted; all of them are searched when listing/loading).
        // Same ~ expansion as LocalMusicPath, above.
        if (key == "PlaylistsPath" || key == "playlists_path") {
            std::string path = trim_ws(unquote(value));
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path = std::string(home) + path.substr(1);
            }
            if (!path.empty() && s.playlists_paths.empty()) s.playlists_paths.push_back(path);   // ONE playlist folder: further lines are ignored
            continue;
        }

        // --- Download folder (single) ---
        // Where yt-dlp writes its output: one folder, not a list. Empty
        // stays empty on purpose -- an unset DownloadFolder means "the
        // built-in cache folder", and keeping it unset (rather than
        // materialising the default into the file) is what lets a changed
        // default take effect without rewriting anyone's config.txt.
        // Same ~ expansion as LocalMusicPath, above.
        if (key == "PlaylistExportPath" || key == "playlist_export_path") {
            std::string path = trim_ws(unquote(value));
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path = std::string(home) + path.substr(1);
            }
            if (!path.empty()) s.playlist_export_path = path;
            continue;
        }
        if (key == "PlaylistImportPath" || key == "playlist_import_path") {
            std::string path = trim_ws(unquote(value));
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path = std::string(home) + path.substr(1);
            }
            if (!path.empty()) s.playlist_import_path = path;
            continue;
        }
        if (key == "ConversionExportPath" || key == "conversion_export_path") {
            std::string path = trim_ws(unquote(value));
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path = std::string(home) + path.substr(1);
            }
            if (!path.empty()) s.conversion_export_path = path;
            continue;
        }
        if (key == "HistoryPath" || key == "history_path") {
            std::string path = trim_ws(unquote(value));
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path = std::string(home) + path.substr(1);
            }
            if (!path.empty()) s.history_path = path;
            continue;
        }
        if (key == "DownloadFolder" || key == "download_folder") {
            std::string path = trim_ws(unquote(value));
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path = std::string(home) + path.substr(1);
            }
            if (!path.empty()) s.download_folder = path;
            continue;
        }

        // --- Hotkeys ---
        if (key.substr(0, 4) == "HKEY" || key.substr(0, 4) == "KHEY" || key.substr(0, 4) == "HKey") {
            s.hotkeys[key] = unquote(value);
            continue;
        }
    }

    if (!focus_color_set) s.focus_color = s.header_color;
    spectro_profiles_finish(s);
    s.mode_normalize();
    // The field starts with the default view. A config from before VizDefault existed starts with the view it was left on.
    if (!have_viz_default) s.viz_default = 4;
    if (s.viz_default <= 3) {   // 4 = last used: the view the "." key left (ElimentLyrics / LyricViz) stays
        s.element_lyrics = s.viz_default == 3;
        if (s.viz_default < 3) s.lyric_viz = s.viz_default;
        s.video_view = false;
    } else if (s.viz_default == 5) {   // video: the lyrics view with the picture (needs the Video mode, else it is just the lyrics)
        s.element_lyrics = true;
        s.video_view = true;
    }
    if (!s.video_mode) s.video_view = false;
    if (!s.element_lyrics) s.video_view = false;
    return s;
}

// =====================================================================
// Legacy settings.txt loader (for migration)
// =====================================================================

static Settings load_from_legacy(const fs::path& path) {
    Settings s;
    std::ifstream in(path);
    if (!in.is_open()) return s;

    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trim_ws(line.substr(0, eq))] = trim_ws(line.substr(eq + 1));
    }

    auto get = [&](const char* key, std::string& field) { if (kv.count(key)) field = kv[key]; };
    auto get_int = [&](const char* key, int& field) { if (kv.count(key)) { try { field = std::stoi(kv[key]); } catch (...) {} } };
    auto get_dbl = [&](const char* key, double& field) { if (kv.count(key)) { try { field = std::stod(kv[key]); } catch (...) {} } };

    get_int("visualizer_fluidity", s.visualizer_fluidity);
    get("disk_color", s.disk_color);
    get("disk_color_end", s.disk_color_end);
    get_dbl("disk_rotation_speed", s.disk_rotation_speed);
    get("border_color", s.border_color);
    get("active_line_color", s.active_line_color);
    get("active_word_color", s.active_word_color);
    get("inactive_line_color", s.inactive_line_color);
    get("progress_remaining_color", s.progress_remaining_color);
    get("progress_played_color", s.progress_played_color);
    get("list_color", s.list_color);
    get("list_playing_color", s.list_playing_color);
    get("list_cursor_color", s.list_cursor_color);
    get("queue_color", s.queue_color);
    get("queue_playing_color", s.queue_playing_color);
    get("button_color", s.button_color);
    get("visualizer_color", s.visualizer_color);
    get("visualizer_color_end", s.visualizer_color_end);
    get("theme_name", s.theme_name);
    get_int("play_mode", s.play_mode);
    if (kv.count("queue_visible")) s.element_queue = (kv["queue_visible"] == "1" || kv["queue_visible"] == "true");
    if (kv.count("waveform_smooth")) s.waveform_smooth = (kv["waveform_smooth"] == "1" || kv["waveform_smooth"] == "true");

    s.visualizer_fluidity = std::clamp(s.visualizer_fluidity, 1, 10);
    s.play_mode = std::clamp(s.play_mode, 0, 4);
    return s;
}

// =====================================================================
// Public load/save
// =====================================================================

std::string read_start_mode() {
    std::ifstream in(config_path());
    std::string line, mode = "player";
    while (std::getline(in, line)) {
        if (line.compare(0, 10, "StartMode=") != 0) continue;
        std::string v = trim_ws(unquote(line.substr(10)));
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        mode = (v == "radio") ? "radio" : (v == "audiobook") ? "audiobook" : "player";
    }
    // a mode that is switched off cannot be the start mode: the first one that is on takes over
    const ModeConfig mc = read_mode_config();
    const int m = mode_index(mode);
    if (m >= 0 && !mc.use[m]) for (int i = 0; i < 3; ++i) if (mc.use[mc.order[i]]) { mode = kModeNames[mc.order[i]]; break; }
    return mode;
}

bool read_black_background() {
    std::ifstream in(config_path());
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 16, "BlackBackground=") != 0) continue;
        std::string v = trim_ws(line.substr(16));
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return !(v == "false" || v == "0" || v == "off" || v == "no");
    }
    return true;
}

RemoteAppConfig read_remote_app() {
    RemoteAppConfig rc;
    std::ifstream in(config_path());
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string k = trim_ws(line.substr(0, eq));
        std::string v = trim_ws(unquote(line.substr(eq + 1)));
        if (k == "RemoteApp") {
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            rc.enabled = v == "true" || v == "1" || v == "on" || v == "yes";
        } else if (k == "RemoteAppPort") {
            const int pt = std::atoi(v.c_str());
            if (pt >= 1024 && pt <= 65535) rc.port = pt;
        } else if (k == "RemoteAppPin") rc.pin = v;
    }
    return rc;
}

bool write_remote_app(RemoteAppConfig& rc) {
    if (rc.enabled && rc.pin.empty()) {
        std::random_device rd;
        std::uniform_int_distribution<int> d(0, 999999);
        char b[8];
        std::snprintf(b, sizeof b, "%06d", d(rd));
        rc.pin = b;
    }
    const fs::path p = config_path();
    std::vector<std::string> lines;
    {
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    const std::pair<std::string, std::string> want[3] = {{"RemoteApp", rc.enabled ? "true" : "false"},
                                                         {"RemoteAppPort", std::to_string(rc.port)},
                                                         {"RemoteAppPin", rc.pin}};
    for (const auto& [k, v] : want) {
        bool done = false;
        for (auto& l : lines) {
            const size_t eq = l.find('=');
            if (!done && !l.empty() && l[0] != '#' && eq != std::string::npos && trim_ws(l.substr(0, eq)) == k) { l = k + "=" + v; done = true; }
        }
        if (!done) lines.push_back(k + "=" + v);
    }
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::string text;
    for (const auto& l : lines) text += l + "\n";
    return write_text_atomic(p, text);
}

bool read_share_profiles() {
    std::ifstream in(config_path());
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 14, "ShareProfiles=") != 0) continue;
        std::string v = trim_ws(unquote(line.substr(14)));
        for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return v == "true" || v == "1" || v == "on" || v == "yes";
    }
    return false;
}

bool read_update_check() {
    std::ifstream in(config_path());
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 12, "UpdateCheck=") != 0) continue;
        std::string v = trim_ws(unquote(line.substr(12)));
        for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return !(v == "false" || v == "0" || v == "off" || v == "no");
    }
    return true;
}

bool read_media_keys() {
    std::ifstream in(config_path());
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 10, "MediaKeys=") != 0) continue;
        std::string v = trim_ws(unquote(line.substr(10)));
        for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return !(v == "false" || v == "0" || v == "off" || v == "no");
    }
    return true;
}

bool read_player_viz(VizPrefs& out) {
    std::error_code ec;
    if (!fs::exists(config_path(), ec)) return false;
    const Settings s = load_settings();
    out.take_shared_from(s);   // (keeps out's own grid colour)
    return true;
}

bool write_player_viz(const VizPrefs& v) {
    std::error_code ec;
    if (!fs::exists(config_path(), ec)) return false;
    Settings s = load_settings();
    s.take_shared_from(v);     // (keeps the player's own grid colour)
    save_settings(s);
    return true;
}

bool write_latin_letters(bool on) {
    const std::string want = std::string("LatinLetters=") + (on ? "true" : "false");
    const fs::path p = config_path();
    std::error_code ec;
    if (!fs::exists(p, ec)) return false;   // the player owns config.txt: nothing to keep yet
    std::vector<std::string> lines;
    {
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    bool done = false;
    for (auto& l : lines) if (!done && l.compare(0, 13, "LatinLetters=") == 0) { l = want; done = true; }
    if (!done) lines.push_back(want);
    std::string text;
    for (const auto& l : lines) text += l + "\n";
    return write_text_atomic(p, text);
}

bool write_start_mode(const std::string& mode) {
    const std::string want = std::string("StartMode=") + (mode == "radio" ? "radio" : "player");
    const fs::path p = config_path();
    std::vector<std::string> lines;
    {
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    bool done = false;
    for (auto& l : lines) if (!done && l.compare(0, 10, "StartMode=") == 0) { l = want; done = true; }
    if (!done) lines.push_back(want);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::string text;
    for (const auto& l : lines) text += l + "\n";
    return write_text_atomic(p, text);
}

// A config.txt from before "ColorCodes=palette": 30-37 / 90-97 in text fields and 40-47 / 100-107 in background fields were plain
// 16-colour terminal codes. They become the palette numbers that look the same (black = 16, since 0 means "no colour").
// The ends of colour fades (disk, visualizer) were always palette numbers and stay.
static void migrate_legacy_colors(Settings& s) {
    using themes::ColorRole;
    for (std::string* f : {&s.border_color, &s.border_color_bottom, &s.meta_key_color, &s.meta_val_color, &s.progress_played_color,
                           &s.progress_remaining_color, &s.progress_timestamp_color, &s.search_text_color, &s.search_inactive_text_color,
                           &s.list_color, &s.list_playing_color, &s.list_cursor_color, &s.queue_color, &s.queue_playing_color,
                           &s.queue_cursor_color, &s.inactive_line_color, &s.active_line_color, &s.active_word_color, &s.header_color,
                           &s.legend_color, &s.legend_cmd_color, &s.tab_current_color, &s.tab_other_color, &s.change_text_color, &s.focus_color,
                           &s.button_color, &s.status_color, &s.lyr_inactive_color, &s.lyr_active_line_color, &s.lyr_active_word_color})
        *f = themes::legacy_to_palette(*f, ColorRole::Text);
    for (std::string* f : {&s.search_bg_color, &s.search_inactive_bg_color, &s.list_inactive_bg_color, &s.list_playing_bg_color,
                           &s.list_cursor_bg_color, &s.queue_inactive_bg_color, &s.queue_playing_bg_color, &s.queue_cursor_bg_color,
                           &s.inactive_line_bg_color, &s.active_line_bg_color, &s.active_word_bg_color, &s.change_bg_color,
                           &s.pl_active_bg_color})
        *f = themes::legacy_to_palette(*f, ColorRole::Background);
}

Settings load_settings() {
    fs::path cfg = config_path();
    std::error_code ec;

    Settings s;
    bool loaded_ok = false;
    if (fs::exists(cfg, ec)) {
        // Defensive: load_from_config() is a hand-written parser that
        // has to tolerate a human editing config.txt directly -- and a
        // hand edit can introduce anything from a stray character to a
        // whole different text encoding (Windows Notepad, in particular,
        // silently re-saves as UTF-16 unless told otherwise, which turns
        // every ASCII byte into byte+NUL and would previously make the
        // app disappear with no explanation, indistinguishable from any
        // other startup failure). Whatever load_from_config() throws (or
        // whatever we throw ourselves, below) is caught here and reported
        // instead of propagating out of App's constructor -- config.txt
        // is meant to be hand-edited, so a bad edit should fall back to
        // defaults with a visible reason, never take the whole app down.
        try {
            s = load_from_config(cfg);
            loaded_ok = true;
        } catch (const std::exception& e) {
            std::cerr << "mousiki: couldn't parse " << path_utf8(cfg) << " (" << e.what()
                      << ") -- using defaults for this session. Your file on disk is\n"
                         "unchanged; fix it and restart, or delete it to regenerate a\n"
                         "clean one.\n";
            s = Settings();
        } catch (...) {
            std::cerr << "mousiki: couldn't parse " << path_utf8(cfg)
                      << " -- using defaults for this session. Your file on disk is\n"
                         "unchanged; fix it and restart, or delete it to regenerate a\n"
                         "clean one.\n";
            s = Settings();
        }
    } else if (fs::exists(legacy_settings_path(), ec)) {
        // Migrate from legacy format
        s = load_from_legacy(legacy_settings_path());
    }
    // else: all defaults

    apply_default_hotkeys(s);
    if (!s.colors_palette) {
        // An older config.txt: convert its colours, and write the file once so that it carries the marker from now on (otherwise a
        // palette number typed by hand later would be converted again).
        const bool from_file = loaded_ok;
        migrate_legacy_colors(s);
        s.colors_palette = true;
        if (from_file) save_settings(s);
    }
    return s;
}

static void write_settings(std::ostream& out, const Settings& s) {
    out << "# Mousiki Configuration File\n";
    out << config_stamp_line() << "  (the first save by a newer version keeps the previous file as config.txt.bak)\n";
    out << "# Location: $HOME/.config/mousiki/config.txt\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##              PANEL 1: COLORS\n";
    out << "##-------------------------------------------\n\n";
    out << "# Colour numbers are numbers of the 256-colour terminal palette (1-255) in every field; 0 or empty = no colour (the terminal's own).\n";
    out << "# 16-255 look the same in every terminal; 1-15 are the terminal's 16 base colours and change with its colour scheme (10 is green in CMD, maybe yellow in yours).\n";
    out << "ColorCodes=palette\n\n";
    out << "# Frame & Controls\n";
    out << "ColorBorderTop=" << s.border_color << "\n";
    out << "ColorBorderBottom=" << s.border_color_bottom << "\n";
    out << "# BorderGradient: smooth = the frame fades line by line from the top to the bottom colour, rough = only the bottom border line has the bottom colour\n";
    out << "BorderGradient=" << (s.border_smooth ? "smooth" : "rough") << "\n";
    out << "ColorDiskTop=" << s.disk_color << "\n";
    out << "ColorDiskBottom=" << s.disk_color_end << "\n";
    out << "ColorMetadataKey=" << s.meta_key_color << "\n";
    out << "ColorMetadataVal=" << s.meta_val_color << "\n";
    out << "ColorVizLeft=" << s.visualizer_color << "\n";
    out << "ColorVizRight=" << s.visualizer_color_end << "\n";
    out << "ColorProgressBarPlayed=" << s.progress_played_color << "\n";
    out << "ColorProgressBarPending=" << s.progress_remaining_color << "\n";
    out << "\n# Section headers (the REFERENCE tab's category titles and the\n";
    out << "# LOCAL PATH / DOWNLOAD PATH / PLAYLIST PATH titles on the PATHS tab) -- 0 = no color, plain bold text\n";
    out << "ColorHeader=" << s.header_color << "\n";
    out << "\n# Key command legends (the grey hint lines such as \"[ESC] close\" in the Settings, the big\n";
    out << "# list / queue overlays, the playlist / meta editor and the history) -- 0 = terminal default\n";
    out << "ColorLegend=" << s.legend_color << "\n";
    out << "# ... and the descriptions behind those keys (\"Close\" in \"[ESC] Close\")\nColorLegendCommand=" << s.legend_cmd_color << "\n";
    out << "ColorTabCurrent=" << s.tab_current_color << "\nColorTabOther=" << s.tab_other_color << "\n";
    out << "\n# \"This field is actively being typed into\" (a PATHS row, a hex colour value, a\n";
    out << "# hotkey, a meta editor tag) -- 0 = terminal default\n";
    out << "ColorChangeFg=" << s.change_text_color << "\nColorChangeBg=" << s.change_bg_color << "\n";
    out << "\n# Pane focus indicator (the little triangle after the title of the pane that has the keys) -- 0 = plain bold\n";
    out << "ColorFocus=" << s.focus_color << "\n";
    out << "\n# Search bar: typed query (highlighted behind the query text only) and the idle bar\n# (empty FG = same as List Inactive; empty BG = none)\n";
    out << "ColorSearchFg=" << s.search_text_color << "\nColorSearchBg=" << s.search_bg_color << "\n";
    out << "ColorSearchInactiveFg=" << s.search_inactive_text_color << "\nColorSearchInactiveBg=" << s.search_inactive_bg_color << "\n";
    out << "\n# List\n";
    out << "ColorListInactiveFg=" << s.list_color << "\n";
    out << "ColorListInactiveBg=" << s.list_inactive_bg_color << "\n";
    out << "ColorListPlayingFg=" << s.list_playing_color << "\n";
    out << "ColorListPlayingBg=" << s.list_playing_bg_color << "\n";
    out << "ColorListCursorFg=" << s.list_cursor_color << "\n";
    out << "ColorListCursorBg=" << s.list_cursor_bg_color << "\n";
    out << "\n# Queue\n";
    out << "ColorQueueInactiveFg=" << s.queue_color << "\n";
    out << "ColorQueueInactiveBg=" << s.queue_inactive_bg_color << "\n";
    out << "ColorQueuePlayingFg=" << s.queue_playing_color << "\n";
    out << "ColorQueuePlayingBg=" << s.queue_playing_bg_color << "\n";
    out << "ColorQueueCursorFg=" << s.queue_cursor_color << "\n";
    out << "ColorQueueCursorBg=" << s.queue_cursor_bg_color << "\n";
    out << "\n# Lyrics\n";
    out << "ColorLyricsInactiveFg=" << s.inactive_line_color << "\n";
    out << "ColorLyricsInactiveBg=" << s.inactive_line_bg_color << "\n";
    out << "ColorLyricsActiveLineFg=" << s.active_line_color << "\n";
    out << "ColorLyricsActiveLineBg=" << s.active_line_bg_color << "\n";
    out << "ColorLyricsActiveWordFg=" << s.active_word_color << "\n";
    out << "ColorLyricsActiveWordBg=" << s.active_word_bg_color << "\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##              PANEL 2: GENERAL\n";
    out << "##-------------------------------------------\n\n";
    auto tf = [](bool v) -> const char* { return v ? "true" : "false"; };
    out << "ElimentDisk=" << tf(s.element_disk) << "\n";
    out << "LatinLetters=" << tf(s.latin_letters) << "\n## true , false -- Japanese, Korean and Chinese titles, tags and lyrics are shown in Latin letters (switch with SHIFT+' on a US keyboard, see HKeyTranslit)\n";
    out << "LyricsNameVariants=" << tf(s.lyrics_name_variants) << "\n## true , false -- a lyrics search (also the refetch) that finds nothing tries other spellings: the Latin-letters spelling, the title / artist without their Latin or without their Japanese part, the title alone\n";
    out << "LatinMacrons=" << tf(s.latin_macrons) << "\n## true , false -- Latin letters (SHIFT+'): Japanese long vowels with a macron (koohii -> koohii off, kōhī on; ou -> ō); off = spelled out (ou, uu)\n";
    out << "LatinWo=" << tf(s.latin_wo) << "\n## true , false -- Latin letters (SHIFT+'): the particle を as \"wo\" (on) or \"o\" (off)\n";
    out << "LatinNameLookup=" << tf(s.latin_lookup) << "\n## true , false -- the artists whose name is written in Chinese characters are looked up on MusicBrainz in the background to get their Latin name (sends the NAME of the artist, one at a time, to musicbrainz.org; the answers are kept in kanji_readings_learned.txt; needs Python 3)\n";
    out << "LatinHanziDefault=" << (s.latin_hanzi == 1 ? "chinese" : s.latin_hanzi == 2 ? "japanese" : "leave") << "\n## leave , chinese , japanese -- Latin letters (SHIFT+'): Chinese characters of which nothing says whether the text is Japanese or Chinese (the same in both writings, e.g. 東京): leave them, read them as Chinese (pinyin), or as Japanese (they stay as they are unless the optional kanji reader, see the manual, knows their reading)\n";
    out << "AlbumCover=" << tf(s.album_cover) << "\n## true , false -- music player: the cover of the playing track takes the disk's place (needs ElimentDisk=true; where a track has no cover the disk is shown)\n";
    out << "CoverMode=" << (s.cover_mode == 0 ? "cover" : s.cover_mode == 1 ? "gradient" : s.cover_mode == 3 ? "disk2" : s.cover_mode == 4 ? "full" : s.cover_mode == 5 ? "ascii" : s.cover_mode == 6 ? "ascii2" : "disk") << "\n## cover (just the cover) , gradient (the cover in the disk's gradient colours, as a wave moving diagonally from the upper left to the lower right) , disk (the cover behind the disk: the disk's dots are pinholes that let the full cover shine through; the dots stay on their grid, only the pattern turns) , disk2 (the same, but the dots themselves turn) , full (the disk's full animation, the dots that come and go as it turns) , ascii (the cover as braille dots in its own colours, also where the terminal could show a picture) , ascii2 (the same in the disk's gradient)\n";
    out << "CoverContrast=" << s.cover_contrast << "\n## 0 .. 100 (percent, steps of 5) -- more contrast for the album cover (0 = the cover as it is)\n";
    out << "CoverBrightness=" << s.cover_brightness << "\n## 0 .. 100 (percent, steps of 5) -- the cover behind the disk: how bright its dots are lifted (0 = the picture's own range; more = brighter, like the plain disk)\n";
    out << "CoverColors=" << s.cover_colors << "\n## 0 .. 100 (percent, steps of 5) -- the cover behind the disk: its own colours mixed into the disk's gradient (0 = gradient only, 100 = the cover's own colours)\n";
    out << "CoverChroma=" << chroma_name(s.cover_chroma) << "\n## chromatic aberration of a cover that shows its own colours (Just Cover, Cover Colors above 0, the ASCII looks): off | subtle | medium | strong\n";
    out << "CoverSearch=" << (s.cover_search == 1 ? "folder first" : s.cover_search == 2 ? "file first" : s.cover_search == 3 ? "folder only" : s.cover_search == 4 ? "file only" : "default")
        << "\n## default (the picture inside the file, else the closest-named picture in the folder) , folder first , file first (inside the file, else only a closely named folder picture) , folder only , file only -- every search except file only ends with any picture of the folder (the first by name) when nothing else gave one\n";
    out << "ElimentDummyButtons=" << tf(s.element_dummy_buttons) << "\n";
    out << "ElimentQueue=" << tf(s.element_queue) << "\n";
    out << "ElimentWaveForm=" << tf(s.element_waveform) << "\n";
    out << "VolumeIndicator=" << kVolIndicatorNames[std::clamp(s.volume_indicator, 0, kVolStyleCount - 1)] << "\n## # , \xE2\xA0\xB6 , \xE2\xA3\xBF , \xE2\xA1\xBE\xE2\xA3\xBD , disk -- what the VOLUME BAR is drawn with (\xE2\xA1\xBE\xE2\xA3\xBD is a fixed row that does not repeat; disk follows the braille effect of the disk)\n";
    out << "VolumeGradient=" << tf(s.volume_gradient) << "\n## true = the filled part of the VOLUME BAR is coloured with the VIZ gradient from left to right (like the oscilloscope), false = one colour\n";
    out << "VizLyricsField=" << tf(s.element_vizfield) << "\n";
    out << "## false = no Viz/Lyrics field right of the meta data at all (the meta data take its width; no lyrics are fetched for it)\n";
    out << "ElimentLyrics=" << tf(s.element_lyrics) << "\n";
    out << "## true = the field shows the lyrics (needs UseLyrics=true), false = it shows the visual of LyricViz. The \".\" key switches it.\n";
    out << "LyricViz=" << (s.lyric_viz == 1 ? "osci" : s.lyric_viz == 2 ? "spectro" : "sphere") << "\n";
    out << "## sphere = the audio-reactive ball | osci = the oscilloscope (both drawn in the VIZ colors) | spectro = the spectrogram\n";
    out << "## (LyricViz and ElimentLyrics are where the \".\" key left the field; the next start begins with VizDefault, see below; VizDefault=last keeps them.)\n";
    out << "VizDefault=" << viz_default_name(s.viz_default) << "\n";
    out << "## The view the field starts with when Mousiki opens: sphere | osci | spectro | lyrics | last (= the view it was left on, \"last used\" in Settings -> ANIMA/VIZ -> Lyrics/Viz Default)\n";
    out << "VideoLyrics=" << tf(s.video_lyrics) << "\n";
    out << "## true = the current lyric line is shown under the video's picture (the & overlay: Include Lyrics)\n";
    out << "## true = the saved translation of the current lyric line is shown under the video (the second reserved row, when the line does not need it)\n";
    out << "VideoAuto=" << tf(s.video_auto) << "\n";
    out << "## true = a track that has a video switches the field to the video view, a track without one to the Lyrics/Viz Default view\n";
    out << "VideoCover=" << (s.video_cover == 1 ? "thumbnail" : s.video_cover == 2 ? "disk" : "cover") << "\n";
    out << "## cover = the disk shows the track's cover; thumbnail = a frame of the track's video (the cover, then the disk, when it has no video); disk = a track with a video shows the plain disk (no cover, no thumbnail)\n";
    out << "VideoSize=" << (s.video_size == 1 ? "cropped" : s.video_size == 2 ? "full" : s.video_size == 3 ? "framed" : "regular") << "\n";
    out << "## the video's picture in the small field: regular = the whole picture | cropped = two columns cut off each side | full = fills the field (the oscilloscope's space), cropped to fit, the lyric line as a small black box over it\n";
    out << "VideoFx=" << video_fx_key(s.video_fx) << "\n";
    out << "## a retro effect on the video's picture (small field and full screen): off | scanlines | crt | vhs | glitch | pixel | gradient | quiet glitch\n";
    out << "VideoContrast=" << s.video_contrast << "\n## 0 .. 100 (percent, steps of 5) -- more contrast for the video's picture (0 = as it is)\n";
    out << "VideoColors=" << s.video_colors << "\n## 0 .. 100 (percent, steps of 5) -- the video's colours (0 = greyscale, 100 = as they are)\n";
    out << "VideoRound=" << tf(s.video_round) << "\n## Rounded Corners: the corners of the video's picture are cut round like the corners of the UI (a track can have its own: Style Option): true | false\n";
    out << "VideoChroma=" << chroma_name(s.video_chroma) << "\n## chromatic aberration of the video's picture (red and blue pushed apart, more towards the corners; works with every effect): off | subtle | medium | strong\n";
    out << "VideoResolution=" << video_res_name(s.video_res) << "\n";
    out << "## the sharpness of the video's picture: standard (up to 640 px wide, Sixel 480; the soft look) | high (1280 px, Sixel 720; Kitty in true colour) | full (1920 px, Sixel 960; Kitty in true colour). Wider pictures need more of the terminal and the processor\n";
    out << "VideoBuffer=" << s.video_buffer << "\n";
    out << "## megabytes of decoded video frames kept ahead of the picture: 8 | 16 | 32 | 64 | 128 | 256 (a wider picture holds fewer seconds in it)\n";
    out << "VideoMp3=" << tf(s.video_mp3) << "\n## yt-dlp mp3 from mp4: SEARCH YT also saves the sound of a downloaded video as an mp3 song in the download folder when no song is the target (CTRL+o in SEARCH YT changes it per download): true | false\n";
    out << "VideoFullLyrics=" << tf(s.video_full_lyrics) << "\n";
    out << "## true = the full screen video (HKeyVideoFull, \"v\") shows the current lyric line in a small black box over the picture\n";
    out << "VideoFullTranslation=" << (s.video_full_translation ? "true" : "false") << "\n";
out << "## Full screen video: show the saved translation of the lyrics under the lyric line (TAB cycles lyrics off / on / with translation).\n";
out << "VideoLayout=" << s.video_layout << "\n";
    out << "## the layout of the full screen video (SHIFT+UP / SHIFT+DOWN there): 0 video | 1 video + oscilloscope | 2 video + spectrogram | 3 video + oscilloscope + spectrogram\n";
    out << "VideoLayoutOsciStyle=" << (s.vl_osci_style < 0 ? "default" : s.vl_osci_style == 1 ? "image" : "braille") << "\n";
    out << "VideoLayoutOsciMode=" << (s.vl_osci_mode < 0 ? "default" : s.vl_osci_mode == 1 ? "yt" : "xy") << "\n";
    out << "VideoLayoutSpectroStyle=" << (s.vl_spec_style < 0 ? "default" : s.vl_spec_style == 1 ? "image" : "braille") << "\n";
    out << "VideoLayoutSpectroView=" << (s.vl_spec_view < 0 ? "default" : s.vl_spec_view == 1 ? "waterfall" : "spectrogram") << "\n";
    out << "## the oscilloscope's and the spectrogram's own picture style (braille | image) and version (XY | YT, spectrogram | waterfall) in those layouts; default = as in their options (SHIFT+o / SHIFT+i)\n";
    out << "VideoLayoutScales=" << tf(s.vl_scales) << "\n";
    out << "## the grid of the oscilloscope and the scales of the spectrogram in those layouts (SHIFT+g there)\n";
    out << "VideoDownHeight=" << s.video_down_height << "\n";
    out << "## the highest picture height of a video that Search YT downloads (into <download folder>/video_down)\n";
    out << "VideoMode=" << tf(s.video_mode) << "\n";
    out << "## true = video files (mp4, mkv, mov, avi, webm ...) are listed too and the \"video\" view joins the \".\" cycle after the lyrics: it plays the picture of the track's video in the field with the current lyric line under it. HKeyVideo (\"&\" = SHIFT+6 on a German keyboard, SHIFT+y elsewhere) switches it.\n";
    out << "VideoView=" << tf(s.video_view) << "\n";
    out << "## true = the field is on the video view right now (VizDefault=last keeps it; VizDefault=video starts with it)\n";
    out << "NoVideoViz=" << (s.novideo_viz == 0 ? "sphere" : s.novideo_viz == 1 ? "osci" : s.novideo_viz == 2 ? "spectro" : s.novideo_viz == 3 ? "none" : "lyrics") << "\n";
    out << "## what the video view shows while the track has no video: lyrics, sphere, osci, spectro or none\n";
    out << "NoLyricsViz=" << (s.nolyrics_viz == 1 ? "osci" : s.nolyrics_viz == 2 ? "spectro" : s.nolyrics_viz == 3 ? "none" : "sphere") << "\n";
    out << "## What the field shows while the lyrics view is on but no lyrics are available (sphere | osci | spectro | none).\n";
    out << "## Picking one turns its \"Use ...\" switch below on; switching that off again moves this pick to the next visual that is on.\n";
    out << "UseLyrics=" << tf(s.use_lyrics) << "\n";
    out << "UseOscilloscope=" << tf(s.use_osci) << "\n";
    out << "UseSpectrogram=" << tf(s.use_spectro) << "\n";
    out << "UseSphere=" << tf(s.use_sphere) << "\n";
    out << "## false = left out of the lyrics area: not in the \".\" cycle. The windows\n";
    out << "## (SHIFT+8 spectrogram, SHIFT+9 oscilloscope) and the full screen (SHIFT+u) work either way.\n";
    {
        static const char* const viz_names[5] = {"sphere", "osci", "spectro", "lyrics", "video"};
        out << "VizOrder=";
        for (int i = 0; i < 5; ++i) out << (i ? "," : "") << viz_names[std::clamp(s.viz_order[i], 0, 4)];
        out << "\n## the order of the views when you press \".\" (sphere, osci, spectro, lyrics, video in any order); set it on Settings -> ANIMA/VIZ -> Viz Order 1..5\n";
    }
    osci_config_write(out, s, "##");
    spectro_config_write(out, s.spectro, "##");
    spectro_profiles_write(out, s, "##");
    out << "## Tune all of these live with SHIFT+o in the main UI\n";
    out << "Visualizer=" << tf(s.element_visualizer) << "\n";
    out << "MetaDataOnly=" << tf(s.meta_only) << "\n";
    out << "## true  = the (search-)lists show embedded metadata only -- the title tag is used\n";
    out << "##         instead of the filename (files without a title tag keep their filename)\n";
    out << "## false = lists show filename AND metadata, as before\n";
    out << "## Toggleable live with HKeyToggleMetaOnly (SHIFT+n).\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             PANEL 3: ANIMA/VIZ\n";
    out << "##-------------------------------------------\n\n";
    out << "VisualizerFluidity=" << s.visualizer_fluidity << "\n## 1 to 10\n";
    out << "WaveformStyle=" << (s.waveform_smooth ? "smooth" : "raw") << "\n## raw , smooth\n";
    out << "DiskRotationSpeed=" << s.disk_rotation_speed << "\n## 0.01x to 1.00x\n";
    out << "StartMode=" << (s.start_mode == "radio" ? "radio" : s.start_mode == "audiobook" ? "audiobook" : "player")
        << "\n## player , radio , audiobook -- the mode Mousiki starts in (SHIFT and + / SHIFT and - switch while it runs; set in Settings -> GENERAL -> MODES)\n";
    out << "UseModePlayer=" << tf(s.use_mode[0]) << "\n";
    out << "UseModeRadio=" << tf(s.use_mode[1]) << "\n";
    out << "UseModeAudiobook=" << tf(s.use_mode[2]) << "\n";
    out << "## false = the mode is left out of the mode switch (one mode always stays on); set in Settings -> GENERAL -> MODES\n";
    out << "ModeOrder=" << mode_order_text(s.mode_order) << "\n## the order SHIFT and + walks through the modes (player, radio, audiobook in any order); SHIFT and - goes back; Settings -> GENERAL -> MODES -> Mode Order 1..3\n";
    out << "BlackBackground=" << (s.black_background ? "true" : "false") << "\n## true , false -- the terminal's background is black while Mousiki runs (given back on exit; player and radio)\n";
    out << "RemoteApp=" << (s.remote_app ? "true" : "false") << "\n## true , false -- the remote app (Android app or phone browser) can control Mousiki over the home network (off by default; player and radio)\n";
    out << "RemoteAppPort=" << s.remote_port << "\n## 1024 - 65535 -- the network port of the remote app (default 4747)\n";
    out << "RemoteAppPin=" << s.remote_pin << "\n## 4 - 8 digits -- the PIN the app needs; made when the remote app is switched on (mousiki --remote-info shows it)\n";
    out << "ShareProfiles=" << (s.share_profiles ? "true" : "false") << "\n## true , false -- the radio uses this file's oscilloscope and spectrogram settings instead of its own (its own come back when this is false; player and radio)\n";
    out << "BookmarkAsk=" << (s.book_ask_bookmark ? "true" : "false") << "\n## true , false -- audio book mode: changing the chapter, the mode or quitting asks first whether to set a bookmark at the exact time (the audio is paused meanwhile)\n";
    out << "BookCoverWave=" << (s.book_cover_wave ? "true" : "false") << "\n## true , false -- with BookCover on: true = the colour wave runs over the cover (a grey cover), false = the cover is shown in its own colours (the standard picture always has the wave)\n";
    out << "TerminalDefaultFont=" << (s.terminal_default_font ? "true" : "false") << "\n## true , false -- true = Mousiki leaves the terminal's own font alone; false = Mousiki's window uses the font of TerminalFont= (Settings -> GENERAL -> TEXT AND TRANSLITERATION -> Terminal Default Font)\n";
    out << "TerminalFont=" << s.terminal_font << "\n## the font of Mousiki's window where the terminal allows it (Windows Terminal: the profile \"Mousiki\"; macOS Terminal.app, iTerm2, the old Windows console): a font installed on the computer; Cascadia Mono is the default\n";
    out << "AudioDevice=" << s.audio_device << "\n## the sound output of all modes, as the system names it (Settings -> GENERAL -> AUDIO -> Choose Audio Device); empty = the system's default device\n";
    {
        char sp[16];
        std::snprintf(sp, sizeof sp, "%.2f", s.book_speed);
        out << "BookSpeed=" << sp << "\n## 0.50 - 3.00 -- audio book mode: playback speed (steps of 0.05; the pitch stays the same). SHIFT+LEFT / SHIFT+RIGHT change it while you listen\n";
    }
    out << "BookCover=" << (s.book_cover ? "true" : "false") << "\n## true , false -- audio book mode: false = the standard picture (a woman reading), true = the cover image found in the book's folder (cover / folder / front / albumart .jpg .png ..., or the only picture there); the colour wave runs over either one\n";
    out << "UpdateCheck=" << (s.update_check ? "true" : "false") << "\n## true , false -- at start-up Mousiki looks (at most once a day, in the background) whether a newer release exists and tells you once per version; nothing is installed and nothing about you is sent (needs Python 3)\n";
    out << "TrackBufferMB=" << s.track_buffer_mb << "\n## 64 - 16384 (default 256) -- a track whose decoded audio is bigger than this many MB (about 10 MB per minute of CD-quality stereo, double at 96 kHz) is not held whole in RAM but in a moving 2-minute window; seeking inside the window is instant, a jump outside it takes a moment. A bigger value = fast seeking in longer tracks, more RAM in use\n";
    out << "TerminalCols=" << s.terminal_cols << "\nTerminalRows=" << s.terminal_rows << "\n## columns and rows (default 120 x 32; at least 100 x 28, at most what fits on the screen; 0 in either = leave the terminal as it is) -- Mousiki asks the terminal for this size when it starts and when you change it in Settings -> GENERAL -> TERMINAL -> Terminal Size. Terminals decide for themselves: xterm, GNOME Terminal, iTerm2, Terminal.app and recent versions of Windows Terminal usually obey, others (Konsole, kitty, Alacritty, tmux, tiling window managers) ignore it\n";
    out << "UseTerminalSettings=" << (s.use_terminal_settings ? "true" : "false") << "\n## true , false -- true: Mousiki leaves the terminal as it is (its own window size and full screen), whatever TerminalCols / TerminalRows / FullScreen say\n";
    out << "FullScreen=" << (s.full_screen ? "true" : "false") << "\n## true , false -- ask the terminal for full screen when Mousiki starts (overrides TerminalCols / TerminalRows while on)\n";
    out << "RateCap96=" << (s.rate_cap_96 ? "true" : "false") << "\n## true , false (default true) -- a track above 96 kHz (e.g. 176.4 / 192 kHz) is brought down to 96 kHz; false plays it at its own rate: more RAM (the decoded audio grows with the rate, see TrackBufferMB) and the audio device must accept the rate. The next track that is loaded uses it\n";
    out << "MediaKeys=" << (s.media_keys ? "true" : "false") << "\n## true , false -- the system's media controls: Windows shows Mousiki in the media pop-up and its media keys work, Linux shows it in the desktop's media widgets (MPRIS; playerctl works), macOS shows Now Playing and handles the media keys (read at the next start; player and radio)\n";
    out << "PlaybackMode=" << play_mode_name(s.play_mode) << "\n## list , loop , shuffle , stop , queue then stop\n"
        << "## shuffle never reorders the queue: n and the end of a track play its next title, only # picks a random one of it\n";
    out << "VisualizerDegradationSpeed=" << s.visualizer_degradation_speed << "\n## 1 to 10\n";
    out << "VisualizerViscosity=" << s.visualizer_viscosity << "\n## 1 to 10\n";
    out << "VisualizerStyle=" << (s.bars_style == 1 ? "hifi" : "classic") << "\n## classic , hifi -- the spectrum bars under the meta data: classic, or Hi-Fi (a braille column per frequency step, stereo, at the program's frame rate)\n";
    out << "HifiResolution=" << (s.hifi_resolution == 1 ? "rough" : "fine") << "\n## fine , rough -- Hi-Fi: a band per dot column, or per two dot columns\n";
    out << "HifiChannels=" << (s.hifi_channels == 1 ? "mono" : "stereo") << "\n## stereo , mono -- Hi-Fi: the left channel on the left and the right one on the right (bass in the middle), or one mix across the width\n";
    out << "HifiScale=" << (s.hifi_scale == 0 ? "full" : s.hifi_scale == 2 ? "low" : s.hifi_scale == 3 ? "very low" : "medium") << "\n## full , medium , low , very low -- Hi-Fi: the height of the bars (the lower, the lower the middle levels; the peaks stay visible)\n";
    out << "HifiFrequencyScale=" << (s.hifi_fscale == 0 ? "linear" : s.hifi_fscale == 1 ? "logarithmic" : s.hifi_fscale == 3 ? "bark" : s.hifi_fscale == 4 ? "erb" : "mel") << "\n## linear , logarithmic , mel , bark , erb -- Hi-Fi: the frequency scale of the bars\n";
    out << "HifiGain=" << s.hifi_gain << "\n## -20 to 20 dB -- Hi-Fi: moves the levels up or down\n";
    out << "HifiRange=" << s.hifi_range << "\n## 40 to 100 dB -- Hi-Fi: the range from the bottom to the top of the bars (smaller = steeper)\n";
    out << "HifiFluidity=" << s.hifi_fluidity << "\n## 1 to 10 -- the Hi-Fi style keeps its own bar values (VisualizerFluidity ... are the Classic ones)\n";
    out << "HifiDegradationSpeed=" << s.hifi_degradation << "\n## 1 to 10\n";
    out << "HifiViscosity=" << s.hifi_viscosity << "\n## 0 to 10\n";
    out << "HifiAdapt=" << (s.hifi_adapt == 1 ? "on" : "off") << "\n## on , off -- Hi-Fi: adapts the height to the song (dense, compressed music with a full frequency range is lowered more)\n";
    out << "HifiSplit=" << (s.hifi_split == 0 ? "remove" : "add") << "\n## add , remove -- Hi-Fi: an empty dot column between the left and the right channel\n";
    out << "HifiColor=" << (s.hifi_color == 1 ? "corresponding" : "gradient") << "\n## gradient , corresponding -- Hi-Fi: the bar colours from left to right, or the colours of the spectrogram by height\n";
    out << "LyricsAlignment=" << (s.lyrics_alignment == 1 ? "left" : s.lyrics_alignment == 2 ? "right" : "center") << "\n## center , left , right\n";
    {
        const char* anim_name = "full";
        if (s.lyrics_animation == 1) anim_name = "word by word";
        else if (s.lyrics_animation == 2) anim_name = "letter by letter";
        else if (s.lyrics_animation == 3) anim_name = "active line only";
        else if (s.lyrics_animation == 4) anim_name = "active word only";
        else if (s.lyrics_animation == 5) anim_name = "line by line";
        out << "LyricsAnimation=" << anim_name << "\n";
        out << "## full , word by word , line by line , letter by letter\n";
        out << "## active line only , active word only\n";
    }
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             EMOJI IN TITLES\n";
    out << "##-------------------------------------------\n\n";
    out << "ReplaceEmoji=" << (s.emoji_mode == 1 ? "safe" : s.emoji_mode == 2 ? "true" : "false") << "\n";
    out << "## true  = an emoji in a title is drawn as a single \"?\" so the box borders always stay aligned\n";
    out << "## false = draw the real emoji (alignment then depends on how your terminal measures emoji)\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             MARQUEE SCROLLING\n";
    out << "##-------------------------------------------\n\n";
    out << "MarqueeScrolling=" << (s.marquee ? "true" : "false") << "\n";
    out << "## true  = a title that is too long for its column scrolls (the row under the cursor, Name / Artist / Album of the\n";
    out << "##         metadata block, the playlist editor lists)\n";
    out << "## false = nothing scrolls, a too long text is cut off with \"...\"\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             STEREO\n";
    out << "##-------------------------------------------\n\n";
    out << "StereoPlayback=" << (s.stereo ? "true" : "false") << "\n";
    out << "## true  = play tracks in stereo (uses about twice the memory per loaded track)\n";
    out << "## false = mono: left and right are folded together, as in earlier versions\n";
    out << "## Turning it off takes effect immediately; turning it on applies from the next track.\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             LOUDNESS NORMALIZATION\n";
    out << "##-------------------------------------------\n\n";
    out << "NormalizeVolume=" << (s.normalize ? "true" : "false") << "\n";
    out << "NormalizeTargetLufs=" << s.normalize_target_lufs << "\n";
    out << "NormalizeMaxBoostDb=" << s.normalize_max_boost_db << "\n";
    out << "## Each track is measured (LUFS) while it decodes and played at NormalizeTargetLufs.\n";
    out << "## Quiet tracks are raised (at most NormalizeMaxBoostDb), loud/compressed ones lowered.\n";
    out << "## -14 matches YouTube/Spotify; -16 leaves more headroom. Lower number = quieter overall.\n";
    out << "## Tune all three live with SHIFT+v in the main UI (the on/off switch is also on the ANIMA/VIZ tab and the `v` key)\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             EQUALIZER (Shift+E)\n";
    out << "##-------------------------------------------\n\n";
    out << "EqualizerEnabled=" << (s.eq_enabled ? "true" : "false") << "\n";
    out << "EqualizerBands=";
    for (int b = 0; b < kEqBands; ++b) out << (b ? "," : "") << s.eq_gains[b];
    out << "\n";
    out << "## Ten gains in dB (-12 to 12) for 31, 62, 125, 250, 500 Hz, 1, 2, 4, 8, 16 kHz.\n";
    out << "## Easier to change with the overlay (Shift+E), which also has presets.\n";
    for (const auto& cp : s.eq_custom_presets) {
        out << "EqualizerPreset=";
        for (int b = 0; b < kEqBands; ++b) out << (b ? "," : "") << cp.gains[b];
        out << "|" << cp.name << "\n";
    }
    out << "## EqualizerPreset=<ten gains>|<name> is one custom preset (same band order as above), up to " << kEqMaxCustomPresets << ".\n";
    out << "## Create and delete them in the overlay: S saves the current curve under a name, DEL/X deletes the selected one.\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             CONSOLE / LOGGING\n";
    out << "##-------------------------------------------\n\n";
    out << "ConsoleVerbosity=" << (s.console_verbosity == 1 ? "verbose" : "basic") << "\n## basic , verbose\n";
    out << "## basic   = every command mousiki ran (yt-dlp/ffprobe/ffmpeg/lyrics-fetch) + its raw output\n";
    out << "## verbose = basic, plus internal/OS-level events (terminal resize, audio device init, spawn errors, ...)\n";
    out << "## Log file: $HOME/.cache/mousiki/logs/console.log -- wiped fresh at the start of every session.\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             AUTOSAVE / SESSION SNAPSHOT\n";
    out << "##-------------------------------------------\n\n";
    out << "SleepFade=" << tf(s.sleep_fade) << "\n## sleep timer: fade the volume out over the last 10 % of the time (30 s .. 10 min), then pause\n";
    out << "MenuIndexClassic=" << tf(s.menu_index_classic) << "\n## the \"<\" menus overlay: false = the tree (organigram), true = the classic view with every category as a box (SHIFT+t switches, the last view is kept)\n";
    out << "WizardSeen=" << tf(s.wizard_seen) << "\n## the help wizard (SHIFT+w) opened itself once already; false makes it greet you again on the next start\n";
    out << "SidPlayLength=" << s.sid_play_length << "\n## SID tunes (C64) are rendered by sidplayfp for this many seconds (10 .. 1800)\n";
    out << "KaraokeLyricsSize=" << s.karaoke_lyrics_size << "\n## karaoke overlay (k): lyrics size 1 (normal text) .. 5, SHIFT and + / SHIFT and - change it there\n";
    out << "TranslateService=" << s.translate_service << "\n## READ LYRICS (CTRL+L) translation service, picked with [t] there: google | mymemory\n";
    out << "TranslateTo=" << s.translate_to << "\n## language the lyrics are translated INTO (a code such as en, de, fr, es ...), picked with [t] in READ LYRICS\n";
    out << "TranslateFrom=" << s.translate_from << "\n## language OF the lyrics: auto = the service detects it, or a code such as de, ja ... (picked with [t])\n";
    out << "AutoSave=" << tf(s.autosave_enabled) << "\n## resume exact song/position/queue/repeat/shuffle next launch\n";
    out << "AutoSaveIndicator=" << tf(s.autosave_indicator) << "\n";
    out << "AutoSaveDelayInSec=" << s.autosave_delay_sec << "\n";
    out << "AutoSaveChr=" << s.autosave_chr << "\n## exactly one character is used, even if you paste more\n";
    out << "AutoSaveIndicatorType=" << (s.autosave_indicator_type == 1 ? "color" : "blink") << "\n## blink , color\n";
    out << "AutoSaveC1=" << s.autosave_c1 << "\n";
    out << "AutoSaveC2=" << s.autosave_c2 << "\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             PANEL 4: REFERENCE\n";
    out << "##-------------------------------------------\n\n";
    out << "font_en={\n";
    for (char c = 'A'; c <= 'Z'; ++c) {
        auto it = s.font_map.find(c);
        std::string upper(1, c), lower(1, static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (it != s.font_map.end()) { upper = it->second.first; lower = it->second.second; }
        out << "          " << c << "={" << upper << "," << lower << "},\n";
    }
    out << "        };\n\n";
    out << "UpperLeftCorner=\"" << s.box_upper_left << "\";\n";
    out << "UpperRightCorner=\"" << s.box_upper_right << "\";\n";
    out << "BottomLeftCorner=\"" << s.box_lower_left << "\";\n";
    out << "LowerRightCorner=\"" << s.box_lower_right << "\";\n";
    out << "Vertical=\"" << s.box_vertical << "\";\n";
    out << "Horizontal=\"" << s.box_horizontal << "\";\n";
    out << "Seprator=\"" << s.meta_separator << "\"\n";
    out << "ListSeparator=\"" << s.list_separator << "\"\n";
    out << "\n# GENERAL\n";
    {
        std::string setting_key = s.hotkeys.count("HKeySetting") ? s.hotkeys.at("HKeySetting") : "s";
        out << "HKeySetting=\"" << setting_key << "\"\n";
    }
    out << "\n# Local Music Paths\n";
    for (const auto& path : s.local_music_paths) {
        // Blank entries are skipped rather than written out as an empty
        // "LocalMusicPath=" line: an abandoned "+ new path" edit in the
        // GENERAL tab can leave one behind in memory, and the loader skips
        // empty values anyway, so writing it would only litter the file.
        if (path.empty()) continue;
        out << "LocalMusicPath=" << path << "\n";
    }
    out << "\n# Playlists folder(s) (optional -- overrides the LocalMusicPath[0]/playlists default).\n";
    out << "# One line per folder: playlists are listed/loaded from all of them, while saving\n";
    out << "# and deleting uses the first one.\n";
    for (const auto& path : s.playlists_paths) {
        if (!path.empty()) out << "PlaylistsPath=" << path << "\n";
    }
    if (!s.playlist_export_path.empty()) {
        out << "\n# Where playlists are exported as M3U8 / M3U (one folder; unset means the \"export\" folder inside the playlist folder).\n";
        out << "PlaylistExportPath=" << s.playlist_export_path << "\n";
    }
    if (!s.playlist_import_path.empty()) {
        out << "\n# Where playlists are imported from (M3U8 / M3U; one folder; unset means the \"import\" folder inside the playlist folder).\n";
        out << "# The files stay; a copy of each list lands in the imported_lists folder of the playlist folder.\n";
        out << "PlaylistImportPath=" << s.playlist_import_path << "\n";
    }
    if (!s.conversion_export_path.empty()) {
        out << "\n# Where the file conversion overlay (SHIFT+c) writes converted files (one folder; unset means the\n";
        out << "# \"conversion-export\" folder inside the download folder).\n";
        out << "ConversionExportPath=" << s.conversion_export_path << "\n";
    }
    if (!s.audiobook_path.empty()) {
        out << "\n# The folder of the audio books (one folder; every folder below it with audio files is one book).\n";
        out << "# Mousiki keeps a .cue file beside each book that records where you last stopped listening.\n";
        out << "AudioBookPath=" << s.audiobook_path << "\n";
    }
    if (!s.history_path.empty()) {
        out << "\n# Folder of the listening history (history.json; one folder, unset means ~/.cache/mousiki/history).\n";
        out << "HistoryPath=" << s.history_path << "\n";
    }
    if (!s.download_folder.empty()) {
        out << "\n# Where yt-dlp downloads go (one folder; unset means ~/.cache/mousiki).\n";
        out << "# The folder below is also added to the local music paths automatically.\n";
        out << "DownloadFolder=" << s.download_folder << "\n";
    }

    out << "\n# Navigation\n";
    // Write every mapped hotkey, stable order, whatever the key is named.
    static const char* hkey_order[] = {
        "HKeyNavigateUp", "HKeyNavigateDown", "HKeyPlay", "HKeyPlayNextSong", "HKeyPlayPreviousSong",
        "HKeyTogglePlayPause", "HKeyCyclePlayMode", "HKeySearch",
        "HKeySeekForward", "HKeySeekBackward", "HKeyIncreaseVolume", "HKeyDecreaseVolume",
        "HKeyAddHoveringSongToQueue", "HKeyRemoveHoveringSongFromQueue", "HKeySwitchBetweenCards",
        "HKeyFilterForFolder", "HKeyClearFilter", "HKeyQuit", "HKeyDownloadStream",
        "HKeyToggleNormalize", "HKeyToggleMetaOnly", "HKeyMetaEditor", "HKeyHistory", "HKeyEqualizer",
    };
    for (const char* name : hkey_order) {
        auto it = s.hotkeys.find(name);
        if (it != s.hotkeys.end()) out << name << "=\"" << it->second << "\"\n";
    }
    for (const auto& [k, v] : s.hotkeys) {
        if (k == "HKeySetting") continue;
        bool found = false;
        for (const char* name : hkey_order) { if (k == name) { found = true; break; } }
        if (!found) out << k << "=\"" << v << "\"\n";
    }
    out << "\n";
}

std::string settings_to_text(const Settings& s) {
    std::ostringstream out;
    write_settings(out, s);
    return out.str();
}

std::string config_stamp_line() { return std::string("# Written by Mousiki v") + MOUSIKI_VERSION; }

std::string config_written_by(const fs::path& p) {
    std::ifstream in(p);
    std::string line;
    const std::string tag = "# Written by Mousiki v";
    for (int i = 0; i < 12 && std::getline(in, line); ++i) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.compare(0, tag.size(), tag) == 0) {
            std::string v = line.substr(tag.size());
            return v.substr(0, v.find_first_of(" \t"));   // the version, without the note behind it
        }
    }
    return std::string();
}

bool backup_config_if_older(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec) || fs::file_size(p, ec) == 0) return false;
    if (config_written_by(p) == MOUSIKI_VERSION) return false;      // written by this version already: nothing to keep
    fs::path bak = p;
    bak += ".bak";
    fs::copy_file(p, bak, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

// ---- one look per mode (see settings.h) ----
void copy_look(const Settings& src, Settings& dst) {
    static_cast<VizPrefs&>(dst) = static_cast<const VizPrefs&>(src);   // oscilloscope / spectrogram
#define MOUSIKI_LOOK(f) dst.f = src.f
    // colours
    MOUSIKI_LOOK(theme_name); MOUSIKI_LOOK(border_color); MOUSIKI_LOOK(border_color_bottom); MOUSIKI_LOOK(border_smooth);
    MOUSIKI_LOOK(disk_color); MOUSIKI_LOOK(disk_color_end); MOUSIKI_LOOK(disk_color_gradient);
    MOUSIKI_LOOK(active_line_color); MOUSIKI_LOOK(active_line_bg_color); MOUSIKI_LOOK(active_word_color); MOUSIKI_LOOK(active_word_bg_color);
    MOUSIKI_LOOK(inactive_line_color); MOUSIKI_LOOK(inactive_line_bg_color);
    MOUSIKI_LOOK(progress_remaining_color); MOUSIKI_LOOK(progress_played_color); MOUSIKI_LOOK(progress_timestamp_color);
    MOUSIKI_LOOK(search_text_color); MOUSIKI_LOOK(search_bg_color); MOUSIKI_LOOK(search_inactive_text_color); MOUSIKI_LOOK(search_inactive_bg_color);
    MOUSIKI_LOOK(list_color); MOUSIKI_LOOK(list_inactive_bg_color); MOUSIKI_LOOK(list_playing_color); MOUSIKI_LOOK(list_cursor_color);
    MOUSIKI_LOOK(list_playing_bg_color); MOUSIKI_LOOK(list_cursor_bg_color);
    MOUSIKI_LOOK(queue_color); MOUSIKI_LOOK(queue_inactive_bg_color); MOUSIKI_LOOK(queue_playing_color); MOUSIKI_LOOK(queue_playing_bg_color);
    MOUSIKI_LOOK(queue_cursor_color); MOUSIKI_LOOK(queue_cursor_bg_color);
    MOUSIKI_LOOK(button_color); MOUSIKI_LOOK(header_color); MOUSIKI_LOOK(legend_color); MOUSIKI_LOOK(legend_cmd_color);
    MOUSIKI_LOOK(tab_current_color); MOUSIKI_LOOK(tab_other_color); MOUSIKI_LOOK(change_text_color); MOUSIKI_LOOK(change_bg_color);
    MOUSIKI_LOOK(focus_color); MOUSIKI_LOOK(visualizer_color); MOUSIKI_LOOK(visualizer_color_end);
    MOUSIKI_LOOK(meta_key_color); MOUSIKI_LOOK(meta_val_color); MOUSIKI_LOOK(status_color); MOUSIKI_LOOK(pl_active_bg_color);
    MOUSIKI_LOOK(viz_left_color); MOUSIKI_LOOK(viz_center_color); MOUSIKI_LOOK(viz_right_color);
    MOUSIKI_LOOK(lyr_inactive_color); MOUSIKI_LOOK(lyr_active_line_color); MOUSIKI_LOOK(lyr_active_word_color);
    // elements and visualizations
    MOUSIKI_LOOK(element_disk); MOUSIKI_LOOK(element_dummy_buttons); MOUSIKI_LOOK(element_queue); MOUSIKI_LOOK(element_waveform);
    MOUSIKI_LOOK(volume_indicator); MOUSIKI_LOOK(volume_gradient); MOUSIKI_LOOK(element_vizfield); MOUSIKI_LOOK(element_lyrics);
    MOUSIKI_LOOK(lyric_viz); MOUSIKI_LOOK(nolyrics_viz); MOUSIKI_LOOK(use_lyrics); MOUSIKI_LOOK(use_osci); MOUSIKI_LOOK(use_spectro);
    MOUSIKI_LOOK(use_sphere); MOUSIKI_LOOK(viz_default); MOUSIKI_LOOK(element_visualizer); MOUSIKI_LOOK(waveform_smooth);
    for (int i = 0; i < 5; ++i) dst.viz_order[i] = src.viz_order[i];
    MOUSIKI_LOOK(meta_only); MOUSIKI_LOOK(visualizer_fluidity); MOUSIKI_LOOK(visualizer_degradation_speed); MOUSIKI_LOOK(visualizer_viscosity);
    MOUSIKI_LOOK(bars_style); MOUSIKI_LOOK(hifi_resolution); MOUSIKI_LOOK(hifi_channels); MOUSIKI_LOOK(hifi_color); MOUSIKI_LOOK(hifi_scale); MOUSIKI_LOOK(hifi_adapt); MOUSIKI_LOOK(hifi_fluidity); MOUSIKI_LOOK(hifi_degradation); MOUSIKI_LOOK(hifi_viscosity); MOUSIKI_LOOK(hifi_fscale); MOUSIKI_LOOK(hifi_gain); MOUSIKI_LOOK(hifi_range); MOUSIKI_LOOK(hifi_split);
    MOUSIKI_LOOK(viz_style); MOUSIKI_LOOK(viz_bands); MOUSIKI_LOOK(viz_density); MOUSIKI_LOOK(disk_rotation_speed);
    // border characters
    MOUSIKI_LOOK(box_upper_left); MOUSIKI_LOOK(box_upper_right); MOUSIKI_LOOK(box_lower_left); MOUSIKI_LOOK(box_lower_right);
    MOUSIKI_LOOK(box_vertical); MOUSIKI_LOOK(box_horizontal); MOUSIKI_LOOK(meta_separator); MOUSIKI_LOOK(list_separator);
    MOUSIKI_LOOK(play_mode);
    // the sound of the mode: a book is speech, so loudness, equaliser and stereo / mono are its own
    MOUSIKI_LOOK(normalize); MOUSIKI_LOOK(normalize_target_lufs); MOUSIKI_LOOK(normalize_max_boost_db);
    MOUSIKI_LOOK(eq_enabled); MOUSIKI_LOOK(eq_gains); MOUSIKI_LOOK(stereo);
#undef MOUSIKI_LOOK
}

void sync_shared(const Settings& from, Settings& to) {
    if (&from == &to) return;
    Settings merged = from;
    copy_look(to, merged);            // the look of `to` stays
    merged.book_profile = to.book_profile;
    to = std::move(merged);
}

fs::path book_config_path() { return config_path().parent_path() / "audiobook_config.txt"; }

static bool is_look_key(const std::string& key);   // (below)

// The look lines of a settings object as text: two profiles that give the same text look the same.
static std::string look_text(const Settings& s) {
    std::istringstream in(settings_to_text(s));
    std::string line, out;
    while (std::getline(in, line)) {
        const std::string t = trim_ws(line);
        if (t.empty() || t[0] == '#') continue;
        const size_t cut = t.find_first_of("=:");
        if (cut != std::string::npos && is_look_key(trim_ws(t.substr(0, cut)))) out += t + "\n";
    }
    return out;
}

Settings load_book_settings(const Settings& player, bool* wants_default_theme) {
    if (wants_default_theme) *wants_default_theme = false;
    Settings s = player;
    s.book_profile = true;
    s.element_disk = true;   // the picture field of the audio book mode is always there
    std::error_code ec;
    const fs::path p = book_config_path();
    if (!fs::exists(p, ec)) { if (wants_default_theme) *wants_default_theme = true; return s; }   // not saved yet: the caller gives it the default theme of the mode
    try {
        const Settings file = load_from_config(p);
        copy_look(file, s);
        s.element_disk = true;
        // A profile that is still an untouched copy of the music player's look (what the first version of the mode saved) gets the default
        // theme too; one that was changed on purpose stays as it is.
        if (wants_default_theme && look_text(s) == look_text(player)) *wants_default_theme = true;
    } catch (const std::exception& e) {
        std::cerr << "mousiki: couldn't parse " << path_utf8(p) << " (" << e.what() << ") -- the audio book mode uses the player's look this session.\n";
    } catch (...) {
        std::cerr << "mousiki: couldn't parse " << path_utf8(p) << " -- the audio book mode uses the player's look this session.\n";
    }
    return s;
}

// The keys of config.txt that belong to the look (see copy_look): only these go into audiobook_config.txt.
static bool is_look_key(const std::string& key) {
    static const char* const prefixes[] = {"Color", "BorderGradient", "Eliment", "Volume", "VizLyricsField", "LyricViz", "VizDefault", "NoLyricsViz",
                                           "UseLyrics", "UseOscilloscope", "UseSpectrogram", "UseSphere", "VizOrder", "Osci", "Scope", "Spectro",
                                           "Visualizer", "WaveformStyle", "DiskRotationSpeed", "MetaDataOnly", "PlaybackMode", "UpperLeftCorner",
                                           "UpperRightCorner", "BottomLeftCorner", "LowerRightCorner", "Vertical", "Horizontal", "Seprator", "ListSeparator",
                                           "Normalize", "EqualizerEnabled", "EqualizerBands", "StereoPlayback"};
    for (const char* p : prefixes) if (key.compare(0, std::strlen(p), p) == 0) return true;
    return false;
}

// A save in the audio book mode: the look into audiobook_config.txt, everything else into config.txt (that file keeps the
// music player's look: it is read, the shared values of `s` are put in, and it is written again).
static void save_book_profile(const Settings& s) {
    std::error_code ec;
    const fs::path bp = book_config_path();
    fs::create_directories(bp.parent_path(), ec);
    {
        std::ostringstream all;
        write_settings(all, s);
        std::ostringstream out;
        {
            out << "# Mousiki Configuration File -- AUDIO BOOK MODE\n";
            out << config_stamp_line() << "\n";
            out << "# Only the look of the audio book mode is kept here: colours, elements, visualizations, borders, play mode.\n";
            out << "# Everything else (paths, hotkeys, sound, the mode switches ...) is shared by all modes and lives in config.txt.\n";
            out << "# Same notation as config.txt: colour numbers of the 256-colour palette, 0 or empty = no colour.\n\n";
            std::istringstream in(all.str());
            std::string line;
            while (std::getline(in, line)) {
                const std::string t = trim_ws(line);
                if (t.empty() || t[0] == '#') continue;
                const size_t cut = t.find_first_of("=:");
                if (cut == std::string::npos) continue;
                if (is_look_key(trim_ws(t.substr(0, cut)))) out << line << "\n";
            }
        }
        write_text_atomic(bp, out.str());
    }
    Settings base = load_settings();     // the file as the music player left it
    Settings merged = s;
    copy_look(base, merged);             // its look stays the player's
    merged.book_profile = false;
    const fs::path p = config_path();
    backup_config_if_older(p);
    std::ostringstream out;
    write_settings(out, merged);
    write_text_atomic(p, out.str());
}

std::string config_value(const std::string& key) {
    std::ifstream in(config_path());
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim_ws(line);
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq != std::string::npos && trim_ws(t.substr(0, eq)) == key) return trim_ws(unquote(t.substr(eq + 1)));
    }
    return "";
}

bool set_config_value(const std::string& key, const std::string& value) {
    const fs::path p = config_path();
    std::vector<std::string> lines;
    {
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    bool found = false;
    for (std::string& l : lines) {
        const std::string t = trim_ws(l);
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq != std::string::npos && trim_ws(t.substr(0, eq)) == key) { l = key + "=" + value; found = true; break; }
    }
    if (!found) lines.push_back(key + "=" + value);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::string text;
    for (const std::string& l : lines) text += l + "\n";
    return write_text_atomic(p, text);
}

void save_settings(const Settings& s) {
    if (s.book_profile) { save_book_profile(s); return; }
    fs::path p = config_path();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    backup_config_if_older(p);   // the first save by a newer version keeps the old file as config.txt.bak

    std::ostringstream out;
    write_settings(out, s);
    write_text_atomic(p, out.str());
}

} // namespace muisc
