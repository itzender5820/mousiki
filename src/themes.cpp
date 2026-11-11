// Colour themes: the palettes that set every colour of the program, how a theme is applied and
// captured, and the separate radio keys of a theme.
#include "app_dirs.h"
#include "themes.h"
#include "utf8_util.h"
#include "string_util.h"
#include "path_utf8.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

using muisc::ascii_lower_str;
using muisc::trim_ws;

namespace fs = std::filesystem;

namespace themes {

const std::string* Theme::get(const std::string& key, bool radio) const {
    if (radio) {
        const std::string rk = "Radio." + key;
        for (const auto& p : kv) if (p.first == rk) return &p.second;
    }
    for (const auto& p : kv) if (p.first == key) return &p.second;
    return nullptr;
}

void Theme::set(const std::string& key, const std::string& value, bool radio_only) {
    const std::string k = radio_only ? "Radio." + key : key;
    for (auto& p : kv) if (p.first == k) { p.second = value; return; }
    kv.emplace_back(k, value);
}

namespace {

// One theme in a handful of roles; expand() turns it into every key of both programs.
struct Palette {
    const char* name;
    const char *b1, *b2;       // border top / bottom
    const char *d1, *d2;       // disk, visualizer, on-air sign, oscilloscope: the two ends of the gradient
    const char *mk, *mv;       // metadata key / value
    const char *pp, *pn;       // progress bar / volume: played / pending
    const char *lf, *pf, *cb;  // list text, playing row, cursor background
    const char *hd, *lg;       // header (accent) and legend (dim)
    const char *tc, *to;       // tab names: current / other
    const char *chf, *chb;     // text being edited
    const char *li, *ll, *lw;  // lyrics: inactive line, active line, active word
    const char *wb;            // lyrics: active word background
    const char *fc = nullptr;  // focus indicator (nullptr: the header colour)
};

// Colours are palette numbers (see legacy_to_palette()).
const Palette kPalettes[] = {
    {"Soft Pastel",          "183","152","218","153","183","230","218","146","253","152","238","183","246","218","244","236","218","245","224","230",""},
    {"Cyberpunk",            "201","51","201","51","51","227","201","54","195","227","53","201","63","51","135","236","227","63","51","201",""},
    {"Greyscale",            "255","240","255","238","250","255","255","240","252","246","238","255","244","255","243","236","252","243","250","255",""},
    {"Nord",                 "110","67","110","139","110","189","110","60","188","150","59","110","246","150","245","236","150","245","153","195",""},
    {"Dracula",              "141","212","141","212","117","231","212","60","253","84","60","141","246","212","246","236","84","246","117","228",""},
    {"Solarized Dark",       "136","73","136","73","73","187","64","240","248","73","236","136","240","166","240","236","136","240","245","230",""},
    {"Gruvbox",              "214","142","208","142","214","223","142","241","223","109","239","214","245","208","243","236","214","243","223","229",""},
    {"Monokai",              "197","81","197","81","148","230","148","59","253","186","237","197","242","148","242","236","148","242","186","230",""},
    {"Ocean",                "75","24","81","27","81","195","81","24","153","123","24","81","67","123","67","236","81","67","117","195",""},
    {"Forest",               "71","22","114","28","71","193","114","22","151","192","22","71","65","192","65","236","114","65","151","230",""},
    {"Sunset",               "209","125","214","162","209","223","214","242","224","216","52","209","138","214","242","236","214","138","223","230",""},
    {"Matrix",               "82","22","82","22","82","157","82","22","83","120","22","82","28","118","28","236","82","28","83","157",""},
    {"Amber Terminal",       "214","130","220","166","214","222","220","58","178","220","58","220","136","214","130","236","214","136","178","222",""},
    {"Sakura",               "211","175","218","169","211","255","218","132","225","218","89","211","139","218","139","236","218","139","224","255",""},
    {"Midnight",             "62","18","69","54","69","189","69","54","146","189","17","69","60","141","60","236","69","60","146","189",""},
    {"Colorblind Deuteranopia","214","75","214","75","75","229","214","67","255","220","24","214","246","220","244","236","214","244","117","229",""},
    {"Colorblind Protanopia","220","69","226","63","69","229","226","61","255","117","18","220","246","226","244","236","220","244","117","230",""},
    {"Colorblind Tritanopia","203","80","203","80","80","224","203","66","255","217","53","203","246","210","244","236","203","244","80","255",""},
    {"High Contrast",        "255","255","226","51","226","255","226","244","255","226","21","226","250","226","250","236","226","250","255","231",""},
    {"Coffee",               "137","58","173","242","180","223","173","242","187","180","58","180","243","180","243","236","180","243","187","230",""},
    // v4.2.0: ten more (dark-terminal themes; the last value is the focus colour)
    {"Catppuccin Mocha",    "183","111","218","116","111","189","218","239","146","218","237","183","243","183","243","236","183","243","189","218","","183"},
    {"Tokyo Night",         "111","141","141","117","111","189","117","238","146","215","236","111","60","111","60","236","111","60","189","117","","141"},
    {"Rose Pine",           "181","183","168","152","152","223","181","239","146","222","237","181","102","181","102","236","181","102","223","168","","183"},
    {"Everforest",          "108","109","180","108","108","187","180","238","145","180","236","108","243","108","243","236","108","243","187","180","","109"},
    {"Kanagawa",            "67","174","179","174","110","187","179","238","145","179","236","110","60","110","60","236","110","60","187","174","","67"},
    {"Vaporwave",           "218","123","218","141","123","225","218","60","183","123","54","218","97","218","97","236","123","97","225","218","","123"},
    {"Outrun",              "208","199","214","93","81","225","199","54","183","214","53","199","96","199","96","236","214","96","225","214","","208"},
    {"Ember",               "202","124","220","160","208","223","202","238","180","220","52","208","137","208","137","236","208","137","223","220","","202"},
    {"Glacier",             "195","67","255","74","117","255","153","238","152","195","24","153","66","153","66","236","153","66","255","117","","195"},
    {"Lavender Haze",       "183","147","219","141","147","225","183","239","189","219","60","183","103","183","103","236","183","103","225","219","","147"},
};

// `v8`: the theme as v4.2.0 first shipped it (some text colours empty = the terminal's own text colour, which differs
// from terminal to terminal); without it every text colour is a fixed palette number, so the theme looks the same
// in every terminal (Windows Terminal, CMD, GNOME, ... whatever their colour schemes say).
Theme expand(const Palette& p, bool v8 = false) {
    const char* text = v8 ? "" : p.lf;   // text that had no colour of its own: the theme's list text colour now
    Theme t;
    t.name = p.name;
    t.builtin = true;
    auto a = [&](const char* k, const char* v) { t.kv.emplace_back(k, v); };
    // --- keys both programs share ---
    a("ColorBorderTop", p.b1);            a("ColorBorderBottom", p.b2);
    a("ColorMetadataKey", p.mk);          a("ColorMetadataVal", p.mv);
    a("ColorVizLeft", p.d1);              a("ColorVizRight", p.d2);
    a("ColorTabCurrent", p.tc);           a("ColorTabOther", p.to);
    a("ColorHeader", p.hd);               a("ColorLegend", p.lg);
    a("ColorFocus", p.fc ? p.fc : p.hd);   // the pane focus indicator: the theme's header color unless the theme names its own
    a("Radio.ColorHeader", p.lg);         a("Radio.ColorLegend", p.hd);   // the radio draws them the other way round
    a("ColorLegendCommand", p.lf);        a("Radio.ColorLegendCommand", p.lf);   // the descriptions behind the legend keys: the theme's list text colour
    a("Radio.ColorFocus", p.lg);          // ... and its focus indicator follows ITS header colour
    a("ColorSearchFg", text);             a("ColorSearchBg", "");
    a("ColorSearchInactiveFg", text);     a("ColorSearchInactiveBg", "");
    a("ColorListInactiveFg", p.lf);       a("ColorListInactiveBg", "");
    a("ColorListPlayingFg", p.pf);        a("ColorListPlayingBg", "");
    a("ColorListCursorFg", text);         a("ColorListCursorBg", p.cb);
    // --- player only ---
    a("ColorDiskTop", p.d1);              a("ColorDiskBottom", p.d2);
    a("ColorProgressBarPlayed", p.pp);    a("ColorProgressBarPending", p.pn);
    a("ColorChangeFg", p.chf);            a("ColorChangeBg", p.chb);
    a("ColorQueueInactiveFg", p.lf);      a("ColorQueueInactiveBg", "");
    a("ColorQueuePlayingFg", p.pf);       a("ColorQueuePlayingBg", "");
    a("ColorQueueCursorFg", text);        a("ColorQueueCursorBg", p.cb);
    a("ColorLyricsInactiveFg", p.li);     a("ColorLyricsInactiveBg", "");
    a("ColorLyricsActiveLineFg", p.ll);   a("ColorLyricsActiveLineBg", "");
    a("ColorLyricsActiveWordFg", p.lw);   a("ColorLyricsActiveWordBg", p.wb);
    // --- radio only ---
    a("ColorOnAirUpperLeft", p.d1);       a("ColorOnAirBottomRight", p.d2);
    a("ColorVizCenter", "");
    a("ColorOsciLeft", p.d1);             a("ColorOsciRight", p.d2);
    a("ColorFrequencyLine", p.d1);        a("ColorFrequencyMHz", p.lg);
    a("ColorVolumeCurrent", p.pp);        a("ColorVolumePossible", p.pn);
    a("ColorPresetInactiveFg", p.lg);     a("ColorPresetInactiveBg", "");
    a("ColorPresetKeyFg", p.lf);
    return t;
}

// The two hand-made themes (not expressible as a Palette: they also colour the search bar, the playing row and so on).
// "SeriousMeowerShell(Default)" is the music player's default and list index 0 (the base/fallback theme everywhere);
// "MeowerShell" is the cyber-cat original, index 1.
// The two hand-made themes were made in a terminal whose own colour scheme turns some of the 16 base colours
// into other colours (10 is yellow there, 8 and 1 pink) and whose default text colour is light blue. In another
// terminal (CMD, a GNOME terminal ...) the same numbers look different: 10 is green, 8 grey, the default text grey.
// Since v4.2.0 they only use the 240 fixed colours (16-255), which look the same in every terminal: the colours they
// were made with. `v8` gives the old values (recognises a config.txt that still holds them).
std::string universal_color(const std::string& key, const std::string& v) {
    if (v == "10") return "227";   // yellow
    if (v == "8" || v == "1") return "199";   // pink
    if (v == "7") return "231";    // white
    if (v.empty() && key.size() > 2 && key.compare(key.size() - 2, 2, "Fg") == 0) return "75";   // the light blue default text
    return v;
}

Theme hand_made(const char* name, const char* const (*kv)[2], size_t n, bool v8) {
    Theme t;
    t.name = name;
    t.builtin = true;
    for (size_t i = 0; i < n; ++i) t.kv.emplace_back(kv[i][0], v8 ? std::string(kv[i][1]) : universal_color(kv[i][0], kv[i][1]));
    return t;
}

Theme serious_meowershell(bool v8 = false) {
    static const char* const kv[][2] = {
        {"ColorBorderTop","170"},{"ColorBorderBottom","104"},{"ColorMetadataKey","140"},{"ColorMetadataVal","182"},
        {"ColorVizLeft","170"},{"ColorVizRight","30"},{"ColorTabCurrent","8"},{"ColorTabOther","10"},{"ColorHeader","10"},
        {"ColorFocus","183"},{"ColorLegend","104"},{"ColorLegendCommand","170"},{"Radio.ColorLegendCommand","170"},{"Radio.ColorHeader","8"},{"Radio.ColorLegend","104"},
        {"Radio.ColorFocus","8"},{"ColorSearchFg","7"},{"ColorSearchBg","1"},{"ColorSearchInactiveFg",""},
        {"ColorSearchInactiveBg",""},{"ColorListInactiveFg",""},{"ColorListInactiveBg",""},{"ColorListPlayingFg","16"},
        {"ColorListPlayingBg","182"},{"ColorListCursorFg","10"},{"ColorListCursorBg","238"},{"ColorDiskTop","170"},
        {"ColorDiskBottom","30"},{"ColorProgressBarPlayed","213"},{"ColorProgressBarPending","182"},{"ColorChangeFg","7"},
        {"ColorChangeBg","1"},{"ColorQueueInactiveFg",""},{"ColorQueueInactiveBg",""},{"ColorQueuePlayingFg","16"},
        {"ColorQueuePlayingBg","182"},{"ColorQueueCursorFg",""},{"ColorQueueCursorBg","240"},{"ColorLyricsInactiveFg",""},
        {"ColorLyricsInactiveBg",""},{"ColorLyricsActiveLineFg","182"},{"ColorLyricsActiveLineBg",""},
        {"ColorLyricsActiveWordFg",""},{"ColorLyricsActiveWordBg",""},{"ColorOnAirUpperLeft","170"},
        {"ColorOnAirBottomRight","30"},{"ColorVizCenter",""},{"ColorOsciLeft","170"},{"ColorOsciRight","30"},
        {"ColorFrequencyLine","170"},{"ColorFrequencyMHz","8"},{"ColorVolumeCurrent","170"},{"ColorVolumePossible","182"},
        {"ColorPresetInactiveFg","8"},{"ColorPresetInactiveBg",""},{"ColorPresetKeyFg","255"},
    };
    return hand_made("SeriousMeowerShell(Default)", kv, sizeof kv / sizeof kv[0], v8);
}

Theme meowershell(bool v8 = false) {
    static const char* const kv[][2] = {
        {"ColorBorderTop","170"},{"ColorBorderBottom","182"},{"ColorMetadataKey","170"},{"ColorMetadataVal","182"},
        {"ColorVizLeft","170"},{"ColorVizRight","30"},{"ColorTabCurrent","8"},{"ColorTabOther","10"},{"ColorHeader","10"},
        {"ColorFocus","183"},{"ColorLegend","182"},{"ColorLegendCommand","170"},{"Radio.ColorLegendCommand","170"},{"Radio.ColorHeader","8"},{"Radio.ColorLegend","182"},
        {"Radio.ColorFocus","8"},{"ColorSearchFg","7"},{"ColorSearchBg","1"},{"ColorSearchInactiveFg",""},
        {"ColorSearchInactiveBg",""},{"ColorListInactiveFg",""},{"ColorListInactiveBg",""},{"ColorListPlayingFg","16"},
        {"ColorListPlayingBg","182"},{"ColorListCursorFg","10"},{"ColorListCursorBg","238"},{"ColorDiskTop","170"},
        {"ColorDiskBottom","30"},{"ColorProgressBarPlayed","170"},{"ColorProgressBarPending","182"},{"ColorChangeFg","7"},
        {"ColorChangeBg","1"},{"ColorQueueInactiveFg",""},{"ColorQueueInactiveBg",""},{"ColorQueuePlayingFg","16"},
        {"ColorQueuePlayingBg","182"},{"ColorQueueCursorFg",""},{"ColorQueueCursorBg","240"},{"ColorLyricsInactiveFg",""},
        {"ColorLyricsInactiveBg",""},{"ColorLyricsActiveLineFg","182"},{"ColorLyricsActiveLineBg",""},
        {"ColorLyricsActiveWordFg",""},{"ColorLyricsActiveWordBg",""},{"ColorOnAirUpperLeft","170"},
        {"ColorOnAirBottomRight","30"},{"ColorVizCenter",""},{"ColorOsciLeft","170"},{"ColorOsciRight","30"},
        {"ColorFrequencyLine","170"},{"ColorFrequencyMHz","8"},{"ColorVolumeCurrent","170"},{"ColorVolumePossible","182"},
        {"ColorPresetInactiveFg","8"},{"ColorPresetInactiveBg",""},{"ColorPresetKeyFg","255"},
    };
    return hand_made("MeowerShell", kv, sizeof kv / sizeof kv[0], v8);
}



void write_theme(std::ostream& out, const Theme& t) {
    out << "[" << t.name << "]\n";
    for (const auto& p : t.kv) out << p.first << "=" << p.second << "\n";
    out << "\n";
}

const char* kHeader =
    "# format: 10\n"
    "# Colours are numbers of the 256-colour terminal palette (1-255, 0 or empty = none).\n"
    "# Mousiki colour themes (shared by the player and the radio).\n"
    "# [Name] starts a theme; the lines below it use the key names of config.txt / radio_config.txt.\n"
    "# A key a theme leaves out keeps whatever colour is set. Keys starting with \"Radio.\" are the radio's own values (colours made in the radio are stored that way, so the radio and the player can look different in one theme).\n"
    "# Built-in themes can be edited here; new ones are made with CREATE THEME on the Colors tab.\n\n";

std::vector<Theme> parse_file(const fs::path& p) {
    std::vector<Theme> out;
    std::ifstream in(p);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        line = trim_ws(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            Theme t;
            t.name = trim_ws(line.substr(1, line.size() - 2));
            if (t.name.empty()) t.name = "(unnamed)";
            out.push_back(t);
            continue;
        }
        if (out.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.back().set(trim_ws(line.substr(0, eq)), trim_ws(line.substr(eq + 1)));
    }
    return out;
}

// The number in a "# format: N" first line (0: no such line -- a hand-made file).
int file_format(const fs::path& p) {
    std::ifstream in(p);
    std::string first;
    std::getline(in, first);
    first = trim_ws(first);
    const std::string tag = "# format:";
    if (first.compare(0, tag.size(), tag) != 0) return 0;
    try { return std::stoi(trim_ws(first.substr(tag.size()))); } catch (...) { return 0; }
}

// Themes from before format 6 used 30-37 / 90-97 / 40-47 / 100-107 as plain 16-colour codes.
void convert_legacy(Theme& t) {
    for (auto& p : t.kv) p.second = legacy_to_palette(p.second, role_of_key(p.first));
}

bool is_builtin_name(const std::string& n) {
    if (ascii_lower_str(n) == ascii_lower_str(serious_meowershell().name) || ascii_lower_str(n) == ascii_lower_str(meowershell().name)) return true;
    // names of earlier versions: the built-ins were renamed, so a copy in an old themes.txt is not a theme of the user's
    if (ascii_lower_str(n) == "meowershell(default)" || ascii_lower_str(n) == "serious meowershell") return true;
    for (const Palette& p : kPalettes) if (ascii_lower_str(p.name) == ascii_lower_str(n)) return true;
    return false;
}

} // namespace

int find_name(const std::vector<Theme>& all, const std::string& name) {
    const std::string n = ascii_lower_str(sanitize_name(name));
    for (size_t i = 0; i < all.size(); ++i) if (ascii_lower_str(all[i].name) == n) return static_cast<int>(i);
    return -1;
}

ColorRole role_of_key(const std::string& key_in) {
    std::string k = key_in;
    if (k.compare(0, 6, "Radio.") == 0) k = k.substr(6);
    static const char* const gradient[] = {"ColorDiskTop", "ColorDiskBottom", "ColorVizLeft", "ColorVizCenter", "ColorVizRight",
                                           "ColorOsciLeft", "ColorOsciRight", "ColorOnAirUpperLeft", "ColorOnAirBottomRight"};
    for (const char* g : gradient) if (k == g) return ColorRole::Gradient;
    if (k.size() > 2 && k.compare(k.size() - 2, 2, "Bg") == 0) return ColorRole::Background;
    return ColorRole::Text;
}

std::string legacy_to_palette(const std::string& value, ColorRole role) {
    if (role == ColorRole::Gradient || value.empty()) return value;
    for (unsigned char c : value) if (!std::isdigit(c)) return value;
    if (value.size() > 3) return value;
    const int v = std::stoi(value);
    if (role == ColorRole::Text) {
        if (v == 30) return "16";
        if (v >= 31 && v <= 37) return std::to_string(v - 30);
        if (v >= 90 && v <= 97) return std::to_string(v - 82);
    } else {
        if (v == 40) return "16";
        if (v >= 41 && v <= 47) return std::to_string(v - 40);
        if (v >= 100 && v <= 107) return std::to_string(v - 92);
    }
    return value;
}

fs::path file_path() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = std::getenv("USERPROFILE");
    fs::path base = (home && *home) ? fs::u8path(home) : fs::path(".");
    return muisc::app_dirs::config() / "themes.txt";
}

