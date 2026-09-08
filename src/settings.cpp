#include "settings.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
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

// Matches to_ansi(val, is_bg) from the reference implementation exactly,
// with one deliberate exception: "0"/empty means no color at all,
// per the explicit earlier requirement that 0 = none everywhere. The
// reference code doesn't special-case 0 (it would render as 256-color
// index 0, i.e. black) -- that's the one intentional deviation here,
// kept because it was a separate, explicit instruction. Everything else
// -- including the reference's own quirk where is_bg is ignored for the
// 30-47/90-107 direct-SGR range, and no clamping of out-of-range values
// -- is matched verbatim, not "fixed".
std::string ansi_for(const std::string& color_name, bool /*bold*/) {
    if (color_name.empty()) return "";
    int v = 0;
    try { v = std::stoi(color_name); } catch (...) { return ""; }
    if (v <= 0) return "";
    if (v >= 30 && v <= 47) return "\x1b[" + std::to_string(v) + "m";
    if (v >= 90 && v <= 107) return "\x1b[" + std::to_string(v) + "m";
    return "\x1b[38;5;" + std::to_string(v) + "m";
}

std::string bg_ansi_for(const std::string& color_name) {
    if (color_name.empty()) return "";
    int v = 0;
    try { v = std::stoi(color_name); } catch (...) { return ""; }
    if (v <= 0) return "";
    if (v >= 30 && v <= 47) return "\x1b[" + std::to_string(v) + "m";
    if (v >= 90 && v <= 107) return "\x1b[" + std::to_string(v) + "m";
    return "\x1b[48;5;" + std::to_string(v) + "m";
}

std::string sgr_params_for(const std::string& color_name) {
    if (color_name.empty()) return "";
    int v = 0;
    try { v = std::stoi(color_name); } catch (...) { return ""; }
    if (v <= 0) return "";
    if ((v >= 30 && v <= 47) || (v >= 90 && v <= 107)) return std::to_string(v);
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
    // the way it used to (that required emitting a truecolor SGR string,
    // which is no longer a valid stored value) -- this just recovers the
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

std::string apply_font_map(const std::string& text,
                            const std::unordered_map<char, std::pair<std::string, std::string>>& font_map) {
    if (font_map.empty()) return text;
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
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

void apply_default_hotkeys(Settings& s) {
    if (s.hotkeys.empty()) {
        s.hotkeys = {
            {"HKeySetting",                     "s"},
            {"HKeyNavigateUp",                  "ARROW_KEY_UP"},
            {"HKeyNavigateDown",                "ARROW_KEY_DOWN"},
            {"HKeyPlay",                        "ENTER"},
            {"HKeySearch",                      "/"},
            {"HKeySearchOnline",                "/s:"},
            {"HKeyPlayNextSong",                "n"},
            {"HKeyPlayPreviousSong",            "b"},
            {"HKeySeekForward",                 "ARROW_KEY_RIGHT"},
            {"HKeySeekBackward",                "ARROW_KEY_LEFT"},
            {"HKeyIncreaseVolume",              "1"},
            {"HKeyDecreaseVolume",              "2"},
            {"HKeyAddHoveringSongToQueue",      "a"},
            {"HKeyRemoveHoveringSongFromQueue", "d"},
            {"HKeySwitchBetweenCards",          "TAB"},
            {"HKeyToggleRepeat",                "r"},
            {"HKeyTogglePlayPause",             "p"},
            {"HKeyToggleShuffle",               "m"},
            {"HKeyFilterForFolder",             "f"},
            {"HKeyClearFilter",                 "c"},
            {"HKeyQuit",                        "q"},
            {"HKeyResetPreference",             "e"},
            {"HKeyDownloadStream",              "y"},
        };
    }
}

// =====================================================================
// Config file path
// =====================================================================

fs::path config_path() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::path(".");
    return base / ".config" / "mousiki" / "config.txt";
}

// Legacy path for migration
static fs::path legacy_settings_path() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::path(".");
    return base / ".config" / "mousiki" / "settings.txt";
}

// =====================================================================
// Parsing helpers
// =====================================================================

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Strip surrounding quotes: "value" -> value
static std::string unquote(const std::string& s) {
    std::string t = trim(s);
    if (!t.empty() && t.back() == ';') {
        t.pop_back();
        t = trim(t);
    }
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        return t.substr(1, t.size() - 2);
    }
    return t;
}

