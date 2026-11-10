#pragma once
#include <memory>
#include <thread>
// Helpers that app.cpp and the files split out of it (app_meta.cpp, app_history.cpp, app_readlyrics.cpp, app_library.cpp, app_playback.cpp,
// app_panels.cpp, app_playlist.cpp, app_keys.cpp, app_render.cpp, ... -- every app_*.cpp is a part of class App) both use.
// Only for those files: nothing here is part of the program's interface. The functions are defined in app.cpp
// (n_files_label in app_history.cpp).
#include <algorithm>
#include "utf8_util.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include "console_log.h"
#include "path_utf8.h"
#include "lyrics_fetcher.h"
#include "settings.h"
#include "themes.h"
#include "terminal_ui.h"
#include "text_edit.h"
#include "oscilloscope_visualizer.h"
#include "meta_editor.h"
#include "metadata_probe.h"

namespace muisc {

// The sound of a video file as an mp3 (ffmpeg) in `dir` (src/app_videosearch.cpp); "" and `err` set when it fails.
std::string video_make_mp3_file(const std::string& video, const std::filesystem::path& dir, const std::string& base, const std::string& title, std::string& err);


// When a file arrived in the library (later of modification and creation / change time); 0 when unreadable. app_history.cpp.
long long file_added_time(const fs::path& p);

// Worker-thread exception guard: whatever a background task throws is turned into a log line and that task alone fails
// (an exception that escapes a std::thread would call std::terminate() and end the whole program).
template <class F>
inline void run_guarded(const char* what, F&& body) noexcept {
    try {
        body();
    } catch (const std::exception& e) {
        ConsoleLog::instance().log_basic(std::string("internal error in ") + what + ": " + e.what());
    } catch (...) {
        ConsoleLog::instance().log_basic(std::string("internal error in ") + what + " (unknown exception)");
    }
}

// Removes exactly one full UTF-8 codepoint from the end of a text-entry buffer (not just its last byte).
inline void pop_utf8_char(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.erase(i);
}

// SGR for a "cursor" (hovering) row; reverse video when both colours are empty, so the cursor is never invisible.
inline std::string cursor_sgr(const std::string& fg, const std::string& bg) {
    std::string s = ansi_for(fg) + bg_ansi_for(bg);
    return s.empty() ? std::string("\x1b[7m") : s;
}

// ---- colours of the shared screen elements (defined in app.cpp) ----
std::string search_sgr(const Settings& s);        // the typed query in a search bar
std::string search_idle_sgr(const Settings& s);   // everything else in the bar
std::string search_run(const Settings& s, const std::string& text, bool nonempty);
std::string focus_sgr(const Settings& s);
std::string focus_indicator(const Settings& s, const std::string& border_ansi);
std::string strip_csi(const std::string& s);
std::string header_sgr(const Settings& s);
std::string legend_sgr(const Settings& s);
std::string legend_colored(const Settings& s, const std::string& text);
std::string paint_legend(const Settings& s, const std::vector<LegendRow>& rows, size_t i, const std::string& border, int pad_to, bool fade);
std::string change_sgr(const Settings& s);
// The tab strip of a menu overlay: two lines (the top border with the title and the tabs, the line below).
std::pair<std::string, std::string> menu_tab_strip(int W, const std::string& title, const std::vector<std::string>& labels,
                                                   int cur, const std::string& top_ansi, const std::string& bot_ansi,
                                                   const std::string& corner_ul, const std::string& corner_ur,
                                                   const std::string& vertical, int min_inner = 0,
                                                   const std::string& line2 = "", int line2_cols = 0,
                                                   const std::string& tab_cur = "", const std::string& tab_oth = "");
// Word-wraps one lyric line to `width` columns with the words already sung highlighted (defined in app.cpp).
fs::path find_preview_video();   // scripts/preview_video.mp4 next to the program (the Settings preview of Use Video)
std::vector<std::string> render_lyric_line_wrapped(const LyricLine& line, double elapsed, int width, bool is_active, const Settings& settings);
// The rows of the lyric block of line `act`: mode 1 the lyric line, 2 the lyric line and its translation under it, 3 the translation only.
// `tr` (one text per line of `lines`, may be null) is shown the way the lyrics are: its letters / words appear while the line is sung (spread over the
// line's time), not as a dark line. A line without a translation shows its lyric line.
std::vector<std::string> lyric_translation_rows(const std::vector<LyricLine>& lines, const std::vector<std::string>* tr, int act, double lt, int width, int mode, const Settings& settings);
// A key as it is shown to the user ("ALT+L" -> the label of this platform), and word wrapping for the help texts (app.cpp).
std::string pretty_key(const std::string& k);
std::vector<std::string> wrap_words(const std::string& text, int width);
// "Chapter 2" before "Chapter 10" (audio book mode, app_audiobook.cpp)
bool natural_less(const std::string& a, const std::string& b);
// "12 files" / "1 file" (defined in app_history.cpp)
std::string n_files_label(size_t n);

// scripts/<filename> next to the program (the same search as for the lyrics helper)
std::filesystem::path locate_script(const std::string& filename);

void theme_apply(Settings& s, const themes::Theme& t);   // sets the colours of a theme (app_settings.cpp)

// Hotkey strings (defined in app.cpp, used by the Settings screen)
const char* cover_look_name(int look);               // 0 "Disk (no cover)", 1..7 the Cover Modes (app_cover.cpp)
std::string combo_key_name(int key);                  // "" when `key` is not a bindable combination
std::string normalize_hotkey_string(std::string s);   // what was typed into a key field, cleaned up

// Floating-panel legends (defined here so every panel file uses the same ones; used by the EQ / oscilloscope / spectrogram panels and the
// audio book tag editor).
// A bottom border with its text centred in it (the dashes are split evenly on both sides).
std::vector<std::string> video_download_args(const std::string& id, const std::filesystem::path& dir, const std::string& base_name, int max_height);   // the yt-dlp command of a video download (app_videosearch.cpp)
inline std::string centred_bottom(const Settings& s, int W, const std::string& text, const std::string& colour) {
    const std::string t = " " + truncate_str(text, W - 6) + " ";
    const int dashes = std::max(0, W - 2 - display_width(t));
    const int left = dashes / 2, right = dashes - left;
    std::string out = s.box_lower_left;
    for (int i = 0; i < left; ++i) out += s.box_horizontal;
    out += t;
    for (int i = 0; i < right; ++i) out += s.box_horizontal;
    out += s.box_lower_right;
    return colour.empty() ? out : colour + out + "\x1b[0m";
}

// The legend of a floating option panel, all rows in the bottom border colour. With several rows the middle one sits ON the bottom
// border line (rows/2 rows inside the panel, the rest hanging below it), so the block is centred on that line. `inside(text)` makes
// a framed row, `bottom(text)` the bottom border carrying a text.
template <class Inside, class Bottom>
void push_legend_block(std::vector<std::string>& lines, int W, const std::string& colour, const std::vector<std::string>& rows,
                       Inside inside, Bottom bottom) {
    const size_t before = rows.size() / 2;
    for (size_t i = 0; i < before; ++i) lines.push_back(inside(rows[i]));
    lines.push_back(bottom(rows[before]));
    for (size_t i = before + 1; i < rows.size(); ++i) {
        const std::string tx = truncate_str(rows[i], W - 4);
        const int left = std::max(0, (W - display_width(tx)) / 2);
        lines.push_back(colour + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, W - left - display_width(tx))), ' ') + "\x1b[0m");
    }
}