std::string sanitize_name(const std::string& raw) {
    std::string s = trim_ws(raw);
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '[' || c == ']' || c == '\n' || c == '\r'; }), s.end());
    s = trim_ws(s);
    // Cut at 25 characters without splitting a UTF-8 sequence.
    int chars = 0;
    size_t i = 0;
    while (i < s.size() && chars < kMaxNameLen) {
        i++;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i++;
        chars++;
    }
    return s.substr(0, i);
}

std::vector<Theme> load_all() {
    std::vector<Theme> all;
    all.push_back(serious_meowershell());   // list index 0: the base/fallback theme everywhere
    all.push_back(meowershell());
    for (const Palette& p : kPalettes) all.push_back(expand(p));

    const fs::path path = file_path();
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path);
        if (out) {
            out << kHeader;
            for (const Theme& t : all) write_theme(out, t);
        }
        return all;
    }
    // A themes.txt written by an older version (no "# format: 3" line) is rewritten with the current built-ins; the
    // user's own themes are kept. Dropped on the way: the old built-in "Default" (now "Serious MeowerShell") and
    // any copy of a built-in theme that carries identical values (e.g. a MeowerShell the user saved by hand).
    {
        std::ifstream in(path);
        std::string first;
        std::getline(in, first);
        if (trim_ws(first) != "# format: 10") {   // format 10: the built-ins have a visible text colour for the edited text (236); format 9: fixed colours only; format 8: ten more built-in themes
            const int old_format = file_format(path);
            std::vector<Theme> mine;
            for (Theme& ft : parse_file(path)) {
                if (old_format == 7 && is_builtin_name(ft.name)) {   // format 7 already had today's keys: keep edits made to a built-in theme
                    for (Theme& t : all) if (ascii_lower_str(t.name) == ascii_lower_str(ft.name)) for (const auto& kvp : ft.kv) t.set(kvp.first, kvp.second);
                    continue;
                }
                if (is_builtin_name(ft.name) || ascii_lower_str(ft.name) == "default") continue;
                // format 6: every colour number is a palette number (30-37 / 90-97 / 40-47 / 100-107 were plain 16-colour codes before)
                if (old_format > 0 && old_format < 6) convert_legacy(ft);
                // format 7: the built-ins were renamed (SeriousMeowerShell(Default) is index 0 now, MeowerShell the second theme)
                // format 5: the new MeowerShell (default) and Serious MeowerShell values; format 4 added ColorFocus (the pane focus indicator): a theme of your own gets its header color
                if (!ft.get("ColorFocus", false)) if (const std::string* hd = ft.get("ColorHeader", false)) ft.set("ColorFocus", *hd);
                bool dup = false;
                for (const Theme& b : all) {
                    if (b.kv.size() != ft.kv.size()) continue;
                    bool same = true;
                    for (const auto& kvp : ft.kv) { const std::string* v = b.get(kvp.first, false); if (!v || *v != kvp.second) { same = false; break; } }
                    if (same) { dup = true; break; }
                }
                if (!dup) mine.push_back(ft);
            }
            std::ostringstream buf;
            buf << kHeader;
            for (const Theme& t : all) write_theme(buf, t);
            for (const Theme& t : mine) write_theme(buf, t);
            { std::ofstream out(path, std::ios::trunc); if (out) out << buf.str(); }
            for (Theme& t : mine) all.push_back(t);
            return all;
        }
    }
    for (Theme& ft : parse_file(path)) {
        bool merged = false;
        for (Theme& t : all) {
            if (ascii_lower_str(t.name) != ascii_lower_str(ft.name)) continue;
            if (t.builtin) for (const auto& p : ft.kv) t.set(p.first, p.second);   // the file's edits win
            merged = true;
            break;
        }
        if (!merged) all.push_back(ft);
    }
    return all;
}