static bool parse_bool(const std::string& v) {
    std::string s = trim(v);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s == "yes" || s == "true" || s == "1";
}

// Parse font_en block: font_en={ A={A,a}, B={B,b}, ... };
static void parse_font_block(std::ifstream& in, Settings& s) {
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.find("};") != std::string::npos || t == "}") break;
        // Expected: A={A,a},  or  A={𝓐,𝓪},
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string letter_str = trim(t.substr(0, eq));
        if (letter_str.empty()) continue;
        char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter_str[0])));
        // Parse {upper,lower}
        size_t ob = t.find('{', eq);
        size_t cb = t.find('}', ob != std::string::npos ? ob : 0);
        if (ob == std::string::npos || cb == std::string::npos) continue;
        std::string inner = t.substr(ob + 1, cb - ob - 1);
        size_t comma = inner.find(',');
        if (comma == std::string::npos) continue;
        std::string upper_glyph = trim(inner.substr(0, comma));
        std::string lower_glyph = trim(inner.substr(comma + 1));
        if (!upper_glyph.empty() && !lower_glyph.empty()) {
            s.font_map[letter] = {upper_glyph, lower_glyph};
        }
    }
}

static void parse_about_app_block(std::ifstream& in, Settings& s) {
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t == "};" || t == "}") break;
        s.about_app_lines.push_back(line);
    }
    // Trim trailing blank lines so the About tab doesn't end in empty rows.
    while (!s.about_app_lines.empty() && trim(s.about_app_lines.back()).empty()) s.about_app_lines.pop_back();
}

// =====================================================================
// Load settings from config.txt
// =====================================================================

