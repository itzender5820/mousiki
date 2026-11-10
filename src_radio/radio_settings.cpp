// Radio settings: reads and writes the radio's own configuration, including its colours, the remote
// app setting and the shared visualisation values.
#include "radio_settings.h"
#include "utf8_util.h"
#include "string_util.h"
#include "remote_server.h"
#include "version.h"
#include "about_text.h"
#include "keyboard_layout.h"
#include "terminal_ui.h"
#include "path_utf8.h"
#include "settings.h"   // kModeNames, ModeConfig, read_mode_config(): the mode switch is the player's config.txt
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace muisc::radio {

namespace fs = std::filesystem;

// ===========================================================================================
// COLORS tab schema
// ===========================================================================================
const ColorRowSpec kColorRows[] = {
    {"BORDER_COLOR", "TOP",          &RadioSettings::border_top,         "BOTTOM",         &RadioSettings::border_bottom},
    {"ON_AIR",       "UPPER_LEFT",   &RadioSettings::on_air_upper_left,  "BOTTOM_RIGHT",   &RadioSettings::on_air_bottom_right},
    {"METADATA",     "KEY",          &RadioSettings::meta_key,           "VAL",            &RadioSettings::meta_val},
    {"VIZ",          "LEFT",         &RadioSettings::viz_left,           "RIGHT",          &RadioSettings::viz_right},
    {"OSCI",         "LEFT",         &RadioSettings::osci_left,          "RIGHT",          &RadioSettings::osci_right},
    {"FREQUENCY_BAR","LINE",         &RadioSettings::freq_line,          "MHz",            &RadioSettings::freq_mhz},
    {"VOLUME",       "CURRENT",      &RadioSettings::volume_current,     "POSSIBLE",       &RadioSettings::volume_possible},
    // ---- divider ----
    {"SEARCH",       "FG",           &RadioSettings::search_fg,          "BG",             &RadioSettings::search_bg},
    {"",             "INACTIVE  FG", &RadioSettings::search_inactive_fg, "BG",             &RadioSettings::search_inactive_bg},
    {"LIST",         "INACTIVE  FG", &RadioSettings::list_fg,            "BG",             &RadioSettings::list_bg},
    {"",             "PLAYING   FG", &RadioSettings::list_playing_fg,    "BG",             &RadioSettings::list_playing_bg},
    {"",             "CURSOR    FG", &RadioSettings::list_cursor_fg,     "BG",             &RadioSettings::list_cursor_bg},
    {"PRESETS",      "INACTIVE  FG", &RadioSettings::preset_inactive_fg, "BG",             &RadioSettings::preset_inactive_bg},
    {"",             "KEY       FG", &RadioSettings::preset_key_fg,      nullptr,          nullptr},
    {"HEADER",       "TEXT",         &RadioSettings::header,             nullptr,          nullptr},
    {"LEGEND",       "KEY-CMD",      &RadioSettings::legend,             "COMMAND",        &RadioSettings::legend_cmd},
    {"TAB_NAMES",    "CURRENT",      &RadioSettings::tab_current,        "OTHER",          &RadioSettings::tab_other},
    {"FOCUS",        "TEXT",         &RadioSettings::focus,              nullptr,          nullptr},
};
const int kColorRowCount = static_cast<int>(sizeof(kColorRows) / sizeof(kColorRows[0]));

namespace {
struct ThemeKey { const char* key; std::string RadioSettings::* p; };
const ThemeKey kThemeKeys[] = {
    {"ColorBorderTop", &RadioSettings::border_top},       {"ColorBorderBottom", &RadioSettings::border_bottom},
    {"ColorOnAirUpperLeft", &RadioSettings::on_air_upper_left}, {"ColorOnAirBottomRight", &RadioSettings::on_air_bottom_right},
    {"ColorMetadataKey", &RadioSettings::meta_key},       {"ColorMetadataVal", &RadioSettings::meta_val},
    {"ColorVizLeft", &RadioSettings::viz_left},           {"ColorVizCenter", &RadioSettings::viz_center},
    {"ColorVizRight", &RadioSettings::viz_right},
    {"ColorOsciLeft", &RadioSettings::osci_left},         {"ColorOsciRight", &RadioSettings::osci_right},
    {"ColorFrequencyLine", &RadioSettings::freq_line},    {"ColorFrequencyMHz", &RadioSettings::freq_mhz},
    {"ColorVolumeCurrent", &RadioSettings::volume_current}, {"ColorVolumePossible", &RadioSettings::volume_possible},
    {"ColorSearchFg", &RadioSettings::search_fg},         {"ColorSearchBg", &RadioSettings::search_bg},
    {"ColorSearchInactiveFg", &RadioSettings::search_inactive_fg}, {"ColorSearchInactiveBg", &RadioSettings::search_inactive_bg},
    {"ColorListInactiveFg", &RadioSettings::list_fg},     {"ColorListInactiveBg", &RadioSettings::list_bg},
    {"ColorListPlayingFg", &RadioSettings::list_playing_fg}, {"ColorListPlayingBg", &RadioSettings::list_playing_bg},
    {"ColorListCursorFg", &RadioSettings::list_cursor_fg}, {"ColorListCursorBg", &RadioSettings::list_cursor_bg},
    {"ColorPresetInactiveFg", &RadioSettings::preset_inactive_fg}, {"ColorPresetInactiveBg", &RadioSettings::preset_inactive_bg},
    {"ColorPresetKeyFg", &RadioSettings::preset_key_fg},
    {"ColorHeader", &RadioSettings::header},              {"ColorLegend", &RadioSettings::legend},            {"ColorLegendCommand", &RadioSettings::legend_cmd},
    {"ColorTabCurrent", &RadioSettings::tab_current},     {"ColorTabOther", &RadioSettings::tab_other},
    {"ColorFocus", &RadioSettings::focus},
};
} // namespace

namespace {
// Does the theme carry its own radio value for the focus indicator? A theme from before it existed (or one made in
// the player) does not: the radio's focus indicator then follows the radio's header colour, as it always did.
bool has_radio_focus(const themes::Theme& t) {
    for (const auto& p : t.kv) if (p.first == "Radio.ColorFocus") return true;
    return false;
}
} // namespace

void theme_apply(RadioSettings& s, const themes::Theme& t) {
    for (const ThemeKey& k : kThemeKeys) if (const std::string* v = t.get(k.key, true)) s.*(k.p) = *v;
    if (!has_radio_focus(t)) s.focus = s.header;
}

themes::Theme theme_capture(const RadioSettings& s, themes::Theme base) {
    base.builtin = false;
    // Everything the radio's colours say goes into the theme's "Radio." keys: a theme made (or saved) in the radio keeps
    // the player's look as it was and the radio gets its own, so each mode can have its own colours in the same theme.
    for (const ThemeKey& k : kThemeKeys) base.set(k.key, s.*(k.p), true);
    return base;
}

bool theme_matches(const RadioSettings& s, const themes::Theme& t) {
    int known = 0;
    for (const ThemeKey& k : kThemeKeys) {
        if (std::string(k.key) == "ColorFocus" && !has_radio_focus(t)) continue;   // nothing of its own to compare
        const std::string* v = t.get(k.key, true);
        if (!v) continue;
        ++known;
        if (*v != s.*(k.p)) return false;
    }
    return known > 0;
}

int theme_index(const RadioSettings& s, const std::vector<themes::Theme>& all) {
    // a theme of your own wins over a built-in one with exactly the same colours (a renamed copy keeps its new name)
    for (size_t i = 0; i < all.size(); ++i) if (!all[i].builtin && theme_matches(s, all[i])) return static_cast<int>(i);
    for (size_t i = 0; i < all.size(); ++i) if (theme_matches(s, all[i])) return static_cast<int>(i);
    return -1;
}

namespace {
enum RsId {
    kRsPlayback, kRsStartMode, kRsUseModePlayer, kRsUseModeRadio, kRsUseModeBook, kRsModeOrder1, kRsModeOrder2, kRsModeOrder3, kRsBlackBg, kRsShareProfiles, kRsRemoteApp,
    kRsOnAir, kRsPulse, kRsDummy, kRsVolIndicator, kRsVisualizer, kRsStereo, kRsNormalize, kRsTuneNoise, kRsMarquee, kRsTimeshift, kRsUseOsci, kRsUseSpectro,
    kRsGradSpeed, kRsPulseSpeed, kRsFluidity, kRsDegr, kRsVisc, kRsBehavior, kRsBarStyle, kRsHifiRes, kRsHifiChan, kRsHifiColor, kRsHifiScale, kRsHifiAdapt, kRsHifiSplit, kRsHifiFScale, kRsHifiGain, kRsHifiRange,
    kRsScope, kRsOsciStyle,
    kRsAudioDevice,   // [ENTER]: the CHOOSE AUDIO DEVICE overlay
    kRsTermDefaultFont, // leave the terminal's own font alone (one value for all modes: TerminalDefaultFont= in the player's config.txt)
};
}

static bool g_rs_hifi = false;   // the Hi-Fi rows are listed (rs_set_hifi_rows)
const std::vector<SettingsLine>& rs_layout(int tab) {
    // A header with an empty text is the blank line before the next header.
    static const std::vector<SettingsLine> on_off = {
        {true, -1, "MODES"},
        {false, kRsPlayback, "Playback"}, {false, kRsStartMode, "Start Mode"},
        {true, -1, ""},
        {true, -1, "MODES ON/OFF AND ORDER"},
        {false, kRsUseModePlayer, "Use Music Player"}, {false, kRsUseModeRadio, "Use Radio"}, {false, kRsUseModeBook, "Use Audio Books"},
        {false, kRsModeOrder1, "Mode Order 1"}, {false, kRsModeOrder2, "Mode Order 2"}, {false, kRsModeOrder3, "Mode Order 3"},
        {true, -1, ""},
        {true, -1, "TERMINAL"},
        {false, kRsBlackBg, "Black Background"},
        {true, -1, ""},
        {true, -1, "REMOTE / UPDATES"},
        {false, kRsRemoteApp, "Remote App"},
        {true, -1, ""},
        {true, -1, "AUDIO"},
        {false, kRsAudioDevice, "Choose Audio Device"},
        {false, kRsOnAir, "On air ascii"}, {false, kRsPulse, "Pulse wave"}, {false, kRsDummy, "Dummy buttons"},
        {false, kRsStereo, "Stereo sound"}, {false, kRsNormalize, "Normalize volume"},
        {false, kRsTuneNoise, "Tuning noise"}, {false, kRsTimeshift, "Timeshift buffer"},
        {false, kRsUseOsci, "Use oscilloscope"}, {false, kRsUseSpectro, "Use spectrogram"},   // false: not in the "." cycle (the window still works)
        {true, -1, ""},
        {true, -1, "TEXT"},
        {false, kRsTermDefaultFont, "Terminal default font"},
        {false, kRsMarquee, "Marquee scrolling"},
    };
    static const std::vector<SettingsLine> anima = {
        {true, -1, "ON AIR SIGN"},
        {false, kRsGradSpeed, "Gradient wave speed"}, {false, kRsPulseSpeed, "Pulse wave speed"},
        {true, -1, ""},
        {true, -1, "SEEK AND VOLUME"},
        {false, kRsVolIndicator, "Volume indicator"},
        {true, -1, ""},
        {true, -1, "SPECTRUM BARS"},   // the spectrum bars under the station info
        {false, kRsVisualizer, "On/Off"},
        {false, kRsBarStyle, "Bars Style"}, {false, kRsHifiRes, "Hi-Fi Resolution"}, {false, kRsHifiChan, "Hi-Fi Channels"}, {false, kRsHifiColor, "Hi-Fi Color"}, {false, kRsHifiScale, "Hi-Fi Height"}, {false, kRsHifiAdapt, "Hi-Fi Adapt Height"}, {false, kRsHifiSplit, "Hi-Fi Channel Gap"}, {false, kRsHifiFScale, "Hi-Fi Frequency Scale"}, {false, kRsHifiGain, "Hi-Fi Gain (dB)"}, {false, kRsHifiRange, "Hi-Fi Range (dB)"},
        {false, kRsBehavior, "Bars Behavior"}, {false, kRsFluidity, "Bars Fluidity"}, {false, kRsDegr, "Bars Degradation"}, {false, kRsVisc, "Bars Viscosity"},
        {true, -1, ""},
        {true, -1, "OSCI/SPECTRO SETTINGS"},
        {false, kRsShareProfiles, "Modes Share Settings"},
        {false, kRsScope, "Osci/sphere"}, {false, kRsOsciStyle, "Osci style"},
    };
    if (tab == 1) return on_off;
    if (g_rs_hifi) return anima;
    static std::vector<SettingsLine> plain;   // without Hi-Fi its three rows are left out
    if (plain.empty())
        for (const auto& l : anima) if (l.header || (l.id != kRsHifiRes && l.id != kRsHifiChan && l.id != kRsHifiColor && l.id != kRsHifiScale && l.id != kRsHifiAdapt && l.id != kRsHifiSplit && l.id != kRsHifiFScale && l.id != kRsHifiGain && l.id != kRsHifiRange)) plain.push_back(l);
    return plain;
}
void rs_set_hifi_rows(bool on) { g_rs_hifi = on; }
int rs_row_count(int tab) {
    int n = 0;
    for (const auto& l : rs_layout(tab)) if (!l.header) ++n;
    return n;
}
static int rs_id(int tab, int row) {
    int n = 0;
    for (const auto& l : rs_layout(tab)) if (!l.header && n++ == row) return l.id;
    return -1;
}
int rs_row_line(int tab, int row) {
    int n = 0, line = 0;
    for (const auto& l : rs_layout(tab)) { if (!l.header && n++ == row) return line; ++line; }
    return 0;
}
const char* rs_row_label(int tab, int row) {
    int n = 0;
    for (const auto& l : rs_layout(tab)) if (!l.header && n++ == row) return l.text;
    return "";
}
const int kOnOffRowCount = rs_row_count(1);
const int kAnimRowCount = rs_row_count(2);

std::string scope_mode_name(const RadioSettings& s) {
    return s.scope_mode == 1 ? "sphere" : s.scope_mode == 2 ? "off" : s.scope_mode == 3 ? "spectro" : "osci";
}

namespace {
// One scale for both wave speeds (cycles / loops per second). Contains both defaults (0.25 colour wave, 0.125 pulse).
constexpr double kSpeedSteps[] = {0.01, 0.03, 0.05, 0.10, 0.125, 0.17, 0.25, 0.35, 0.50, 0.75, 1.00};
constexpr int kSpeedStepCount = static_cast<int>(sizeof(kSpeedSteps) / sizeof(kSpeedSteps[0]));
int nearest_step(double v) {
    int best = 0;
    for (int i = 1; i < kSpeedStepCount; ++i)
        if (std::fabs(kSpeedSteps[i] - v) < std::fabs(kSpeedSteps[best] - v)) best = i;
    return best;
}
void cycle_int(int& v, int lo, int hi, int dir) {
    const int n = hi - lo + 1;
    v = lo + ((v - lo + (dir == 0 ? 1 : dir) + n) % n + n) % n;
}
void cycle_speed(double& v, int dir) {
    const int i = nearest_step(v);
    v = kSpeedSteps[((i + (dir == 0 ? 1 : dir)) % kSpeedStepCount + kSpeedStepCount) % kSpeedStepCount];
}
std::string fmt_speed(double v) {
    std::ostringstream os;
    os << v;
    return os.str();
}
bool* flag_of(RadioSettings& s, int id) {
    switch (id) {
        case kRsOnAir: return &s.on_air_ascii;
        case kRsPulse: return &s.pulse_wave;
        case kRsDummy: return &s.dummy_buttons;
        case kRsVisualizer: return &s.element_visualizer;
        case kRsStereo: return &s.stereo;
        case kRsNormalize: return &s.normalize;
        case kRsTuneNoise: return &s.tune_noise;
        case kRsMarquee: return &s.marquee;
        case kRsTermDefaultFont: return &s.terminal_default_font;
        case kRsUseOsci: return &s.use_osci;
        case kRsUseSpectro: return &s.use_spectro;
        case kRsBlackBg: return &s.black_background;
        default: return nullptr;
    }
}
} // namespace

std::string rs_row_value(const RadioSettings& s, int tab, int row) {
    g_rs_hifi = s.bars_style == 1;
    const int id = rs_id(tab, row);
    if (id < 0) return "";
    if (bool* f = flag_of(const_cast<RadioSettings&>(s), id)) return *f ? "true" : "false";
    switch (id) {
        case kRsShareProfiles: return s.share_profiles ? "true" : "false";
        case kRsAudioDevice: return (s.audio_device.empty() ? std::string("System Default") : s.audio_device) + "  [ENTER] choose";
        case kRsRemoteApp: return s.remote_app ? "on   [ENTER] settings" : "off  [ENTER] settings";   // opens the REMOTE APP overlay
        case kRsPlayback: return s.playback_shuffle ? "shuffle" : "list";
        case kRsStartMode: return s.start_mode == "radio" || s.start_mode == "audiobook" ? s.start_mode : "player";
        case kRsUseModePlayer: return s.use_mode[0] ? "true" : "false";
        case kRsUseModeRadio: return s.use_mode[1] ? "true" : "false";
        case kRsUseModeBook: return s.use_mode[2] ? "true" : "false";
        case kRsModeOrder1: return muisc::kModeNames[std::clamp(s.mode_order[0], 0, 2)];
        case kRsModeOrder2: return muisc::kModeNames[std::clamp(s.mode_order[1], 0, 2)];
        case kRsModeOrder3: return muisc::kModeNames[std::clamp(s.mode_order[2], 0, 2)];
        case kRsVolIndicator: return muisc::kVolIndicatorNames[std::clamp(s.volume_indicator, 0, muisc::kVolStyleCount - 1)];
        case kRsTimeshift: return std::to_string(timeshift_minutes(s)) + " min";
        case kRsGradSpeed: return fmt_speed(kSpeedSteps[nearest_step(s.gradient_wave_speed)]);
        case kRsPulseSpeed: return fmt_speed(kSpeedSteps[nearest_step(s.pulse_wave_speed)]);
        case kRsFluidity: return std::to_string(s.bar_fluidity());
        case kRsBehavior: return bars_preset_name(bars_preset_of(s.bar_fluidity(), s.bar_degradation(), s.bar_viscosity()));
        case kRsBarStyle: return s.bars_style == 1 ? "hi-fi" : "classic";
        case kRsHifiRes: return s.hifi_resolution == 1 ? "rough" : "fine";
        case kRsHifiChan: return s.hifi_channels == 1 ? "mono" : "stereo";
        case kRsHifiColor: return s.hifi_color == 1 ? "corresponding" : "gradient";
        case kRsHifiScale: return s.hifi_scale == 0 ? "full" : s.hifi_scale == 2 ? "low" : s.hifi_scale == 3 ? "very low" : "medium";
        case kRsHifiAdapt: return s.hifi_adapt == 1 ? "on" : "off";
        case kRsHifiSplit: return s.hifi_split == 0 ? "remove" : "add";
        case kRsHifiFScale: { static const char* n[] = {"linear", "logarithmic", "mel", "bark", "erb"}; return n[std::clamp(s.hifi_fscale, 0, 4)]; }
        case kRsHifiGain: return std::to_string(s.hifi_gain);
        case kRsHifiRange: return std::to_string(s.hifi_range);
        case kRsDegr: return std::to_string(s.bar_degradation());
        case kRsVisc: return std::to_string(s.bar_viscosity());
        case kRsScope: return scope_mode_name(s);
        case kRsOsciStyle: return s.osci_style == 1 ? "image" : "braille";
    }
    return "";
}

void rs_row_change(RadioSettings& s, int tab, int row, int dir) {
    g_rs_hifi = s.bars_style == 1;
    const int id = rs_id(tab, row);
    if (id < 0) return;
    if (bool* f = flag_of(s, id)) { *f = dir == 0 ? !*f : dir > 0; return; }
    auto step = [&](int& v, int n) { v = dir == 0 ? (v + 1) % n : (v + (dir > 0 ? 1 : n - 1)) % n; };
    switch (id) {
        case kRsShareProfiles: radio_share_profiles(s, dir == 0 ? !s.share_profiles : dir > 0); break;
        case kRsRemoteApp:
            s.remote_app = dir == 0 ? !s.remote_app : dir > 0;
            if (s.remote_app && s.remote_pin.empty()) s.remote_pin = muisc::remote::net::make_pin();
            break;
        case kRsPlayback: s.playback_shuffle = !s.playback_shuffle; break;
        case kRsStartMode: {   // walks through the modes that are switched on, in the mode order
            int cur = muisc::mode_index(s.start_mode);
            if (cur < 0) cur = 0;
            muisc::ModeConfig mc; for (int i = 0; i < 3; ++i) { mc.use[i] = s.use_mode[i]; mc.order[i] = s.mode_order[i]; }
            const int nx = muisc::next_mode_in(mc, cur, dir < 0 ? -1 : 1);
            s.start_mode = muisc::kModeNames[nx];
            break;
        }
        case kRsUseModePlayer: case kRsUseModeRadio: case kRsUseModeBook: {
            const int m = id - kRsUseModePlayer;
            const bool want = dir == 0 ? !s.use_mode[m] : dir > 0;
            int others = 0;
            for (int i = 0; i < 3; ++i) if (i != m && s.use_mode[i]) ++others;
            if (want || others > 0) s.use_mode[m] = want;   // one mode always stays on
            // a start mode that is switched off moves to the first mode that is on
            int sm = muisc::mode_index(s.start_mode);
            if (sm < 0 || !s.use_mode[sm]) for (int i = 0; i < 3; ++i) if (s.use_mode[s.mode_order[i]]) { s.start_mode = muisc::kModeNames[s.mode_order[i]]; break; }
            break;
        }
        case kRsModeOrder1: case kRsModeOrder2: case kRsModeOrder3: {   // the slot takes the next / previous mode; the slot that held it takes the old one (a swap)
            const int slot = id - kRsModeOrder1;
            const int old = s.mode_order[slot];
            const int nw = ((old + (dir < 0 ? 2 : 1)) % 3 + 3) % 3;
            for (int i = 0; i < 3; ++i) if (s.mode_order[i] == nw) s.mode_order[i] = old;
            s.mode_order[slot] = nw;
            break;
        }
        case kRsVolIndicator: step(s.volume_indicator, muisc::kVolStyleCount); break;
        case kRsTimeshift: step(s.timeshift_idx, 5); break;
        case kRsGradSpeed: cycle_speed(s.gradient_wave_speed, dir); break;
        case kRsPulseSpeed: cycle_speed(s.pulse_wave_speed, dir); break;
        case kRsFluidity: cycle_int(s.bar_fluidity(), 1, 10, dir); break;
        case kRsBehavior: {
            const int now = bars_preset_of(s.bar_fluidity(), s.bar_degradation(), s.bar_viscosity());
            const int to = now == kBarsCustom ? (dir > 0 ? 0 : 3) : ((now + dir) % 4 + 4) % 4;
            s.bar_fluidity() = kBarsPresets[to].fluidity; s.bar_degradation() = kBarsPresets[to].degradation; s.bar_viscosity() = kBarsPresets[to].viscosity;
            break;
        }
        case kRsBarStyle: step(s.bars_style, 2); g_rs_hifi = s.bars_style == 1; break;
        case kRsHifiRes: step(s.hifi_resolution, 2); break;
        case kRsHifiChan: step(s.hifi_channels, 2); break;
        case kRsHifiColor: step(s.hifi_color, 2); break;
        case kRsHifiScale: step(s.hifi_scale, 4); break;
        case kRsHifiAdapt: step(s.hifi_adapt, 2); break;
        case kRsHifiSplit: step(s.hifi_split, 2); break;
        case kRsHifiFScale: step(s.hifi_fscale, 5); break;
        case kRsHifiGain: s.hifi_gain = std::clamp(s.hifi_gain + 2 * dir, -20, 20); break;
        case kRsHifiRange: s.hifi_range = std::clamp(s.hifi_range + 4 * dir, 40, 100); break;
        case kRsDegr: cycle_int(s.bar_degradation(), 1, 10, dir); break;
        case kRsVisc: cycle_int(s.bar_viscosity(), 0, 10, dir); break;
        case kRsScope: step(s.scope_mode, 4); break;
        case kRsOsciStyle: step(s.osci_style, 2); break;
    }
}

std::string osci_row_value(const RadioSettings& s, int row) {
    if (row == kOrDisplay) return s.scope_mode == 1 ? "sphere" : s.scope_mode == 3 ? "spectro" : "osci";
    return muisc::osci_row_value(static_cast<const VizPrefs&>(s), row);
}

void osci_adjust(RadioSettings& s, int row, int dir) {
    if (row != kOrDisplay) { muisc::osci_adjust(static_cast<VizPrefs&>(s), row, dir); return; }
    {   // osci -> sphere -> spectro (off only on the GENERAL tab; switched-off ones are skipped)
            static const int order[3] = {0, 1, 3};
            int i = s.scope_mode == 1 ? 1 : s.scope_mode == 3 ? 2 : 0;
            for (int k = 0; k < 3; ++k) {
                i = (i + (dir > 0 ? 1 : 2)) % 3;
                if ((order[i] == 0 && !s.use_osci) || (order[i] == 3 && !s.use_spectro)) continue;
                break;
            }
            s.scope_mode = order[i];
    }
}
void radio_enforce_visuals(RadioSettings& s) {
    if (s.scope_mode == 0 && !s.use_osci) s.scope_mode = 1;      // the sphere instead
    if (s.scope_mode == 3 && !s.use_spectro) s.scope_mode = 1;
}

void norm_adjust(RadioSettings& s, int row, int dir) {
    if (row == 0) { s.normalize = dir > 0; return; }
    double* v = row == 1 ? &s.normalize_target_lufs : &s.normalize_max_boost_db;
    const Knob& k = kNormKnobs[std::clamp(row, 0, 2)];
    *v = std::clamp(std::round((*v + dir * k.step) / k.step) * k.step, k.lo, k.hi);
}
void norm_reset(RadioSettings& s) {
    const RadioSettings d;
    s.normalize_target_lufs = d.normalize_target_lufs; s.normalize_max_boost_db = d.normalize_max_boost_db;
}

const int kPathRowCount = 6;
const char* const kPathRows[] = {"STATION LISTS PATH", "PRESETS PATH", "DOWNLOAD PATH", "Same folder as music player", "HISTORY PATH", "AUDIO BOOK PATH"};

std::string* path_row_text(RadioSettings& s, int row) {
    switch (row) {
        case 0: return &s.stations_path;
        case 1: return &s.presets_path;
        case 2: return &s.download_path;
        case 4: return &s.history_path;
        case 5: return &s.audiobook_path;   // lives in the player's config.txt (AudioBookPath=), not in radio_config.txt
    }
    return nullptr;
}
const std::string* path_row_text(const RadioSettings& s, int row) { return path_row_text(const_cast<RadioSettings&>(s), row); }

std::string* color_field(RadioSettings& s, int row, int col) {
    if (row < 0 || row >= kColorRowCount) return nullptr;
    const ColorRowSpec& r = kColorRows[row];
    if (col == 0) return &(s.*(r.field1));
    if (col == 1 && r.field2) return &(s.*(r.field2));
    return nullptr;
}
const std::string* color_field(const RadioSettings& s, int row, int col) {
    return color_field(const_cast<RadioSettings&>(s), row, col);
}

// ===========================================================================================
// colour helpers
// ===========================================================================================
namespace {

int color_index(const std::string& v) {
    if (v.empty()) return 0;
    try { return std::stoi(v); } catch (...) { return 0; }
}

struct RGB { int r, g, b; };

RGB palette_rgb(int idx) {
    static const RGB basic16[16] = {
        {0,0,0}, {128,0,0}, {0,128,0}, {128,128,0}, {0,0,128}, {128,0,128}, {0,128,128}, {192,192,192},
        {128,128,128}, {255,0,0}, {0,255,0}, {255,255,0}, {0,0,255}, {255,0,255}, {0,255,255}, {255,255,255}};
    if (idx >= 0 && idx < 16) return basic16[idx];
    if (idx >= 16 && idx <= 231) {                       // 6x6x6 cube
        const int ci = idx - 16;
        auto v = [](int c) { return c == 0 ? 0 : 55 + 40 * c; };
        return {v(ci / 36), v((ci / 6) % 6), v(ci % 6)};
    }
    if (idx >= 232 && idx <= 255) { const int g = 8 + (idx - 232) * 10; return {g, g, g}; }   // greys
    return {255, 255, 255};
}

bool parse_rgb(const std::string& v, RGB& out) {
    const int idx = color_index(v);
    if (idx <= 0 || idx > 255) return false;             // none has no RGB
    out = palette_rgb(idx);
    return true;
}

} // namespace

bool color_rgb(const std::string& color, int& r, int& g, int& b) {
    RGB c;
    if (!parse_rgb(color, c)) return false;
    r = c.r; g = c.g; b = c.b;
    return true;
}

// Every colour number is a 256-colour palette number in every field (see themes::legacy_to_palette for the old files).
std::string ansi_fg(const std::string& color) {
    const int v = color_index(color);
    if (v <= 0) return "";
    return "\x1b[38;5;" + std::to_string(v) + "m";
}

std::string ansi_bg(const std::string& color) {
    const int v = color_index(color);
    if (v <= 0) return "";
    return "\x1b[48;5;" + std::to_string(v) + "m";
}

std::string sgr_params(const std::string& color) {
    const int v = color_index(color);
    if (v <= 0) return "";
    return "38;5;" + std::to_string(v);
}

std::string gradient_fg(const std::string& start, const std::string& end, float t) {
    if (end.empty()) return ansi_fg(start);
    // A frame asks for the same few colour pairs hundreds of times (every cell of a gradient, the border, the scope, the lamp ...):
    // the last 8 pairs are remembered, so the names are read once and not on every call.
    struct Pair { std::string s, e; RGB a{}, b{}; bool ok = false, used = false; };
    static thread_local Pair cache[8];
    static thread_local unsigned next = 0;
    Pair* hit = nullptr;
    for (Pair& c : cache) if (c.used && c.s == start && c.e == end) { hit = &c; break; }
    if (!hit) {
        hit = &cache[next++ % 8];
        hit->s = start; hit->e = end; hit->used = true;
        hit->ok = parse_rgb(start, hit->a) && parse_rgb(end, hit->b);
    }
    if (!hit->ok) return ansi_fg(start);
    const RGB a = hit->a, b = hit->b;
    t = std::clamp(t, 0.0f, 1.0f);
    const int r = static_cast<int>(a.r + (b.r - a.r) * t);
    const int g = static_cast<int>(a.g + (b.g - a.g) * t);
    const int bl = static_cast<int>(a.b + (b.b - a.b) * t);
    // "ESC[38;2;R;G;Bm", written straight into a buffer (it is built hundreds of times per frame)
    char buf[24];
    char* p = buf;
    *p++ = '\x1b'; *p++ = '['; *p++ = '3'; *p++ = '8'; *p++ = ';'; *p++ = '2'; *p++ = ';';
    const auto put = [&p](int v) {
        v = std::clamp(v, 0, 255);
        if (v >= 100) { *p++ = static_cast<char>('0' + v / 100); v %= 100; *p++ = static_cast<char>('0' + v / 10); *p++ = static_cast<char>('0' + v % 10); }
        else if (v >= 10) { *p++ = static_cast<char>('0' + v / 10); *p++ = static_cast<char>('0' + v % 10); }
        else *p++ = static_cast<char>('0' + v);
    };
    put(r); *p++ = ';'; put(g); *p++ = ';'; put(bl); *p++ = 'm';
    return std::string(buf, static_cast<size_t>(p - buf));
}

std::string gradient3_fg(const std::string& left, const std::string& center, const std::string& right, float t) {
    if (center.empty()) return gradient_fg(left, right, t);
    t = std::clamp(t, 0.0f, 1.0f);
    return t <= 0.5f ? gradient_fg(left, center, t * 2.0f) : gradient_fg(center, right, (t - 0.5f) * 2.0f);
}

std::string gradient_bar(const std::string& start, const std::string& end, int width) {
    std::string out;
    for (int i = 0; i < width; ++i)
        out += gradient_fg(start, end, width > 1 ? static_cast<float>(i) / static_cast<float>(width - 1) : 0.0f) + "█";
    return out + "\x1b[0m";
}

bool valid_color_value(const std::string& v) {
    if (v.empty()) return true;
    for (unsigned char c : v) if (!std::isdigit(c)) return false;
    if (v.size() > 3) return false;
    return std::stoi(v) <= 255;
}

// ===========================================================================================
// config file
// ===========================================================================================
namespace {



fs::path home_config_path() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = std::getenv("USERPROFILE");
    if (home && *home) return fs::path(home) / ".config" / "mousiki" / "radio_config.txt";
    return fs::path("radio_config.txt");
}

bool as_bool(const std::string& v, bool def) {
    const std::string l = ascii_lower_str(v);
    if (l == "true" || l == "1" || l == "on" || l == "yes") return true;
    if (l == "false" || l == "0" || l == "off" || l == "no") return false;
    return def;
}

int as_int(const std::string& v, int def, int lo, int hi) {
    try { return std::clamp(std::stoi(v), lo, hi); } catch (...) { return def; }
}

} // namespace