std::vector<std::pair<Theme, Theme>> renewed_builtins() {
    std::vector<std::pair<Theme, Theme>> out;
    out.emplace_back(serious_meowershell(true), serious_meowershell());
    out.emplace_back(meowershell(true), meowershell());
    for (const Palette& p : kPalettes) out.emplace_back(expand(p, true), expand(p));
    return out;
}

bool file_holds(const fs::path& config, const Theme& th, bool radio) {
    std::ifstream in(config);
    if (!in) return false;
    std::vector<std::pair<std::string, std::string>> raw;
    std::string line;
    while (std::getline(in, line)) {
        line = trim_ws(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        raw.emplace_back(ascii_lower_str(trim_ws(line.substr(0, eq))), trim_ws(line.substr(eq + 1)));
    }
    int compared = 0;
    for (const auto& kvp : th.kv) {
        if (kvp.first.compare(0, 6, "Radio.") == 0) continue;
        const std::string* want = th.get(kvp.first, radio);
        const std::string k = ascii_lower_str(kvp.first);
        for (const auto& [rk, rv] : raw) {
            if (rk != k) continue;
            if (rv != *want) return false;
            ++compared;
            break;
        }
    }
    return compared >= 10;
}

std::filesystem::path exchange_dir() { return file_path().parent_path(); }

std::string file_name_for(const std::string& theme_name) {
    std::string n = sanitize_name(theme_name);
    for (char& c : n) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    if (n.empty()) n = "theme";
    return n + ".theme";
}

bool write_theme_file(const fs::path& file, const Theme& t, std::string& err) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    if (!out) { err = "cannot write " + muisc::path_utf8(file); return false; }
    out << "# format: 9\n# A Mousiki colour theme. Import it with SHIFT+i on the Colors tab of the settings.\n\n";
    write_theme(out, t);
    return true;
}