static Settings load_from_config(const fs::path& path) {
    Settings s;
    std::ifstream in(path);
    if (!in.is_open()) return s;

    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        // Font block
        if (t.find("font_en") != std::string::npos && t.find('{') != std::string::npos) {
            parse_font_block(in, s);
            continue;
        }
        // About App text block
        if (t.find("ClassTextAboutApp") != std::string::npos && t.find('{') != std::string::npos) {
            parse_about_app_block(in, s);
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
            key = trim(t.substr(0, eq2));
            value = trim(t.substr(eq2 + 2));
        } else if (colon != std::string::npos && (eq1 == std::string::npos || colon < eq1)) {
            // "key : value" format
            key = trim(t.substr(0, colon));
            value = trim(t.substr(colon + 1));
        } else if (eq1 != std::string::npos) {
            // "key=value" or 'key="value"' format
            key = trim(t.substr(0, eq1));
            value = trim(t.substr(eq1 + 1));
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
            {"ColorListInactiveFg", "list_color"}, {"ColorListInactiveBg", "list_inactive_bg_color"},
            {"ColorListPlayingFg", "list_playing_color"}, {"ColorListPlayingBg", "list_playing_bg_color"},
            {"ColorListCursorFg", "list_cursor_color"}, {"ColorListCursorBg", "list_cursor_bg_color"},
            {"ColorQueueInactiveFg", "queue_color"}, {"ColorQueueInactiveBg", "queue_inactive_bg_color"},
            {"ColorQueuePlayingFg", "queue_playing_color"}, {"ColorQueuePlayingBg", "queue_playing_bg_color"},
            {"ColorQueueCursorFg", "queue_cursor_color"}, {"ColorQueueCursorBg", "queue_cursor_bg_color"},
            {"ColorLyricsInactiveFg", "inactive_line_color"}, {"ColorLyricsInactiveBg", "inactive_line_bg_color"},
            {"ColorLyricsActiveLineFg", "active_line_color"}, {"ColorLyricsActiveLineBg", "active_line_bg_color"},
            {"ColorLyricsActiveWordFg", "active_word_color"}, {"ColorLyricsActiveWordBg", "active_word_bg_color"},
            {"ElimentDisk", "Eliment_disk"}, {"ElimentDummyButtons", "Element_dummy_buttons"},
            {"ElimentQueue", "Eliment_queue"}, {"ElimentWaveForm", "Eliment_waveform_progress_bar"},
            {"ElimentLyrics", "Eliment_lyrics"}, {"LyricsPlaceholderBall", "Eliment_lyrics_placeholder_ball"},
            {"Visualizer", "Eliment_visualizer"},
            {"VisualizerFluidity", "visualizer_fluidity"}, {"DiskRotationSpeed", "disk_rotation_speed"},
            {"VisualizerDegradationSpeed", "visualizer_degradation_speed"}, {"VisualizerViscosity", "visualizer_viscosity"},
            {"LyricsAlignment", "lyrics_alignment"}, {"LyricsAnimation", "lyrics_animation"},
            {"UpperLeftCorner", "upper_left_corner"}, {"UpperRightCorner", "upper_right_corner"},
            {"BottomLeftCorner", "bottom_left_corner"}, {"LowerRightCorner", "lower_right_corner"},
            {"Vertical", "vertical"}, {"Horizontal", "horizontal"},
            {"Seprator", "seprator"}, {"ListSeparator", "list_separator"},
        };
        {
            auto it = kKeyAliases.find(key);
            if (it != kKeyAliases.end()) key = it->second;
        }
        // These two don't map onto an existing key 1:1 (string enum ->
        // bool / int), so they're handled directly instead of aliased.
        if (key == "WaveformStyle") { s.waveform_smooth = (value == "smooth"); continue; }
        if (key == "PlaybackMode") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "loop") s.play_mode = 1;
            else if (v == "shuffle") s.play_mode = 2;
            else if (v == "stop") s.play_mode = 3;
            else s.play_mode = 0; // "list" or anything unrecognized
            continue;
        }

        // --- Element toggles ---
        if (key == "Eliment_disk" || key == "Element_disk") { s.element_disk = parse_bool(value); continue; }
        if (key == "Element_dummy_buttons") { s.element_dummy_buttons = parse_bool(value); continue; }
        if (key == "Eliment_queue" || key == "Element_queue") { s.element_queue = parse_bool(value); continue; }
        if (key == "Eliment_waveform_progress_bar" || key == "Element_waveform") { s.element_waveform = parse_bool(value); continue; }
        if (key == "Eliment_lyrics" || key == "Element_lyrics") { s.element_lyrics = parse_bool(value); continue; }
        if (key == "Eliment_lyrics_placeholder_ball" || key == "Element_lyrics_placeholder_ball") { s.element_lyrics_placeholder_ball = parse_bool(value); continue; }
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
        if (key == "active_line_color") { if (!value.empty()) s.active_line_color = normalize_color_value(value); continue; }
        if (key == "active_line_bg_color") { if (!value.empty()) s.active_line_bg_color = normalize_color_value(value); continue; }
        if (key == "active_word_color") { if (!value.empty()) s.active_word_color = normalize_color_value(value); continue; }
        if (key == "active_word_bg_color") { if (!value.empty()) s.active_word_bg_color = normalize_color_value(value); continue; }
        if (key == "inactive_line_color") { if (!value.empty()) s.inactive_line_color = normalize_color_value(value); continue; }
        if (key == "inactive_line_bg_color") { if (!value.empty()) s.inactive_line_bg_color = normalize_color_value(value); continue; }
        if (key == "progress_remaining_color") { if (!value.empty()) s.progress_remaining_color = normalize_color_value(value); continue; }
        if (key == "progress_played_color") { if (!value.empty()) s.progress_played_color = normalize_color_value(value); continue; }
        if (key == "progress_timestamp_color") { if (!value.empty()) s.progress_timestamp_color = normalize_color_value(value); continue; }
        if (key == "meta_key_color") { if (!value.empty()) s.meta_key_color = normalize_color_value(value); continue; }
        if (key == "meta_val_color") { if (!value.empty()) s.meta_val_color = normalize_color_value(value); continue; }
        if (key == "list_color") { if (!value.empty()) s.list_color = normalize_color_value(value); continue; }
        if (key == "list_inactive_bg_color") { if (!value.empty()) s.list_inactive_bg_color = normalize_color_value(value); continue; }
        if (key == "list_playing_color") { if (!value.empty()) s.list_playing_color = normalize_color_value(value); continue; }
        if (key == "list_cursor_color") { if (!value.empty()) s.list_cursor_color = normalize_color_value(value); continue; }
        if (key == "list_playing_bg_color") { if (!value.empty()) s.list_playing_bg_color = normalize_color_value(value); continue; }
        if (key == "list_cursor_bg_color") { if (!value.empty()) s.list_cursor_bg_color = normalize_color_value(value); continue; }
        if (key == "queue_color") { if (!value.empty()) s.queue_color = normalize_color_value(value); continue; }
        if (key == "queue_inactive_bg_color") { if (!value.empty()) s.queue_inactive_bg_color = normalize_color_value(value); continue; }
        if (key == "queue_playing_color") { if (!value.empty()) s.queue_playing_color = normalize_color_value(value); continue; }
        if (key == "queue_playing_bg_color") { if (!value.empty()) s.queue_playing_bg_color = normalize_color_value(value); continue; }
        if (key == "queue_cursor_color") { if (!value.empty()) s.queue_cursor_color = normalize_color_value(value); continue; }
        if (key == "queue_cursor_bg_color") { if (!value.empty()) s.queue_cursor_bg_color = normalize_color_value(value); continue; }
        if (key == "button_color") { if (!value.empty()) s.button_color = normalize_color_value(value); continue; }
        if (key == "visualizer_color") { if (!value.empty()) s.visualizer_color = normalize_color_value(value); continue; }
        if (key == "visualizer_color_end") { if (!value.empty()) s.visualizer_color_end = normalize_color_value(value); continue; }
        if (key == "visualizer_fluidity") { try { s.visualizer_fluidity = std::clamp(std::stoi(value), 1, 10); } catch (...) {} continue; }
        if (key == "visualizer_degradation_speed") { try { s.visualizer_degradation_speed = std::clamp(std::stoi(value), 1, 10); } catch (...) {} continue; }
        if (key == "visualizer_viscosity") { try { s.visualizer_viscosity = std::clamp(std::stoi(value), 0, 10); } catch (...) {} continue; }
        if (key == "theme_name") { s.theme_name = value; continue; }
        if (key == "play_mode") { try { s.play_mode = std::clamp(std::stoi(value), 0, 3); } catch (...) {} continue; }
        if (key == "waveform_smooth") { s.waveform_smooth = parse_bool(value); continue; }

        // --- Local music library paths ---
        // Each LocalMusicPath= line appends one directory.
        // A leading ~ is expanded to $HOME so users can write:
        //   LocalMusicPath=~/Music/Rock
        if (key == "LocalMusicPath" || key == "local_music_path") {
            std::string path = trim(unquote(value));
            if (!path.empty()) {
                if (path[0] == '~') {
                    const char* home = std::getenv("HOME");
                    if (home) path = std::string(home) + path.substr(1);
                }
                s.local_music_paths.push_back(path);
            }
            continue;
        }

        // --- Hotkeys ---
        if (key.substr(0, 4) == "HKEY" || key.substr(0, 4) == "KHEY" || key.substr(0, 4) == "HKey") {
            s.hotkeys[key] = unquote(value);
            continue;
        }
    }

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
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
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
    s.play_mode = std::clamp(s.play_mode, 0, 3);
    return s;
}