fs::path radio_default_dir() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = std::getenv("USERPROFILE");
    if (home && *home) return fs::path(home) / ".config" / "mousiki";
    return fs::path(".");
}

std::string player_download_folder() {
    // Read-only peek at the player's own config.txt; the radio has no other connection to the player's settings.
    std::ifstream in(radio_default_dir() / "config.txt");
    std::string line;
    while (std::getline(in, line)) {
        line = trim_ws(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = ascii_lower_str(trim_ws(line.substr(0, eq)));
        if (key == "downloadfolder" || key == "download_folder") return trim_ws(line.substr(eq + 1));
    }
    return "";
}

namespace {
// One KEY=value line of the player's config.txt (the settings both programs share live there): the value, "" when missing.
std::string player_config_value(const std::string& key_lower) {
    std::ifstream in(radio_default_dir() / "config.txt");
    std::string line;
    while (std::getline(in, line)) {
        line = trim_ws(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (ascii_lower_str(trim_ws(line.substr(0, eq))) == key_lower) return trim_ws(line.substr(eq + 1));
    }
    return "";
}
// Rewrites that one line (appended when it is missing); nothing else in the file is touched. A config.txt that does not
// exist is not created here: the player owns it.
bool set_player_config_value(const std::string& key, const std::string& value) {
    const fs::path f = radio_default_dir() / "config.txt";
    std::ifstream in(f);
    if (!in) return false;
    std::vector<std::string> lines;
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        const std::string t = trim_ws(line);
        const size_t eq = t.find('=');
        if (!t.empty() && t[0] != '#' && eq != std::string::npos && ascii_lower_str(trim_ws(t.substr(0, eq))) == ascii_lower_str(key)) {
            if (!found) lines.push_back(key + "=" + value);
            found = true;
            continue;
        }
        lines.push_back(line);
    }
    in.close();
    if (!found) lines.push_back(key + "=" + value);
    std::string text;
    for (const auto& l : lines) text += l + "\n";
    return write_text_atomic(f, text);
}
} // namespace

std::string player_start_mode() {
    // The start mode lives in the player's config.txt (StartMode=player|radio), one value for both programs.
    const std::string v = ascii_lower_str(player_config_value("startmode"));
    return v == "radio" ? "radio" : v == "audiobook" ? "audiobook" : "player";
}

bool set_player_start_mode(const std::string& mode) {
    return set_player_config_value("StartMode", mode == "radio" ? "radio" : mode == "audiobook" ? "audiobook" : "player");
}

void load_player_mode_settings(RadioSettings& s) {
    const muisc::ModeConfig mc = muisc::read_mode_config();   // same file; defaults when missing, always one mode on
    for (int i = 0; i < 3; ++i) { s.use_mode[i] = mc.use[i]; s.mode_order[i] = mc.order[i]; }
    s.audiobook_path = player_config_value("audiobookpath");
    {
        std::string v = ascii_lower_str(player_config_value("terminaldefaultfont"));
        s.terminal_default_font = v == "true" || v == "1" || v == "yes" || v == "on";
        if (const std::string f = player_config_value("terminalfont"); !f.empty()) s.terminal_font = f;
    }
    s.audio_device = player_config_value("audiodevice");   // CHOOSE AUDIO DEVICE: one choice for every mode
    int sm = muisc::mode_index(s.start_mode);
    if (sm < 0 || !s.use_mode[sm]) for (int i = 0; i < 3; ++i) if (s.use_mode[s.mode_order[i]]) { s.start_mode = muisc::kModeNames[s.mode_order[i]]; break; }
}

void save_player_mode_settings(const RadioSettings& s) {
    const muisc::ModeConfig now = muisc::read_mode_config();
    const char* const keys[3] = {"UseModePlayer", "UseModeRadio", "UseModeAudiobook"};
    for (int i = 0; i < 3; ++i) if (now.use[i] != s.use_mode[i]) set_player_config_value(keys[i], s.use_mode[i] ? "true" : "false");
    bool same = true;
    for (int i = 0; i < 3; ++i) if (now.order[i] != s.mode_order[i]) same = false;
    if (!same) set_player_config_value("ModeOrder", muisc::mode_order_text(s.mode_order));
    if (s.audiobook_path != player_config_value("audiobookpath")) set_player_config_value("AudioBookPath", s.audiobook_path);
}

bool player_black_background() {
    // Black Background: one value for both programs, BlackBackground= in the player's config.txt (true when missing)
    const std::string v = ascii_lower_str(player_config_value("blackbackground"));
    return !(v == "false" || v == "0" || v == "off" || v == "no");
}

bool player_reduce_motion() {
    // Reduce Motion When Paused: one value for both programs, ReduceMotionWhenPaused= in the player's config.txt (true when missing)
    const std::string v = ascii_lower_str(player_config_value("reducemotionwhenpaused"));
    return !(v == "false" || v == "0" || v == "off" || v == "no");
}

bool set_player_black_background(bool on) {
    return set_player_config_value("BlackBackground", on ? "true" : "false");
}

bool player_share_profiles() {
    const std::string v = ascii_lower_str(player_config_value("shareprofiles"));
    return v == "true" || v == "1" || v == "on" || v == "yes";
}

bool set_player_share_profiles(bool on) {
    return set_player_config_value("ShareProfiles", on ? "true" : "false");
}

void radio_share_profiles(RadioSettings& s, bool on) {
    if (on) {
        if (!s.share_profiles) s.own_viz = s;          // the radio's own look is kept (the VizPrefs part of s)
        s.share_profiles = true;
        VizPrefs pv;
        if (read_player_viz(pv)) s.take_shared_from(pv);   // no player config.txt yet: the radio keeps its own values
    } else {
        if (s.share_profiles) s.take_shared_from(s.own_viz);   // back to the radio's own values (its grid colour was never shared)
        s.share_profiles = false;
    }
}


WizardPalette player_wizard_palette() {
    WizardPalette p;
    // Same read-only peek as above. A row the player's config.txt does not
    // carry simply keeps the default in the struct, so a missing or partial
    // config.txt costs nothing.
    std::ifstream in(radio_default_dir() / "config.txt");
    std::string line;
    while (std::getline(in, line)) {
        line = trim_ws(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = ascii_lower_str(trim_ws(line.substr(0, eq)));
        const std::string val = trim_ws(line.substr(eq + 1));
        if      (key == "colorbordertop")         p.border      = val;
        else if (key == "colorborderbottom")      p.border_bot  = val;
        else if (key == "colorheader")            p.header      = val;
        else if (key == "colorlegend")            p.legend      = val;
        else if (key == "colorlistinactivefg")    p.list        = val;
        else if (key == "colorlistcursorfg")      p.cursor_fg   = val;
        else if (key == "colorlistcursorbg")      p.cursor_bg   = val;
        else if (key == "colorlyricsactivewordfg")p.accent      = val;
        else if (key == "colordisktop")           p.disk_top    = val;
        else if (key == "colordiskbottom")        p.disk_bottom = val;
    }
    // The accent (the arrows and the <<spans>>) has to be visible even on a
    // config.txt that leaves the lyric colours blank, which is how the player
    // ships: fall back the same way the player's own wizard does.
    if (p.accent.empty() || p.accent == "0") p.accent = p.cursor_fg;
    if (p.accent.empty() || p.accent == "0") p.accent = p.header;
    return p;
}


// ---- REFERENCE ----------------------------------------------------------------------------------------------
const KeyAction kKeyActions[] = {
    {"PLAYBACK", "Next", "Next Channel", "n"},
    {nullptr, "Shuffle", "Random Channel", "#"},
    {nullptr, "Previous", "Previous Channel", "b"},
    {nullptr, "Mode", "Surf Mode S / L", "m"},
    {nullptr, "Mute", "Mute / Unmute", "p"},
    {nullptr, "Stop", "Stop Stream", "x"},
    {nullptr, "Reconnect", "Reconnect", "R"},
    {nullptr, "VolumeUp", "Volume Up", "+"},
    {nullptr, "VolumeDown", "Volume Down", "-"},
    {nullptr, "Normalize", "Toggle Normalization", "v"},
    {nullptr, "Record", "Record Stream", "y"},
    {nullptr, "SleepTimer", "Sleep Timer", "Z"},
    {"MENUS", "Search", "Search Stations", "/"},
    {nullptr, "Sort", "Toggle Sort A-Z", "T"},
    {nullptr, "Presets", "Open Presets Menu", "P"},
    {nullptr, "Browser", "Open Radio Browser", "S"},
    {nullptr, "Lists", "Open Station Lists", "K"},
    {nullptr, "History", "Open History", "h"},
    {nullptr, "Stations", "Big Stations Overlay", "L"},
    {nullptr, "OsciMenu", "Oscilloscope Overlay", "O"},
    {nullptr, "ScopeWindow", "Scope Window", ")"},            // SHIFT+9 on a German keyboard (was SHIFT+w)
    {nullptr, "SpectroWindow", "Spectrogram Window", "("},    // SHIFT+8 on a German keyboard
    {nullptr, "SpectroMenu", "Spectrogram Options", "I"},
    {nullptr, "BarsMenu", "Spectrum Bars Options", "B"},   // SHIFT+b, as in the music player
    {nullptr, "SpectroFull", "Spectrogram Full Screen", "U"},
    {nullptr, "ScopeBig", "Oscilloscope Full Screen", "="},     // SHIFT+0 on a German keyboard: YT left, XY right, grid
    {nullptr, "NormMenu", "Normalization Overlay", "V"},
    {nullptr, "EqMenu", "Equalizer Overlay", "E"},
    {nullptr, "ScopeToggle", "Cycle Osci / Sphere / Spectro", "."},   // "." like the player's lyrics-area cycle (was "o" up to v3.0.0)
    {"TIMESHIFT", "Pause", "Pause / Resume (keeps buffering)", "SPACE"},
    {nullptr, "Back30", "Back 30 Seconds", "["},
    {nullptr, "Fwd30", "Forward 30 Seconds", "]"},
    {nullptr, "Back5m", "Back 5 Minutes", "{"},
    {nullptr, "GoLive", "Back to Live", "}"},
    {"SYSTEM", "Settings", "Open Settings", "s"},
    {nullptr, "Wizard", "Help Wizard", "W"},          // SHIFT+w, the same key the music player uses
    {nullptr, "Cheatsheet", "Cheatsheet", "?"},
    {nullptr, "Menus", "Menus / Overlays Index", "<"},
    {nullptr, "AudioDevice", "Choose Audio Device", "°"},   // SHIFT+^ on a German keyboard; "&" by default on other layouts (key_binding)
    {nullptr, "Quit", "Quit Application", "q"},
};
const int kKeyActionCount = static_cast<int>(sizeof(kKeyActions) / sizeof(kKeyActions[0]));

std::string key_binding(const RadioSettings& s, int a) {
    if (a < 0 || a >= kKeyActionCount) return "";
    const auto it = s.keys.find(kKeyActions[a].id);
    if (it != s.keys.end() && !it->second.empty()) return it->second;
    if (std::strcmp(kKeyActions[a].id, "AudioDevice") == 0) return muisc::default_audio_device_key();   // "°" or "&", by keyboard layout
    return kKeyActions[a].def;
}

int key_code(const std::string& k) {
    if (k == "SPACE") return ' ';
    if (k == "TAB") return 9;
    if (k == "BACKSPACE") return 127;
    if (k == "\xC2\xB0") return kKeyDegree;   // "°": poll_key() joins its two bytes into one key
    if (k.size() == 1 && static_cast<unsigned char>(k[0]) > 32 && static_cast<unsigned char>(k[0]) < 127) return static_cast<unsigned char>(k[0]);
    return 0;
}

std::string key_conflict(const RadioSettings& s, const std::string& key, int except) {
    const int code = key_code(key);
    if (code == 0) return "";
    for (int i = 0; i < kKeyActionCount; ++i)
        if (i != except && key_code(key_binding(s, i)) == code) {
            std::string up = kKeyActions[i].label;
            for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return up;
        }
    for (const char* p = "1234567890ertdfg"; *p; ++p) if (code == *p) return "PRESET SLOT KEY";
    if (code == 'j' || code == 'k') return "CURSOR DOWN / UP";
    if (code == '=' || code == '_') return "VOLUME (= and _ are the unshifted + and -)";
    return "";
}

int key_translate(const RadioSettings& s, int typed) {
    {   // CHOOSE AUDIO DEVICE: its key (maybe "°", which is no single character) always comes out as kKeyDegree for the main switch
        const int adev = key_of(s, "AudioDevice");
        if (adev != 0 && typed == adev) return kKeyDegree;
        if (typed == kKeyDegree) return 0;   // "°" while the action has another key
    }
    if (typed <= 32 && typed != 9) return typed;
    if (typed >= 127) return typed;
    // the old aliases of + and - follow the volume actions while those keep their defaults
    // ('=' is not an alias of '+'; it is the big scope overlay's key -- SHIFT+0 on a German keyboard)
    if (typed == '_' && key_binding(s, 8) == "-") return '-';
    for (int i = 0; i < kKeyActionCount; ++i)
        if (key_code(key_binding(s, i)) == typed) return static_cast<unsigned char>(kKeyActions[i].def[0]);
    for (int i = 0; i < kKeyActionCount; ++i)
        if (typed == static_cast<unsigned char>(kKeyActions[i].def[0])) return 0;   // a default key whose action moved elsewhere
    return typed;
}

int key_of(const RadioSettings& s, const char* id) {
    for (int i = 0; i < kKeyActionCount; ++i)
        if (std::string(kKeyActions[i].id) == id) return key_code(key_binding(s, i));
    return 0;
}


StoragePaths resolve_storage_paths(const RadioSettings& s) {
    StoragePaths o;
    const fs::path def = radio_default_dir();
    // Empty = the layout the radio always had: the files sit directly in ~/.config/mousiki.
    if (s.stations_path.empty()) { o.stations_file = def / "stations.txt"; o.lists_file = def / "stationlists.txt"; }
    else {
        o.stations_file = fs::path(s.stations_path) / "stations" / "stations.txt";
        o.lists_file = fs::path(s.stations_path) / "station_lists" / "stationlists.txt";
    }
    o.presets_file = s.presets_path.empty() ? def / "presets.txt" : fs::path(s.presets_path) / "presets" / "presets.txt";
    o.history_dir = (s.history_path.empty() ? def : fs::path(s.history_path)) / "radio_history";
    if (s.download_same_as_player) {
        const std::string p = player_download_folder();
        o.player_download = true;
        if (!p.empty()) o.download_dir = p;
        else {
            const char* home = std::getenv("HOME");
            if (!home || !*home) home = std::getenv("USERPROFILE");
            o.download_dir = fs::path(home ? home : ".") / ".cache" / "mousiki";
            o.download_note = "player has no DownloadFolder: its cache";
        }
    } else {
        fs::path base = s.download_path;
        if (base.empty()) {
            const char* home = std::getenv("HOME");
            if (!home || !*home) home = std::getenv("USERPROFILE");
            base = fs::path(home ? home : ".") / "Music";
        }
        o.download_dir = base / "radio_downloads";
    }
    return o;
}

fs::path radio_config_path() {
    // Same search order as stations.txt: the user's folder first, then the working directory.
    const fs::path home = home_config_path();
    std::error_code ec;
    if (fs::exists(home, ec)) return home;
    if (fs::exists("radio_config.txt", ec)) return fs::path("radio_config.txt");
    return home;   // nothing yet: this is where a save creates it
}

RadioSettings load_radio_settings(std::string* source_out) {
    RadioSettings s;
    const fs::path p = radio_config_path();
    std::ifstream in(p);
    if (!in) {   // no radio_config.txt yet: the defaults, plus what the player's config.txt holds for both modes
        if (source_out) *source_out = "built-in defaults";
        s.start_mode = player_start_mode();
        load_player_mode_settings(s);
        s.black_background = player_black_background();
        if (player_share_profiles()) radio_share_profiles(s, true);
        const RemoteAppConfig rc = read_remote_app();
        s.remote_app = rc.enabled; s.remote_port = rc.port; s.remote_pin = rc.pin;
        return s;
    }
    if (source_out) *source_out = p.string();
    std::map<std::string, std::string> kv;
    std::vector<std::string> eq_preset_lines;
    std::string line;
    while (std::getline(in, line)) {
        line = trim_ws(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[ascii_lower_str(trim_ws(line.substr(0, eq)))] = trim_ws(line.substr(eq + 1));
        if (ascii_lower_str(trim_ws(line.substr(0, eq))) == "equalizerpreset") eq_preset_lines.push_back(trim_ws(line.substr(eq + 1)));   // repeated key
    }
    auto col = [&](const char* key, std::string& field) {
        const auto it = kv.find(ascii_lower_str(key));
        if (it != kv.end() && valid_color_value(it->second)) field = it->second;   // a bad value keeps the default
    };
    col("ColorBorderTop", s.border_top);          col("ColorBorderBottom", s.border_bottom);
    if (const auto it = kv.find("bordergradient"); it != kv.end()) s.border_smooth = ascii_lower_str(it->second) != "rough";
    col("ColorOnAirUpperLeft", s.on_air_upper_left); col("ColorOnAirBottomRight", s.on_air_bottom_right);
    col("ColorMetadataKey", s.meta_key);          col("ColorMetadataVal", s.meta_val);
    col("ColorVizLeft", s.viz_left);              col("ColorVizCenter", s.viz_center);   col("ColorVizRight", s.viz_right);
    col("ColorOsciLeft", s.osci_left);            col("ColorOsciRight", s.osci_right);
    col("ColorFrequencyLine", s.freq_line);       col("ColorFrequencyMHz", s.freq_mhz);
    col("ColorPresetInactiveFg", s.preset_inactive_fg); col("ColorPresetInactiveBg", s.preset_inactive_bg);
    col("ColorPresetKeyFg", s.preset_key_fg);
    col("ColorVolumeCurrent", s.volume_current);  col("ColorVolumePossible", s.volume_possible);
    col("ColorSearchFg", s.search_fg);            col("ColorSearchBg", s.search_bg);
    col("ColorSearchInactiveFg", s.search_inactive_fg); col("ColorSearchInactiveBg", s.search_inactive_bg);
    col("ColorListInactiveFg", s.list_fg);        col("ColorListInactiveBg", s.list_bg);
    col("ColorListPlayingFg", s.list_playing_fg); col("ColorListPlayingBg", s.list_playing_bg);
    col("ColorListCursorFg", s.list_cursor_fg);   col("ColorListCursorBg", s.list_cursor_bg);
    col("ColorHeader", s.header);                 col("ColorLegend", s.legend);                 col("ColorLegendCommand", s.legend_cmd);
    col("ColorTabCurrent", s.tab_current);        col("ColorTabOther", s.tab_other);
    s.focus = s.header;                           // no ColorFocus= line: the indicator keeps following the header colour
    col("ColorFocus", s.focus);

    auto get = [&](const char* key) -> const std::string* {
        const auto it = kv.find(ascii_lower_str(key));
        return it == kv.end() ? nullptr : &it->second;
    };
    if (auto v = get("StationListsPath")) s.stations_path = *v;
    if (auto v = get("PresetsPath")) s.presets_path = *v;
    if (auto v = get("DownloadPath")) s.download_path = *v;
    if (auto v = get("DownloadSameAsMusicPlayer")) s.download_same_as_player = as_bool(*v, s.download_same_as_player);
    if (auto v = get("HistoryPath")) s.history_path = *v;
    if (auto v = get("MenuIndexClassic")) s.menu_classic = as_bool(*v, s.menu_classic);
    if (auto v = get("SleepFade")) s.sleep_fade = as_bool(*v, s.sleep_fade);
    for (int i = 0; i < kKeyActionCount; ++i) {
        if (auto v = get((std::string("HKey") + kKeyActions[i].id).c_str())) {
            std::string val = *v;
            // Configs written up to v3.0.0 saved every key, so the old default "o" of the scope switch is
            // stored there as if it were a choice -- treat it as the old default and use the new one (".").
            if (std::string(kKeyActions[i].id) == "ScopeToggle" && val == "o") continue;
            if (std::string(kKeyActions[i].id) == "ScopeWindow" && val == "W") continue;   // the old default (now SHIFT+9)
            // v4.4.0: SHIFT+p opens the PRESETS menu and SHIFT+k the STATION LISTS (they were the other way round); the old defaults
            // saved in a config are taken as defaults
            if (std::string(kKeyActions[i].id) == "Presets" && val == "K") continue;
            if (std::string(kKeyActions[i].id) == "Lists" && val == "P") continue;
            if (key_code(val) != 0 && val != kKeyActions[i].def) s.keys[kKeyActions[i].id] = val;
        }
    }
    if (auto v = get("OnAirAscii")) s.on_air_ascii = as_bool(*v, s.on_air_ascii);
    if (auto v = get("UseOscilloscope")) s.use_osci = as_bool(*v, s.use_osci);
    if (auto v = get("UseSpectrogram")) s.use_spectro = as_bool(*v, s.use_spectro);
    if (auto v = get("WizardSeen")) s.wizard_seen = as_bool(*v, s.wizard_seen);
    if (auto v = get("PulseWave")) s.pulse_wave = as_bool(*v, s.pulse_wave);
    if (auto v = get("DummyButtons")) s.dummy_buttons = as_bool(*v, s.dummy_buttons);
    if (auto v = get("VolumeGradient")) s.volume_gradient = as_bool(*v, s.volume_gradient);
    if (auto v = get("VolumeIndicator")) { const int st = muisc::volume_style_from_name(*v); if (st >= 0) s.volume_indicator = st; }
    if (auto v = get("OsciSphere")) { const std::string l = ascii_lower_str(*v); s.scope_mode = l == "sphere" ? 1 : l == "off" ? 2 : (l == "spectro" || l == "spectrogram") ? 3 : 0; }
    for (const auto& [k, v] : kv) { if (!spectro_profile_key(s, k, v)) spectro_config_key(s.spectro, k, v); }
    spectro_profiles_finish(s);
    if (auto v = get("TuneNoise")) s.tune_noise = as_bool(*v, s.tune_noise);
    if (auto v = get("MarqueeScrolling")) s.marquee = as_bool(*v, s.marquee);
    if (auto v = get("TimeshiftMinutes")) {
        const int m = muisc::atoi_safe(v->c_str());
        int best = 0;
        for (int i = 0; i < 5; ++i) if (std::abs(kTimeshiftMinutes[i] - m) < std::abs(kTimeshiftMinutes[best] - m)) best = i;
        s.timeshift_idx = best;
    }
    if (auto v = get("StereoSound")) s.stereo = as_bool(*v, s.stereo);
    if (auto v = get("NormalizeVolume")) s.normalize = as_bool(*v, s.normalize);
    if (auto v = get("NormalizeTargetLufs")) s.normalize_target_lufs = as_int(*v, static_cast<int>(s.normalize_target_lufs), -40, 0);
    if (auto v = get("NormalizeMaxBoostDb")) s.normalize_max_boost_db = as_int(*v, static_cast<int>(s.normalize_max_boost_db), 0, 24);
    if (auto v = get("EqualizerEnabled")) s.eq_enabled = as_bool(*v, s.eq_enabled);
    auto parse_gains = [](const std::string& str, EqGains& out) {   // ten comma separated dB values; anything malformed is ignored as a whole
        EqGains g{};
        size_t pos = 0; int n = 0;
        while (pos <= str.size() && n < kEqBands) {
            const size_t comma = str.find(',', pos);
            const std::string tok = str.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            try { g[static_cast<size_t>(n++)] = std::clamp(std::stof(tok), kEqMinDb, kEqMaxDb); } catch (...) { return false; }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (n != kEqBands) return false;
        out = g;
        return true;
    };
    if (auto v = get("EqualizerBands")) parse_gains(*v, s.eq_gains);
    for (const std::string& val : eq_preset_lines) {   // "<ten gains>|<name>"
        const size_t bar = val.find('|');
        if (bar == std::string::npos) continue;
        const std::string name = trim_ws(val.substr(bar + 1));
        if (name.empty() || eq_name_reserved(name) || s.eq_custom_presets.size() >= kEqMaxCustomPresets) continue;
        bool dup = false;
        for (const auto& cp : s.eq_custom_presets) if (eq_name_equal(cp.name, name)) { dup = true; break; }
        EqCustomPreset cp;
        if (dup || !parse_gains(val.substr(0, bar), cp.gains)) continue;
        cp.name = name.substr(0, kEqNameMaxBytes);
        s.eq_custom_presets.push_back(std::move(cp));
    }
    if (auto v = get("GradientWaveSpeed")) if (const auto n_ = muisc::parse_double(*v)) { s.gradient_wave_speed = std::clamp(*n_, 0.01, 1.0); }
    if (auto v = get("PulseWaveSpeed")) if (const auto n_ = muisc::parse_double(*v)) { s.pulse_wave_speed = std::clamp(*n_, 0.01, 1.0); }
    if (auto v = get("PlaybackMode")) s.playback_shuffle = ascii_lower_str(*v) != "list";
    s.start_mode = player_start_mode();
    load_player_mode_settings(s);
    s.black_background = player_black_background();
    if (auto v = get("Visualizer")) s.element_visualizer = as_bool(*v, s.element_visualizer);
    if (auto v = get("VisualizerFluidity")) s.visualizer_fluidity = as_int(*v, s.visualizer_fluidity, 1, 10);
    if (auto v = get("VisualizerDegradationSpeed")) s.visualizer_degradation_speed = as_int(*v, s.visualizer_degradation_speed, 1, 10);
    if (auto v = get("VisualizerViscosity")) s.visualizer_viscosity = as_int(*v, s.visualizer_viscosity, 0, 10);
    if (auto v = get("VisualizerStyle")) s.bars_style = ascii_lower_str(*v) == "hifi" ? 1 : 0;
    if (auto v = get("HifiResolution")) s.hifi_resolution = ascii_lower_str(*v) == "rough" ? 1 : 0;
    if (auto v = get("HifiChannels")) s.hifi_channels = ascii_lower_str(*v) == "mono" ? 1 : 0;
    if (auto v = get("HifiScale")) { const std::string l = ascii_lower_str(*v); s.hifi_scale = l == "full" ? 0 : l == "low" ? 2 : l == "very low" ? 3 : 1; }
    if (auto v = get("HifiFluidity")) s.hifi_fluidity = as_int(*v, s.hifi_fluidity, 1, 10);
    if (auto v = get("HifiDegradationSpeed")) s.hifi_degradation = as_int(*v, s.hifi_degradation, 1, 10);
    if (auto v = get("HifiViscosity")) s.hifi_viscosity = as_int(*v, s.hifi_viscosity, 0, 10);
    if (auto v = get("HifiAdapt")) s.hifi_adapt = ascii_lower_str(*v) == "on" ? 1 : 0;
    if (auto v = get("HifiFrequencyScale")) { const std::string l = ascii_lower_str(*v); s.hifi_fscale = l == "linear" ? 0 : (l == "logarithmic" || l == "log") ? 1 : l == "bark" ? 3 : l == "erb" ? 4 : 2; }
    if (auto v = get("HifiGain")) s.hifi_gain = as_int(*v, s.hifi_gain, -20, 20);
    if (auto v = get("HifiRange")) s.hifi_range = as_int(*v, s.hifi_range, 40, 100);
    if (auto v = get("HifiSplit")) s.hifi_split = ascii_lower_str(*v) == "remove" ? 0 : 1;
    if (auto v = get("HifiColor")) s.hifi_color = ascii_lower_str(*v) == "corresponding" ? 1 : 0;
    // The oscilloscope settings: the player's reader (viz_prefs.h). An older "FrameRate" / "OsciMusicMode" (one value for both styles)
    // gives both styles that value and the per-style keys that follow override it.
    if (auto v = get("OsciBraille")) s.osci_style = as_bool(*v, true) ? 0 : 1;   // the old name: false meant "not braille"
    for (const auto& [k, v] : kv) osci_config_key(s, k, v);
    for (const std::string& k : scope_metric_keys()) if (auto v = get(k.c_str())) scope_metric_parse(s.scope_metrics, k, *v);
    if (auto v = get("TemperatureColor")) if (as_bool(*v, false)) s.osci_set[0].palette = s.osci_set[1].palette = 2;   // the old on/off option
    auto glyph = [&](const char* key, std::string& field) { if (auto v = get(key)) if (!v->empty()) field = *v; };
    glyph("BoxUpperLeft", s.box_upper_left);   glyph("BoxUpperRight", s.box_upper_right);
    glyph("BoxLowerLeft", s.box_lower_left);   glyph("BoxLowerRight", s.box_lower_right);
    glyph("BoxVertical", s.box_vertical);      glyph("BoxHorizontal", s.box_horizontal);
    glyph("ListSeparator", s.list_separator);
    // Share Profiles: the radio's own values go to the backup (own_viz), the player's take their place
    s.share_profiles = false;
    if (player_share_profiles()) radio_share_profiles(s, true);
    {   // Remote App: the player's config.txt holds it
        const RemoteAppConfig rc = read_remote_app();
        s.remote_app = rc.enabled; s.remote_port = rc.port; s.remote_pin = rc.pin;
    }
    // A radio_config.txt from before "ColorCodes=palette": 30-37 / 90-97 (text) and 40-47 / 100-107 (background) were plain 16-colour
    // codes. They become the palette numbers that look the same; the file is written once so that it carries the marker from now on.
    const auto cc = kv.find("colorcodes");
    if (cc == kv.end() || ascii_lower_str(cc->second) != "palette") {
        for (const ThemeKey& k : kThemeKeys) s.*(k.p) = themes::legacy_to_palette(s.*(k.p), themes::role_of_key(k.key));
        save_radio_settings(s);
    }
    return s;
}

namespace {
std::string eq_bands_text(const EqGains& g) {
    std::ostringstream o;
    for (int b = 0; b < kEqBands; ++b) o << (b ? "," : "") << g[static_cast<size_t>(b)];
    return o.str();
}
std::string eq_presets_text(const RadioSettings& s) {
    std::ostringstream o;
    for (const auto& cp : s.eq_custom_presets) o << "EqualizerPreset=" << eq_bands_text(cp.gains) << "|" << cp.name << "\n";
    o << "# EqualizerPreset=<ten gains>|<name> is one custom preset (made in the overlay with S), up to " << kEqMaxCustomPresets << "\n";
    return o.str();
}
} // namespace

// The whole radio_config.txt as text (also how the settings screen sees whether anything changed).
static void write_radio_file(std::ostream& o, const RadioSettings& s) {
    o << "# Mousiki radio mode -- settings (separate from the music player's config.txt)\n"
      << config_stamp_line() << "  (the first save by a newer version keeps the previous file as radio_config.txt.bak)\n"
         "# Location (Windows): %USERPROFILE%\\.config\\mousiki\\radio_config.txt\n"
         "# Location (Linux/macOS): $HOME/.config/mousiki/radio_config.txt  (or ./radio_config.txt next to the program)\n"
         "# Written by the settings screen (press s in the radio); edit by hand if you like.\n"
         "#\n"
         "# Colours are numbers of the 256-colour terminal palette (1-255) in every field. 0 or empty = no colour (the terminal's own).\n"
         "# (A file without the next line is an older one: its 30-37 / 90-97 / 40-47 / 100-107 are converted once.)\n"
         "ColorCodes=palette\n\n"
         "##-------------------------------------------\n"
         "##              COLORS\n"
         "##-------------------------------------------\n\n"
         "# Frame lines: fade from top to bottom\n"
         "ColorBorderTop=" << s.border_top << "\nColorBorderBottom=" << s.border_bottom << "\n"
         "# BorderGradient: smooth = the frame fades line by line from the top to the bottom colour, rough = only the bottom border line has the bottom colour\n"
         "BorderGradient=" << (s.border_smooth ? "smooth" : "rough") << "\n\n"
         "# ON AIR sign: diagonal gradient (upper left -> bottom right); also the needle, the lamp and the LIVE text\n"
         "ColorOnAirUpperLeft=" << s.on_air_upper_left << "\nColorOnAirBottomRight=" << s.on_air_bottom_right << "\n\n"
         "# Station info: the labels (Station, Title ...) and their values\n"
         "ColorMetadataKey=" << s.meta_key << "\nColorMetadataVal=" << s.meta_val << "\n\n"
         "# FFT spectrum under the station info (ColorVizCenter = optional middle stop of the gradient)\n"
         "ColorVizLeft=" << s.viz_left << "\nColorVizCenter=" << s.viz_center << "\nColorVizRight=" << s.viz_right << "\n\n"
         "# Oscilloscope (top right)\n"
         "ColorOsciLeft=" << s.osci_left << "\nColorOsciRight=" << s.osci_right << "\n\n"
         "# Frequency band: the line + ticks / the numbers and the small dots\n"
         "ColorFrequencyLine=" << s.freq_line << "\nColorFrequencyMHz=" << s.freq_mhz << "\n\n"
         "# Volume bar: the # / the -\n"
         "ColorVolumeCurrent=" << s.volume_current << "\nColorVolumePossible=" << s.volume_possible << "\n\n"
         "# Search bar: typed query (highlighted behind the query text only) and the idle bar\n"
         "# (empty FG = same as the STATIONS list rows; empty BG = none)\n"
         "ColorSearchFg=" << s.search_fg << "\nColorSearchBg=" << s.search_bg << "\n"
         "ColorSearchInactiveFg=" << s.search_inactive_fg << "\nColorSearchInactiveBg=" << s.search_inactive_bg << "\n\n"
         "# STATIONS list rows (the station names in the PRESETS pane use the same colours)\n"
         "ColorListInactiveFg=" << s.list_fg << "\nColorListInactiveBg=" << s.list_bg << "\n"
         "ColorListPlayingFg=" << s.list_playing_fg << "\nColorListPlayingBg=" << s.list_playing_bg << "\n"
         "ColorListCursorFg=" << s.list_cursor_fg << "\nColorListCursorBg=" << s.list_cursor_bg << "\n\n"
         "# PRESETS pane: key slots without a station / the short-cut key letters\n"
         "ColorPresetInactiveFg=" << s.preset_inactive_fg << "\nColorPresetInactiveBg=" << s.preset_inactive_bg << "\n"
         "ColorPresetKeyFg=" << s.preset_key_fg << "\n\n"
         "# Secondary text: captions (off air, no signal), the ON AIR sign while nothing is live, empty-pane\n"
         "# messages, status lines, dimmed entries\n"
         "ColorHeader=" << s.header << "\n\n"
         "# Key command legends: the hint lines of the menus ([TAB] Switch pane | ...), their errors, the cheatsheet titles\n"
         "ColorLegend=" << s.legend << "\n"
         "# ... and the descriptions behind those keys (\"Close\" in \"[ESC] Close\")\n"
         "ColorLegendCommand=" << s.legend_cmd << "\n\n"
         "# Settings screen: the current tab name (and the < \u2194 > hint) / the other tab names\n"
         "ColorTabCurrent=" << s.tab_current << "\nColorTabOther=" << s.tab_other << "\n\n"
         "# Pane focus indicator: the little triangle after the title of the pane that has the keys\n"
         "ColorFocus=" << s.focus << "\n\n"
         "##-------------------------------------------\n"
         "##              AUDIO\n"
         "##-------------------------------------------\n\n"
         "OnAirAscii=" << (s.on_air_ascii ? "true" : "false") << "\n"
         "PulseWave=" << (s.pulse_wave ? "true" : "false") << "\n"
         "DummyButtons=" << (s.dummy_buttons ? "true" : "false") << "\n"
         "# what the VOLUME BAR is drawn with: # | \xE2\xA0\xB6 | \xE2\xA3\xBF | \xE2\xA1\xBE\xE2\xA3\xBD (a fixed row, not repeating) | disk (the braille effect of the disk)\n"
         "VolumeGradient=" << (s.volume_gradient ? "true" : "false") << "\n"
         "VolumeIndicator=" << muisc::kVolIndicatorNames[std::clamp(s.volume_indicator, 0, muisc::kVolStyleCount - 1)] << "\n"
         "# right of the station info: osci | sphere | off | spectro\n"
         "OsciSphere=" << scope_mode_name(s) << "\n"
         "Visualizer=" << (s.element_visualizer ? "true" : "false") << "\n"
         "StereoSound=" << (s.stereo ? "true" : "false") << "\n"
         "# false = left out entirely: not in the \".\" cycle of the scope block (the sphere always is); the windows\n"
         "# (SHIFT+8 spectrogram, SHIFT+9 oscilloscope) work either way\n"
         "UseOscilloscope=" << (s.use_osci ? "true" : "false") << "\n"
         "UseSpectrogram=" << (s.use_spectro ? "true" : "false") << "\n"
         "# static that fades in when another station is tuned and fades out when the stream plays\n"
         "TuneNoise=" << (s.tune_noise ? "true" : "false") << "\n"
         "# a station name that is too long for its column scrolls; false = it is cut off with \"...\" instead\n"
         "MarqueeScrolling=" << (s.marquee ? "true" : "false") << "\n"
         "# the help wizard (SHIFT+w) greeted you once already; false makes it open again on the next start\n"
         "WizardSeen=" << (s.wizard_seen ? "true" : "false") << "\n"
         "# timeshift buffer for pause / rewind / recording from the past: 5 | 15 | 30 | 45 | 60 minutes (on disk, 11.5 MB per minute)\n"
         "TimeshiftMinutes=" << timeshift_minutes(s) << "\n"
         "# loudness normalisation (SHIFT+v overlay, v toggles): target -40..0 LUFS, max boost 0..24 dB\n"
         "NormalizeVolume=" << (s.normalize ? "true" : "false") << "\n"
         "NormalizeTargetLufs=" << static_cast<int>(s.normalize_target_lufs) << "\n"
         "NormalizeMaxBoostDb=" << static_cast<int>(s.normalize_max_boost_db) << "\n"
         "# equaliser (SHIFT+e overlay): ten gains in dB (-12..12) for 31, 62, 125, 250, 500 Hz, 1, 2, 4, 8, 16 kHz\n"
         "EqualizerEnabled=" << (s.eq_enabled ? "true" : "false") << "\n"
         "EqualizerBands=" << eq_bands_text(s.eq_gains) << "\n"
         << eq_presets_text(s) <<
         "\n"
         "##-------------------------------------------\n"
         "##              ANIMA/VIZ\n"
         "##-------------------------------------------\n\n"
         "# speeds: 0.01 - 1.00 (colour wave of the ON AIR sign in cycles per second | pulse wave behind it in loops per second)\n"
         "GradientWaveSpeed=" << fmt_speed(s.gradient_wave_speed) << "\n"
         "PulseWaveSpeed=" << fmt_speed(s.pulse_wave_speed) << "\n"
         "# station surf mode (key m): list | shuffle\n"
         "PlaybackMode=" << (s.playback_shuffle ? "shuffle" : "list") << "\n\n"
         "##-------------------------------------------\n"
         "##              PATHS (empty = default)\n"
         "##-------------------------------------------\n\n"
         "# StationListsPath: <path>/stations/stations.txt and <path>/station_lists/stationlists.txt (empty: the files sit in ~/.config/mousiki)\n"
         "StationListsPath=" << s.stations_path << "\n"
         "# PresetsPath: <path>/presets/presets.txt\n"
         "PresetsPath=" << s.presets_path << "\n"
         "# Recordings (key y) and YouTube downloads: <DownloadPath>/radio_downloads, or the music player's download folder\n"
         "DownloadPath=" << s.download_path << "\n"
         "DownloadSameAsMusicPlayer=" << (s.download_same_as_player ? "true" : "false") << "\n"
         "# HistoryPath: <path>/radio_history/history_radio.txt (empty: ~/.config/mousiki)\n"
         "HistoryPath=" << s.history_path << "\n\n"
         "##-------------------------------------------\n"
         "##              SLEEP TIMER\n"
         "##-------------------------------------------\n\n"
         "# the sleep timer (SHIFT+z) fades the volume out before it stops the stream\n"
         "SleepFade=" << (s.sleep_fade ? "true" : "false") << "\n\n"
         "# the \"<\" menus overlay: false = the tree, true = the classic view with every category as a box (SHIFT+t switches)\n"
         "MenuIndexClassic=" << (s.menu_classic ? "true" : "false") << "\n\n"
         "##-------------------------------------------\n"
         "##              KEYS (REFERENCE tab): one character, or SPACE / TAB / BACKSPACE\n"
         "##-------------------------------------------\n\n";
    for (int i = 0; i < kKeyActionCount; ++i) o << "HKey" << kKeyActions[i].id << "=" << key_binding(s, i) << "\n";
    o << "\n"
         "##-------------------------------------------\n"
         "##              LOOK / TUNING (edit here)\n"
         "##-------------------------------------------\n\n"
         "# FFT: 1-10 (higher = smoother / slower rise) | 1-10 (higher = bars fall faster) | 0-10 (neighbour blending)\n"
         "VisualizerFluidity=" << s.visualizer_fluidity << "\n"
         "VisualizerDegradationSpeed=" << s.visualizer_degradation_speed << "\n"
         "VisualizerViscosity=" << s.visualizer_viscosity << "\n"
         "VisualizerStyle=" << (s.bars_style == 1 ? "hifi" : "classic") << "\n"
         "HifiResolution=" << (s.hifi_resolution == 1 ? "rough" : "fine") << "\n"
         "HifiChannels=" << (s.hifi_channels == 1 ? "mono" : "stereo") << "\n"
         "HifiScale=" << (s.hifi_scale == 0 ? "full" : s.hifi_scale == 2 ? "low" : s.hifi_scale == 3 ? "very low" : "medium") << "\n"
         "HifiFluidity=" << s.hifi_fluidity << "\n"
         "HifiDegradationSpeed=" << s.hifi_degradation << "\n"
         "HifiViscosity=" << s.hifi_viscosity << "\n"
         "HifiAdapt=" << (s.hifi_adapt == 1 ? "on" : "off") << "\n"
         "HifiFrequencyScale=" << (s.hifi_fscale == 0 ? "linear" : s.hifi_fscale == 1 ? "logarithmic" : s.hifi_fscale == 3 ? "bark" : s.hifi_fscale == 4 ? "erb" : "mel") << "\n"
         "HifiGain=" << s.hifi_gain << "\n"
         "HifiRange=" << s.hifi_range << "\n"
         "HifiSplit=" << (s.hifi_split == 0 ? "remove" : "add") << "\n"
         "HifiColor=" << (s.hifi_color == 1 ? "corresponding" : "gradient") << "\n"
         ;
    osci_config_write(o, s, "#");
    o <<
         "\n"

         "BoxUpperLeft=" << s.box_upper_left << "\nBoxUpperRight=" << s.box_upper_right << "\n"
         "BoxLowerLeft=" << s.box_lower_left << "\nBoxLowerRight=" << s.box_lower_right << "\n"
         "BoxVertical=" << s.box_vertical << "\nBoxHorizontal=" << s.box_horizontal << "\n"
         "ListSeparator=" << s.list_separator << "\n";
    spectro_config_write(o, s.spectro, "#");
    spectro_profiles_write(o, s, "#");
    o << "\n";
}

// The oscilloscope / spectrogram part as text (the grid colour belongs to the theme and is not shared).
static std::string viz_text(const VizPrefs& v) {
    VizPrefs t = v;
    t.scope_grid_color.clear();
    std::ostringstream o;
    osci_config_write(o, t, "#");
    spectro_config_write(o, t.spectro, "#");
    spectro_profiles_write(o, t, "#");
    return o.str();
}

// What radio_config.txt gets: with Share Profiles on, the radio's OWN oscilloscope / spectrogram settings (the backup), not the
// player's that are in use meanwhile.
static void write_radio_settings(std::ostream& o, const RadioSettings& s) {
    if (!s.share_profiles) { write_radio_file(o, s); return; }
    RadioSettings own = s;
    own.take_shared_from(s.own_viz);
    own.share_profiles = false;
    write_radio_file(o, own);
}

std::string radio_settings_text(const RadioSettings& s) {
    std::ostringstream o;
    write_radio_settings(o, s);
    if (s.share_profiles) o << "# shared with the player:\n" << viz_text(s);   // so that a change of the shared values counts as a change
    o << "# remote app: " << (s.remote_app ? "on " : "off ") << s.remote_port << " " << s.remote_pin << "\n";   // (lives in config.txt)
    return o.str();
}

bool save_radio_settings(const RadioSettings& s, std::string* err) {
    const fs::path p = home_config_path();
    // Written where it is read from: a ./radio_config.txt that is in use stays the one that is saved.
    const fs::path target = fs::exists(fs::path("radio_config.txt")) && !fs::exists(p) ? fs::path("radio_config.txt") : p;
    std::error_code ec;
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    backup_config_if_older(target);   // the first save by a newer version keeps the old file as radio_config.txt.bak
    std::ostringstream o;   // written in one go at the end (write_text_atomic: never a cut-off file)
    if (s.start_mode != player_start_mode()) set_player_start_mode(s.start_mode);   // the settings that live in the player's config.txt
    save_player_mode_settings(s);
    if (s.black_background != player_black_background()) set_player_black_background(s.black_background);
    if (s.share_profiles != player_share_profiles()) set_player_share_profiles(s.share_profiles);
    {
        const RemoteAppConfig now = read_remote_app();
        if (now.enabled != s.remote_app || now.port != s.remote_port || now.pin != s.remote_pin) {
            RemoteAppConfig rc{s.remote_app, s.remote_port, s.remote_pin};
            write_remote_app(rc);
        }
    }
    write_radio_settings(o, s);
    if (!write_text_atomic(target, o.str())) { if (err) *err = "cannot write " + path_utf8(target); return false; }
    if (s.share_profiles) {   // the radio's edits of the shared settings go to the player's config.txt
        VizPrefs now;
        if (read_player_viz(now) && viz_text(now) != viz_text(s)) write_player_viz(s);
    }
    return true;
}

} // namespace muisc::radio