// --- the audio book panels (app_audiobook_notes.cpp, app_audiobook_marks.cpp, app_audiobook_chapters.cpp) ---
// "Chapter 03.mp3" -> "Chapter 03"
inline std::string stem_of(const std::string& file) { return path_utf8(path_from_utf8(file).stem()); }

// One framed line: the exact `inner` columns of already coloured content between the two border bars.
struct Framer {
    std::string bar;
    int inner;
    std::string operator()(const std::string& body, int used) const {
        return bar + " " + body + std::string(static_cast<size_t>(std::max(0, inner - used)), ' ') + "\x1b[0m " + bar;
    }
};

// mm:ss, or h:mm:ss from an hour on
inline std::string clock_text(double sec) {
    const long long t = static_cast<long long>(std::max(0.0, sec) + 0.5);
    char b[32];
    if (t >= 3600) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else std::snprintf(b, sizeof b, "%02lld:%02lld", t / 60, t % 60);
    return b;
}


// ---- small helpers shared by the files that were split out of app.cpp ----
inline bool contains_ci(const std::string& hay, const std::string& needle) {
    return ascii_lower_str(hay).find(ascii_lower_str(needle)) != std::string::npos;
}

inline std::string fmt_mmss(double seconds) {
    if (seconds < 0) return "--:--";
    int total = static_cast<int>(seconds);
    std::ostringstream oss;
    oss.width(2); oss.fill('0'); oss << (total / 60) << ":";
    oss.width(2); oss.fill('0'); oss << (total % 60);
    return oss.str();
}