std::vector<Theme> read_theme_file(const fs::path& file) {
    std::vector<Theme> out;
    const int format = file_format(file);
    for (Theme& t : parse_file(file)) {
        if (format > 0 && format < 6) convert_legacy(t);   // an old export: its 90 / 37 / 41 were plain 16-colour codes
        bool has_color = false;
        for (const auto& p : t.kv) if (p.first.find("Color") != std::string::npos) { has_color = true; break; }
        if (has_color) out.push_back(t);
    }
    return out;
}

std::vector<fs::path> list_theme_files(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = ascii_lower_str(muisc::path_utf8(it->path().extension()));
        if (ext != ".theme" && ext != ".txt") continue;
        if (it->path() == file_path()) continue;   // the theme store itself
        if (!read_theme_file(it->path()).empty()) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return ascii_lower_str(muisc::path_utf8(a.filename())) < ascii_lower_str(muisc::path_utf8(b.filename())); });
    return out;
}

std::string import_theme(Theme t, std::string& err) {
    t.builtin = false;
    std::string base = sanitize_name(t.name);
    if (base.empty()) base = "Imported";
    const std::vector<Theme> all = load_all();
    auto taken = [&](const std::string& n) {
        for (const Theme& x : all) if (ascii_lower_str(x.name) == ascii_lower_str(n)) return true;
        return false;
    };
    std::string name = base;
    for (int i = 2; taken(name) && i < 1000; ++i) {
        const std::string suffix = " (" + std::to_string(i) + ")";
        name = sanitize_name(base.substr(0, static_cast<size_t>(std::max(1, kMaxNameLen - static_cast<int>(suffix.size()))))) + suffix;
    }
    t.name = name;
    return save_theme(t, err) ? name : std::string();
}

