#pragma once
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// Colors: every field holds a plain decimal ANSI 256-color palette index
// as a string, "1".."255". "0" (or empty) means "no color" -- inherit
// whatever the terminal/panel would otherwise show. There is no named
// preset ("cyan", "white", etc.) anymore; type the number directly in
// the settings panel with Enter. "_end" fields, when non-empty, turn a
// solid color into a gradient between the base color and that end color.
struct Settings {
    // --- element visibility toggles (config.txt: "Eliment_*") ----------
    bool element_disk = true;
    bool element_dummy_buttons = true;
    bool element_queue = true;           // replaces the old queue_visible
    bool element_waveform = true;
    bool element_lyrics = true;
    bool element_lyrics_placeholder_ball = true;
    int lyrics_alignment = 0; // 0=center (default), 1=left, 2=right
    int lyrics_animation = 0; // 0=full (default), 1=word by word, 2=letter by letter, 3=only active line, 4=only active word
    bool element_visualizer = true;

    // --- appearance / tuning -------------------------------------------
    int visualizer_fluidity = 1;        // 1-10, higher = smoother/slower rise
    int visualizer_degradation_speed = 8; // 1 (slow fade) - 10 (near-instant), how fast bars fall after a drop
    int visualizer_viscosity = 3;       // 0 (bars move independently) - 10 (heavy neighbor blending)
    std::string disk_color = "36";
    std::string disk_color_end = "34"; // empty = solid disk_color, no gradient
    bool disk_color_gradient = true;    // true = use disk_color_end as a gradient stop, false = solid/"generic" disk_color only
    double disk_rotation_speed = 1.0;   // multiplier on the reference angular velocity, range 0.01x-1.00x

    // --- border / separator characters (from config.txt) ---------------
    std::string box_upper_left = "\u256d";   // ╭
    std::string box_upper_right = "\u256e";  // ╮
    std::string box_lower_left = "\u2570";   // ╰
    std::string box_lower_right = "\u256f";  // ╯
    std::string box_vertical = "\u2502";     // │
    std::string box_horizontal = "\u2500";   // ─
    std::string meta_separator = ":  :";     // separator between disk|meta|lyrics columns
    std::string list_separator = "|";        // separator in list panel between columns

    // --- font map (config.txt font_en block) ---------------------------
    // Maps ASCII letter → {uppercase_display, lowercase_display}.
    // Empty map = use default ASCII glyphs.
    std::unordered_map<char, std::pair<std::string, std::string>> font_map;
    // Raw lines of the config file's trailing ClassTextAboutApp={...}; block
    // -- the About App tab displays these verbatim rather than a hardcoded
    // string, so editing the config file's own about-text actually changes
    // what's shown. Falls back to a sensible default if the config has no
    // such section (e.g. a freshly-generated config.txt).
    std::vector<std::string> about_app_lines;

    // --- colors --------------------------------------------------------
    // Defaults here match the "g_defaults" fallback map from the redesigned
    // config: config.txt intentionally ships every color field blank
    // except these seven, which fall back to a real color even when the
    // config value is empty. Everything else genuinely defaults to "no
    // color" (plain terminal default) until the user sets it.
    std::string border_color = "90";
    std::string border_color_bottom = "90";   // top uses border_color; box_top()/box_bottom() use these two independently

    std::string active_line_color;
    std::string active_line_bg_color;
    std::string active_word_color;
    std::string active_word_bg_color;
    std::string inactive_line_color;
    std::string inactive_line_bg_color;

    std::string progress_remaining_color;
    std::string progress_played_color;
    std::string progress_timestamp_color;    // empty = falls back to border_color

    std::string list_color;
    std::string list_inactive_bg_color;
    std::string list_playing_color;
    std::string list_cursor_color;

    std::string queue_color;
    std::string queue_inactive_bg_color;
    std::string queue_playing_color;
    std::string queue_playing_bg_color;
    std::string queue_cursor_color;     // the "hovering" row once you Tab into the queue
    std::string queue_cursor_bg_color;             // "0"/empty = no background

    // Row background colors. Value "0" (or empty) = no background color.
    std::string list_playing_bg_color;
    std::string list_cursor_bg_color = "238"; // one of the seven config-level defaults (ColorListCursorBg)

    std::string button_color;

    std::string visualizer_color = "32";
    std::string visualizer_color_end = "33";

    // New colors from config.txt
    std::string meta_key_color;          // label text ("Name", "Artist") — empty = inherit list_color
    std::string meta_val_color;          // value text — empty = inherit list_color
    std::string status_color;            // status line — empty = default
    std::string pl_active_bg_color;      // playlist active row background — empty = none