// =====================================================================
// Public load/save
// =====================================================================

Settings load_settings() {
    fs::path cfg = config_path();
    std::error_code ec;

    Settings s;
    if (fs::exists(cfg, ec)) {
        s = load_from_config(cfg);
    } else if (fs::exists(legacy_settings_path(), ec)) {
        // Migrate from legacy format
        s = load_from_legacy(legacy_settings_path());
    }
    // else: all defaults

    apply_default_hotkeys(s);
    if (s.about_app_lines.empty()) {
        s.about_app_lines = {
            "Devloper : ender                Github   : itzender5820",
            "Email    : itz.ender5820@gmail.com",
            "Version  : orignal and final v1.0        Licence  : Apache licence 2.0",
            "",
            "Mousiki",
            "A terminal music player built for people who prefer control.",
            "Zero external UI bloat: 100% native POSIX terminal runtime.",
        };
    }
    return s;
}

void save_settings(const Settings& s) {
    fs::path p = config_path();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    std::ofstream out(p, std::ios::trunc);
    if (!out.is_open()) return;

    out << "# Mousiki Configuration File\n";
    out << "# Location: $HOME/.config/mousiki/config.txt\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##              PANEL 1: COLORS\n";
    out << "##-------------------------------------------\n\n";
    out << "# Frame & Controls\n";
    out << "ColorBorderTop=" << s.border_color << "\n";
    out << "ColorBorderBottom=" << s.border_color_bottom << "\n";
    out << "ColorDiskTop=" << s.disk_color << "\n";
    out << "ColorDiskBottom=" << s.disk_color_end << "\n";
    out << "ColorMetadataKey=" << s.meta_key_color << "\n";
    out << "ColorMetadataVal=" << s.meta_val_color << "\n";
    out << "ColorVizLeft=" << s.visualizer_color << "\n";
    out << "ColorVizRight=" << s.visualizer_color_end << "\n";
    out << "ColorProgressBarPlayed=" << s.progress_played_color << "\n";
    out << "ColorProgressBarPending=" << s.progress_remaining_color << "\n";
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
    out << "##              PANEL 2: ON/OFF\n";
    out << "##-------------------------------------------\n\n";
    auto tf = [](bool v) -> const char* { return v ? "true" : "false"; };
    out << "ElimentDisk=" << tf(s.element_disk) << "\n";
    out << "ElimentDummyButtons=" << tf(s.element_dummy_buttons) << "\n";
    out << "ElimentQueue=" << tf(s.element_queue) << "\n";
    out << "ElimentWaveForm=" << tf(s.element_waveform) << "\n";
    out << "ElimentLyrics=" << tf(s.element_lyrics) << "\n";
    out << "LyricsPlaceholderBall=" << tf(s.element_lyrics_placeholder_ball) << "\n";
    out << "Visualizer=" << tf(s.element_visualizer) << "\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             PANEL 3: ANIMATION\n";
    out << "##-------------------------------------------\n\n";
    out << "VisualizerFluidity=" << s.visualizer_fluidity << "\n## 1 to 10\n";
    out << "WaveformStyle=" << (s.waveform_smooth ? "smooth" : "raw") << "\n## raw , smooth\n";
    out << "DiskRotationSpeed=" << s.disk_rotation_speed << "\n## 0.01x to 1.00x\n";
    out << "PlaybackMode=" << (s.play_mode == 1 ? "loop" : s.play_mode == 2 ? "shuffle" : s.play_mode == 3 ? "stop" : "list") << "\n## list , loop , shuffle , stop\n";
    out << "VisualizerDegradationSpeed=" << s.visualizer_degradation_speed << "\n## 1 to 10\n";
    out << "VisualizerViscosity=" << s.visualizer_viscosity << "\n## 1 to 10\n";
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
    out << "\n# General\n";
    {
        std::string setting_key = s.hotkeys.count("HKeySetting") ? s.hotkeys.at("HKeySetting") : "s";
        out << "HKeySetting=\"" << setting_key << "\"\n";
    }
    out << "\n# Local Music Paths\n";
    for (const auto& path : s.local_music_paths) {
        out << "LocalMusicPath=" << path << "\n";
    }
    out << "\n# Navigation\n";
    // Write every mapped hotkey, stable order, whatever the key is named.
    static const char* hkey_order[] = {
        "HKeyNavigateUp", "HKeyNavigateDown", "HKeyPlay", "HKeyPlayNextSong", "HKeyPlayPreviousSong",
        "HKeyTogglePlayPause", "HKeyToggleRepeat", "HKeyToggleShuffle", "HKeySearch", "HKeySearchOnline",
        "HKeySeekForward", "HKeySeekBackward", "HKeyIncreaseVolume", "HKeyDecreaseVolume",
        "HKeyAddHoveringSongToQueue", "HKeyRemoveHoveringSongFromQueue", "HKeySwitchBetweenCards",
        "HKeyFilterForFolder", "HKeyClearFilter", "HKeyQuit", "HKeyResetPreference", "HKeyDownloadStream",
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

    out << "##-------------------------------------------\n";
    out << "##             PANEL 5: ABOUT APP\n";
    out << "##-------------------------------------------\n\n";
    out << "ClassTextAboutApp= {\n\n";
    for (const auto& l : s.about_app_lines) out << l << "\n";
    out << "\n\n};\n";
}

} // namespace muisc