bool save_theme(const Theme& in_theme, std::string& err) {
    Theme t = in_theme;
    t.name = sanitize_name(t.name);
    t.builtin = false;
    if (t.name.empty()) { err = "give the theme a name first"; return false; }
    if (is_builtin_name(t.name)) { err = "\"" + t.name + "\" is a built-in theme -- pick another name"; return false; }

    const fs::path path = file_path();
    load_all();   // an old themes.txt is converted and rewritten first, so what is read below is current
    std::vector<Theme> existing;
    {
        std::error_code ec;
        if (fs::exists(path, ec)) existing = parse_file(path);
    }
    bool replaced = false;
    for (Theme& e : existing) if (ascii_lower_str(e.name) == ascii_lower_str(t.name)) { e = t; replaced = true; }
    if (!replaced) existing.push_back(t);

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Rewrite the whole file: the header, then the built-ins (as they are now, with any edits), then the user's themes.
    std::vector<Theme> all = load_all();
    std::ostringstream buf;
    buf << kHeader;
    for (const Theme& b : all) if (b.builtin) write_theme(buf, b);
    for (const Theme& e : existing) if (!is_builtin_name(e.name)) write_theme(buf, e);
    std::ofstream out(path, std::ios::trunc);
    if (!out) { err = "cannot write " + muisc::path_utf8(path); return false; }
    out << buf.str();
    return true;
}