    // Multi-stop visualizer gradient (config.txt viz_left/center/right)
    std::string viz_left_color;          // treble edge color
    std::string viz_center_color;        // bass center color
    std::string viz_right_color;         // treble edge color (right side)

    // Lyrics-specific (config.txt lyr_*)
    std::string lyr_inactive_color;      // empty = falls back to inactive_line_color
    std::string lyr_active_line_color;   // empty = falls back to active_line_color
    std::string lyr_active_word_color;   // empty = falls back to active_word_color

    // --- visualizer tuning ---------------------------------------------
    int viz_style = 0;
    int viz_bands = 32;
    int viz_density = 1;

    // --- playback ------------------------------------------------------
    bool waveform_smooth = true;
    int play_mode = 0; // 0=list 1=loop 2=shuffle 3=stop

    std::string theme_name = "default";

    // --- local music library paths -------------------------------------
    // Each entry is an absolute path scanned for audio files.
    // Configured via one or more LocalMusicPath= lines in config.txt.
    // Example:
    //   LocalMusicPath=/home/user/Music
    //   LocalMusicPath=/mnt/nas/albums
    // When empty the scanner falls back to the built-in defaults
    // (~/Music and ~/disk/Music).
    std::vector<std::string> local_music_paths;

    // --- hotkey mapping ------------------------------------------------
    // Action name → key string (e.g. "ARROW_KEY_UP", "s", "ENTER")
    std::unordered_map<std::string, std::string> hotkeys;

    // Convenience: resolve a hotkey action to the configured key code.
    // Returns the default if not mapped. Key codes returned match what
    // TerminalIO::poll_key() produces (single chars) or special names.
    std::string hotkey(const std::string& action) const {
        auto it = hotkeys.find(action);
        return it != hotkeys.end() ? it->second : "";
    }
};

const char* play_mode_name(int mode);

// Migrates a config value from the old named-preset color scheme
// ("cyan", "white", ...) to the new bare-number scheme.
std::string normalize_color_value(const std::string& value);

// ANSI SGR escape for a color value. The value must be a bare decimal
// string "0".."255" (a 256-color palette index); "0" or empty means no
// color at all (returns ""). Anything unparsable is also treated as "no
// color" rather than erroring.
std::string ansi_for(const std::string& color_name, bool bold = true);

// Background-color counterpart to ansi_for(). Same "0"/empty = no
// background rule.
std::string bg_ansi_for(const std::string& color_name);

// Just the numeric SGR parameters for a color value ("38;5;208"), with
// no leading \x1b[ or trailing m. Empty string if the value is "0"/none.
std::string sgr_params_for(const std::string& color_name);

// Rough brightness 0-100 for display next to a color in the settings panel.
int brightness_percent(const std::string& color_value);

// Best-effort RGB extraction for gradient interpolation.
bool try_parse_rgb(const std::string& color_value, int& r, int& g, int& b);

// Linearly interpolates between `start` and `end` at t in [0,1].
std::string gradient_ansi(const std::string& start, const std::string& end, float t, bool bold = true);

// Multi-stop gradient: interpolates across left → center → right using
// position t in [0,1]. Falls back to gradient_ansi(start, end, t) if
// center is empty.
std::string multi_stop_gradient_ansi(const std::string& left, const std::string& center,
                                      const std::string& right, float t);

// A horizontal strip of filled block characters, smoothly interpolated
// between start and end -- used for settings-panel preview swatches.
std::string gradient_preview_bar(const std::string& start, const std::string& end, int width);

// Parse "ANSI_index,brightness" format from config.txt (e.g. "39,100")
// into an SGR color value usable by ansi_for(). Returns the input
// unchanged if it doesn't match the format.
std::string parse_ansi256_brightness(const std::string& value);

// Apply a font map to a string: each ASCII letter is replaced with its
// mapped glyph (uppercase or lowercase variant as appropriate).
std::string apply_font_map(const std::string& text,
                            const std::unordered_map<char, std::pair<std::string, std::string>>& font_map);

// Prebaked themes.
const std::vector<std::string>& theme_names();
void apply_theme(Settings& s, const std::string& theme_name);

// Default hotkey bindings — used when config.txt doesn't specify them.
void apply_default_hotkeys(Settings& s);

// Config file path and I/O. Uses config.txt format (: and == separators).
// Falls back to reading legacy settings.txt if config.txt doesn't exist.
fs::path config_path();
Settings load_settings();
void save_settings(const Settings& s);

} // namespace muisc