// Center-aligns plain (no-ANSI) text within `width` visual columns.
inline std::string center_pad(const std::string& text, int width) {
    std::string t = truncate_str(text, width);
    int pad = std::max(0, width - display_width(t));
    int left = pad / 2;
    int right = pad - left;
    return std::string(left, ' ') + t + std::string(right, ' ');
}

// Visualizer bars use ONLY this glyph set (per explicit instruction) —
// distinct from WaveformQuantizer's 6-level top/mid/bot triples used by
// the progress bar.
// "/r:" searches the ARTIST only, "/a:" the ALBUM only (the plain "/" search looks at title, artist and album).
// Returns 0 (all fields), 1 (artist) or 2 (album) and the query without the prefix.
inline int local_query_scope(const std::string& query, std::string& rest) {
    size_t i = 0;
    while (i < query.size() && query[i] == ' ') ++i;
    rest = query.substr(i);
    int scope = 0;
    if (rest.size() >= 2 && rest[1] == ':') {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(rest[0])));
        scope = c == 'r' ? 1 : c == 'a' ? 2 : 0;
        if (scope) {
            rest.erase(0, 2);
            while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
        }
    }
    return scope;
}

// Called on every keystroke while typing in the search box, before
// Enter is pressed — this is the "incremental search" behavior: the
// list updates live as you type instead of only after confirming. Local
// queries get filtered+ranked immediately via the same fuzzy logic
// submit_search() will commit on Enter. A "y:" (online search) prefix
// is left alone here — firing a network request on every keystroke
// would be wasteful and slow, so online search still only fires on
// Enter — but the view is reset back to whatever it was before '/' was
// pressed so a stale local preview doesn't linger behind the search box
// while an online query is being typed.
// The online search prefixes: "y:" YouTube, "sc:" SoundCloud, "b:" Bandcamp. Returns the source (0 / 1 / 2) and the
// prefix length, or -1 when `buf` starts with none of them.
inline int online_prefix(const std::string& buf, size_t& len) {
    std::string l = buf.substr(0, 3);
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l.compare(0, 3, "sc:") == 0) { len = 3; return 1; }
    if (l.compare(0, 2, "y:") == 0) { len = 2; return 0; }
    if (l.compare(0, 2, "b:") == 0) { len = 2; return 2; }
    return -1;
}

// Oscilloscope / spectrogram colours and parameters (app_panels.cpp)
void osci_palette_rgb(const Settings& s, int id, float t, int& r, int& g, int& b);
void spectro_gradient_from(const Settings& st);
VideoFxGradient video_fx_gradient(const Settings& st);   // the visualizer gradient (left / centre / right) for the video look "gradient"
std::string osci_rgb_seq(int r, int g, int b, float k);
std::string osci_dim_ansi(const std::string& ansi, float k);
OscilloscopeVisualizer::Params osci_scope_params(const Settings& s, double dt, int style = -1);   // style: 0 braille, 1 image, -1 the one in use
// The pending meta-editor edits laid over the metadata of a file (app_panels.cpp); the tags of one file read without ffprobe (app_rowmeta.cpp)
void overlay_meta_entry(TrackMetadata& md, const MetaEditEntry& e, const fs::path& cur);
RowMeta try_native_row_meta(const fs::path& path);
// The folder overlay's path helpers (app_overlays.cpp)
bool fo_is_under(const fs::path& dir, const fs::path& root);
fs::path fo_normal_root(const std::string& s);
std::string fo_name_of(const std::string& p);

} // namespace muisc