bool delete_theme(const std::string& name_in, std::string& err) {
    const std::string name = sanitize_name(name_in);
    if (name.empty()) { err = "no theme chosen"; return false; }
    if (is_builtin_name(name)) { err = "\"" + name + "\" is a built-in theme and cannot be deleted"; return false; }
    const fs::path path = file_path();
    load_all();   // an old themes.txt is converted and rewritten first
    std::vector<Theme> existing;
    std::error_code ec;
    if (fs::exists(path, ec)) existing = parse_file(path);
    bool found = false;
    std::vector<Theme> keep;
    for (Theme& e : existing) {
        if (ascii_lower_str(e.name) == ascii_lower_str(name)) { found = true; continue; }
        keep.push_back(e);
    }
    if (!found) { err = "\"" + name + "\" is not in " + muisc::path_utf8(path); return false; }
    const std::vector<Theme> all = load_all();
    std::ostringstream buf;
    buf << kHeader;
    for (const Theme& b : all) if (b.builtin) write_theme(buf, b);
    for (const Theme& e : keep) if (!is_builtin_name(e.name)) write_theme(buf, e);
    std::ofstream out(path, std::ios::trunc);
    if (!out) { err = "cannot write " + muisc::path_utf8(path); return false; }
    out << buf.str();
    return true;
}

} // namespace themes
