// Shared drawing helpers of the music player's App: search field and focus colours, box lines, and the
// text-field pieces the panels and overlays use. The rest of App lives in the app_*.cpp files next to
// this one, grouped by topic.
#include "app.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "book_art.h"
#include "keyboard_layout.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "console_log.h"
#include "karaoke_font.h"
#include "glyph_raster.h"
#include "chiptune.h"
#include "scope_big.h"
#include "scope_window.h"
#include "wizard.h"
#include "creator.h"
#include "mic_capture.h"
#include "process_util.h"
#include "app_internal.h"
#include "scripts_env.h"
#include "update_check.h"
#include "open_url.h"
#include "app_settings_tables.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <sys/types.h>
#include <sys/stat.h>   // file_added_time(): stat() / _wstat64()
#if defined(_WIN32)
#include "win_compat.h"
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace muisc {

namespace {

// ---------------------------------------------------------------------
// Worker-thread exception guard
//
// An exception that escapes a std::thread's function does NOT propagate to
// the thread that spawned it and does not unwind anywhere useful: the
// standard says it calls std::terminate(), which kills the entire process on
// the spot. On Windows that means the app vanishes with no message at all --
// no stack trace, no console output, nothing in the log. Every background
// task in this file (metadata sweep, decode, lyrics fetch, waveform pass,
// search, bulk queue add) touches the filesystem or spawns subprocesses, and
// any of those can throw.
//
// This wrapper is the process-level safety net: whatever a worker throws is
// turned into a log line, and that task alone fails. The encoding bugs fixed
// alongside it were the cause we know about; this makes sure the next one
// (a disappearing USB drive, a permissions change mid-scan) degrades instead
// of detonating.
// ASCII-only, deliberately: this folds track titles and artist names, which
// are UTF-8. std::tolower over raw bytes mangles multi-byte sequences under
// any single-byte locale -- see ascii_lower() in utf8_util.h.

// Removes exactly one full UTF-8 codepoint from the end of a text-entry
// buffer, not just its last byte. A plain pop_back() left a dangling lead
// byte behind for any accented/non-ASCII character (a German umlaut is
// two bytes; CJK is three) -- that lone byte decodes as a replacement
// glyph, and needed a second, confusing Backspace press to actually
// finish clearing what looked like one character.





// --- shared box-drawing helpers ------------------------------------------
// Every panel is built against `total_width` (the FULL visual width of the
// box, borders included) so two boxes placed side by side on the same row
// always sum to exactly the width the caller asked for, and single-box
// rows line up with everything above/below them. Content is always
// total_width-4 (for the "│ X │" pattern), padded with the codepoint-safe
// helpers from terminal_ui.cpp so multi-byte glyphs can't throw off the
// column count the way byte-length padding did before.

// SGR for a "cursor" (hovering) row. If both the configured foreground and
// background resolve to nothing -- e.g. ColorQueueCursorBg is blank in the
// user's config -- the row would look identical to its neighbours and the
// cursor would be invisible, so fall back to reverse video.

} // namespace

// SGR for search bar input (the "/" box, the meta editor's and playlist
// manager's "Search:" rows): the Colors tab's SEARCH row, where an empty
// cell follows LIST INACTIVE FG/BG -- the colours these bars always had.
std::string search_sgr(const Settings& s) { // the typed query (behind its text only)
    const std::string& fg = s.search_text_color.empty() ? s.list_color : s.search_text_color;
    const std::string& bg = s.search_bg_color.empty() ? s.list_inactive_bg_color : s.search_bg_color;
    return ansi_for(fg, false) + bg_ansi_for(bg);
}
std::string search_idle_sgr(const Settings& s) { // everything else in the bar
    // Text follows LIST INACTIVE FG, but the background is NOT inherited: an
    // unset INACTIVE BG leaves the bar plain, so only the query is highlighted.
    const std::string& fg = s.search_inactive_text_color.empty() ? s.list_color : s.search_inactive_text_color;
    return ansi_for(fg, false) + bg_ansi_for(s.search_inactive_bg_color);
}
// `text` in the query colours, then back to the idle ones; nothing when empty
// (an input that was not performed is not highlighted).
std::string search_run(const Settings& s, const std::string& text, bool nonempty) {
    // The reset matters: an idle colour without a BG would otherwise leave the query's BG running.
    return nonempty ? search_sgr(s) + text + "\x1b[0m" + search_idle_sgr(s) : text;
}

// SGR prefix of the pane focus indicator "◀" (Colors tab FOCUS row, ColorFocus=): bold plus focus_color; an unset
// color degrades to plain bold like the header does.
std::string focus_sgr(const Settings& s) {
    const std::string params = sgr_params_for(s.focus_color);
    return params.empty() ? "\x1b[1m" : "\x1b[1;" + params + "m";
}

// The small "◀" that marks which pane/field within a panel has the keyboard focus. box_top() wraps the whole title
// string in `border_ansi` (one colour for the whole row), so to make just the triangle read in the focus colour, the
// title text itself switches colour for it and then back to `border_ansi` -- the same colour box_top() already has --
// so the dashes that follow the title stay in the border colour.
// The way back to the border colour is a marker (\001) that box_top() resolves: it puts the border colour straight in front of
// the first dash after the title, where the smooth border gradient (clamp_output_rows) finds it and gives it the colour of that
// ROW -- a plain colour escape followed by the blank before the dashes would stay the top colour.
std::string focus_indicator(const Settings& s, const std::string&) {
    return " " + focus_sgr(s) + "◀" + "\x1b[0m" + "\001";
}

// The text of `s` without its CSI colour sequences (what is really drawn).
std::string strip_csi(const std::string& s) {
    std::string plain;
    for (size_t k = 0; k < s.size(); ++k) {
        if (s[k] == '\x1b' && k + 1 < s.size() && s[k + 1] == '[') {
            k += 2;
            while (k < s.size() && !(s[k] >= 0x40 && s[k] <= 0x7e)) ++k;
            continue;
        }
        plain += s[k];
    }
    return plain;
}

std::string App::box_top(const std::string& label_in, int total_width, const std::string& border_ansi) const {
    // A title wider than the pane is cut with "..." so the top border keeps its width. A wider line
    // would reach the terminal's right edge and shift everything below it. A focus indicator at the end
    // of the title (" " + colour + "◀") is kept.
    std::string label = label_in;
    bool back_to_border = false;   // the title ends with a focus indicator: the border colour comes back right before the dashes
    for (size_t at = label.find('\001'); at != std::string::npos; at = label.find('\001')) { label.erase(at, 1); back_to_border = true; }
    if (!label.empty()) {
        const int room = total_width - 1 - display_width(settings_.box_upper_left + settings_.box_horizontal) - 2;   // between "╭─ " and " ╮"
        if (display_width(strip_csi(label)) > room && room >= 6) {
            const size_t esc = label.find('\x1b');
            std::string head = esc == std::string::npos ? label : label.substr(0, esc);
            const std::string tail = esc == std::string::npos ? std::string() : label.substr(esc);
            const bool space = !head.empty() && head.back() == ' ' && !tail.empty();   // the blank in front of the indicator
            if (space) head.pop_back();
            const int head_room = room - display_width(strip_csi(tail)) - (space ? 1 : 0);
            if (head_room >= 4) label = truncate_str(head, head_room) + (space ? " " : "") + tail;
            else label = truncate_str(strip_csi(label), room);
        }
    }
    std::string lbl = label.empty() ? "" : (" " + label + " ");
    if (back_to_border && !label.empty()) lbl += border_ansi;
    std::string prefix = settings_.box_upper_left + settings_.box_horizontal + lbl;
    // The label may carry colour escapes (the focus indicator's "◀" in the
    // header colour): display_width() counts their parameter text as visible
    // columns, which shortened the top border by that many cells. Measure
    // the title with the CSI sequences removed.
    std::string plain;
    for (size_t k = 0; k < prefix.size(); ++k) {
        if (prefix[k] == '\x1b' && k + 1 < prefix.size() && prefix[k + 1] == '[') {
            k += 2;
            while (k < prefix.size() && !(prefix[k] >= 0x40 && prefix[k] <= 0x7e)) ++k;
            continue;
        }
        plain += prefix[k];
    }
    int used = display_width(plain);
    int dashes = std::max(0, total_width - used - 1);
    std::string s = prefix;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal;
    s += settings_.box_upper_right;
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

std::string App::box_bottom(int total_width, const std::string& footer_in, const std::string& border_ansi) const {
    // A footer wider than the pane would run past the corner and the terminal's edge. The hints are separated by two blanks:
    // whole hints are dropped from the end until the rest fits (a single hint that is too long is cut with "...").
    std::string footer = footer_in;
    const int room = total_width - 1 - display_width(settings_.box_lower_left + settings_.box_horizontal) - 2;
    if (!footer.empty() && room >= 6 && display_width(strip_csi(footer)) > room) {
        if (footer.find('\x1b') != std::string::npos) footer = truncate_str(strip_csi(footer), room);
        else {
            for (size_t cut = footer.rfind("  "); cut != std::string::npos && cut > 0 && display_width(footer) > room; cut = footer.rfind("  ", cut - 1))
                footer = footer.substr(0, cut);
            if (display_width(footer) > room) footer = truncate_str(footer, room);
        }
    }
    std::string prefix = footer.empty() ? (settings_.box_lower_left + settings_.box_horizontal) : (settings_.box_lower_left + settings_.box_horizontal + " " + footer + " ");
    int used = display_width(prefix);
    int dashes = std::max(0, total_width - used - 1);
    std::string s = prefix;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal;
    s += settings_.box_lower_right;
    s = pad_right(s, total_width);
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

std::string App::box_line(const std::string& content, int total_width, const std::string& border_ansi) const {
    int inner = std::max(0, total_width - 4);
    std::string padded = pad_right(truncate_str(content, inner), inner);
    if (border_ansi.empty()) return settings_.box_vertical + " " + padded + " " + settings_.box_vertical;
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    return bar + " " + padded + " " + bar;
}

// box_top() for a label whose tail is a live text field (caret + marked
// range). `field` is already painted and therefore full of reverse-video
// escapes, which display_width() -- UTF-8 aware, ANSI blind -- would count
// as columns; so the dash fill is computed from `field_cols`, the number of
// columns paint_edit_field() says it really occupies.
std::string App::box_top_field(const std::string& prefix, const std::string& field, int field_cols,
                               int total_width, const std::string& border_ansi) const {
    const int dashes = std::max(0, total_width - display_width(prefix) - field_cols - 1);
    std::string s = prefix + field;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal; // a std::string, not a char
    s += settings_.box_upper_right;
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

// The same for box_line(): `prefix` plain, `field` painted, padded to
// box_line()'s own inner width (total_width - 4).
std::string App::box_line_field(const std::string& prefix, const std::string& field, int field_cols,
                                int total_width, const std::string& border_ansi, const std::string& text_ansi) const {
    const int inner = std::max(0, total_width - 4);
    const int fill = std::max(0, inner - display_width(prefix) - field_cols);
    std::string padded = text_ansi + prefix + field + std::string(fill, ' ') + (text_ansi.empty() ? "" : "\x1b[0m");
    if (border_ansi.empty()) return settings_.box_vertical + " " + padded + " " + settings_.box_vertical;
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    return bar + " " + padded + " " + bar;
}

// Points the shared caret/selection (edit_caret_/edit_anchor_) at one
// specific field: keeps the position while that same field is being typed
// into continuously, and jumps to the end whenever a different field -- or
// the same one, freshly focused -- takes over. Called once per key by each
// box, right before edit_text_key() runs on it.
void App::edit_focus(const std::string& owner, const std::string& text) {
    if (edit_owner_ != owner) {
        edit_owner_ = owner;
        edit_caret_ = edit_anchor_ = text.size();
        le_undo_map().clear();   // another field takes the keyboard: its undo history starts empty
    }
    le_clamp(text, edit_caret_, edit_anchor_);
}

namespace {



// Word-wraps one lyric line to `width` visual columns (breaking on word
// boundaries, never mid-word), center-aligning each resulting row, and
// bakes in ANSI highlighting for words already "sung" (their timestamp
// <= elapsed) when this is the active line. Width/centering math is done
// on PLAIN text first — color codes are spliced in afterwards, since they
// don't occupy display columns but would otherwise confuse the
// codepoint-counting padding helpers.
// Counts UTF-8 codepoints (not bytes) -- used for the letter-by-letter
// lyrics reveal below. terminal_ui.cpp has an equivalent utf8_take(), but
// it's `static` (file-local) so it isn't reachable from here.


} // namespace (render_lyric_line_wrapped is shared with app_console_karaoke.cpp: declared in app_internal.h)

std::vector<std::string> render_lyric_line_wrapped(const LyricLine& line, double elapsed, int width,
                                                     bool is_active, const Settings& settings) {
    struct W { std::string text; double t; bool has_ts; };
    std::vector<W> words;
    if (!line.words.empty()) {
        for (const auto& wt : line.words) words.push_back({wt.second, wt.first, true});
    } else {
        std::istringstream iss(line.full_text);
        std::string w;
        while (iss >> w) words.push_back({w, line.start_time, false});
    }
    if (words.empty() || width <= 0) return {std::string(std::max(0, width), ' ')};

    std::vector<std::vector<W>> rows;
    std::vector<W> cur;
    int cur_len = 0;
    for (auto& w : words) {
        std::string remain = w.text;
        while (!remain.empty()) {
            int r_wlen = display_width(remain);
            int available = cur.empty() ? width : (width - cur_len - 1);
            
            if (r_wlen <= available) {
                cur.push_back({remain, w.t, w.has_ts});
                cur_len += (cur.empty() ? r_wlen : 1 + r_wlen);
                break;
            }
            
            if (!cur.empty()) {
                rows.push_back(cur);
                cur.clear();
                cur_len = 0;
                continue; // retry fitting on a new line
            }
            
            // The word exceeds the full width of a line, must split it.
            std::string chunk = utf8_take(remain, width);
            if (chunk.empty()) {
                // Failsafe: width is too small (e.g., 1) to fit a wide character (width 2).
                // Force-take 2 columns so we at least make progress (1 grapheme cluster).
                chunk = utf8_take(remain, 2);
            }
            
            cur.push_back({chunk, w.t, w.has_ts});
            rows.push_back(cur);
            cur.clear();
            cur_len = 0;
            remain = remain.substr(chunk.size());
        }
    }
    if (!cur.empty()) rows.push_back(cur);

    std::vector<std::string> out;
    for (auto& row : rows) {
        std::vector<std::string> mapped_words;
        int plain_len = 0;
        for (size_t i = 0; i < row.size(); ++i) {
            std::string mapped = apply_font_map(row[i].text, settings.font_map);
            mapped_words.push_back(mapped);
            plain_len += display_width(mapped);
            if (i > 0) plain_len += 1;
        }
        int total_pad = std::max(0, width - plain_len);
        int left_pad, right_pad;
        if (settings.lyrics_alignment == 1) { left_pad = 0; right_pad = total_pad; }           // left
        else if (settings.lyrics_alignment == 2) { left_pad = total_pad; right_pad = 0; }       // right
        else { left_pad = total_pad / 2; right_pad = total_pad - left_pad; }                    // center (default)

        // Word-level karaoke highlight: find the word currently being sung
        // (the last word whose timestamp has passed but the *next* one
        // hasn't) and render just that one underlined on top of the
        // normal active-word color -- gives the highlight a moving
        // "leading edge" instead of every already-sung word looking
        // identical.
        int currently_singing = -1;
        for (size_t i = 0; i < row.size(); ++i) {
            if (is_active && row[i].has_ts && row[i].t <= elapsed) currently_singing = static_cast<int>(i);
        }
        const char* kUnderline = "\x1b[4m";

        std::string s(left_pad, ' ');
        std::string word_ansi = ansi_for(settings.active_word_color) + bg_ansi_for(settings.active_word_bg_color);
        std::string active_line_ansi = ansi_for(settings.active_line_color) + bg_ansi_for(settings.active_line_bg_color);
        std::string inactive_ansi = ansi_for(settings.inactive_line_color) + bg_ansi_for(settings.inactive_line_bg_color);
        // Word-by-word / letter-by-letter modes hide not-yet-sung words in
        // the active line entirely (blanked to spaces, same width, so the
        // alignment doesn't jump around) instead of showing them in the
        // active-line color right away -- that's the actual "progressive
        // reveal" that was asked for, as opposed to the old always-fully-
        // visible line with just a moving color highlight.
        bool progressive = is_active && (settings.lyrics_animation == 1 || settings.lyrics_animation == 2);
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) s += ' ';
            bool sung = is_active && row[i].has_ts && row[i].t <= elapsed;
            bool no_word_ts_but_active = is_active && !row[i].has_ts;
            bool is_current = sung && static_cast<int>(i) == currently_singing;
            if (is_current && settings.lyrics_animation == 2) {
                // Letter-by-letter: interpolate how many characters of the
                // CURRENT word are revealed so far. Prefer the next word's
                // timestamp (within this wrapped row) as the end bound; if
                // this is the last word in the row (no next timestamp to
                // interpolate toward), fall back to the previous word's
                // pace instead of just popping the whole word in at once --
                // that "spawns out of nowhere" look was the bug. With no
                // timing context at all (a single-word line), use a
                // reasonable fixed pace.
                std::string full = mapped_words[i];
                int nchars = display_width(full);
                double dur = -1.0;
                if (i + 1 < row.size() && row[i + 1].has_ts && row[i + 1].t > row[i].t) {
                    dur = row[i + 1].t - row[i].t;
                } else if (i > 0 && row[i - 1].has_ts && row[i].t > row[i - 1].t) {
                    dur = row[i].t - row[i - 1].t;
                } else {
                    dur = 0.4;
                }
                double frac = std::clamp((elapsed - row[i].t) / dur, 0.0, 1.0);
                int revealed = static_cast<int>(frac * nchars);
                std::string shown = utf8_take(full, revealed);
                int hidden_cols = display_width(full) - display_width(shown);
                s += word_ansi + kUnderline + shown + "\x1b[0m" + std::string(std::max(0, hidden_cols), ' ');
            } else if (is_current) {
                s += word_ansi + kUnderline + mapped_words[i] + "\x1b[0m";
            } else if (sung) {
                s += word_ansi + mapped_words[i] + "\x1b[0m";
            } else if (progressive && row[i].has_ts) {
                s += std::string(display_width(mapped_words[i]), ' '); // not sung yet -- hidden, not just dimmed
            } else if (no_word_ts_but_active) {
                s += active_line_ansi + mapped_words[i] + "\x1b[0m";
            } else if (is_active) {
                s += active_line_ansi + mapped_words[i] + "\x1b[0m";
            } else {
                s += inactive_ansi + mapped_words[i] + "\x1b[0m";
            }
        }
        s += std::string(right_pad, ' ');
        out.push_back(s);
    }
    return out;
}

namespace {

// Resolves a filename under scripts/ next to the running binary. Shared by
// the lyrics helper and the fast-search helper -- same candidate list
// (env override, exe-relative at one/two/three levels up to cover a
// multi-config MSVC build), only the filename differs.
// Never the scripts/ of the folder Mousiki was started in: starting it inside a downloaded project with its own scripts/
// folder would run that project's Python code. A build in <checkout>/build finds <checkout>/scripts one level up.
fs::path find_scripts_file(const std::string& filename) {
    {
        fs::path p = scripts_file_from_env(filename);   // MOUSIKI_SCRIPTS_DIR, checked (see scripts_env.h)
        if (!p.empty()) return p;
    }
    fs::path fallback;   // where the installed copy belongs (returned when it is missing: the callers report "not found")

#if defined(__APPLE__)
    char exe_buf[4096];
    uint32_t size = sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &size) == 0) {
        std::error_code ec;
        fs::path exe_dir = fs::canonical(fs::path(exe_buf), ec).parent_path();
        if (!ec) {
            fs::path p = exe_dir / "scripts" / filename;
            fallback = p;
            if (fs::exists(p)) return p;
            p = exe_dir.parent_path() / "scripts" / filename;
            if (fs::exists(p)) return p;
        }
    }
#elif defined(_WIN32)
    // No /proc on Windows; GetModuleFileNameW is the direct equivalent.
    // Worth noting the lookup one level up matters more here than on Linux:
    // a multi-config MSVC build puts the exe in build\\Release\\, so
    // scripts/ is two levels above it, and the CMake copy step mirrors
    // scripts/ next to the exe to cover that.
    {
        std::string exe = win_executable_path();
        if (!exe.empty()) {
            fs::path exe_dir = path_from_utf8(exe).parent_path();
            fs::path p = exe_dir / "scripts" / filename;
            fallback = p;
            if (fs::exists(p)) return p;
            p = exe_dir.parent_path() / "scripts" / filename;
            if (fs::exists(p)) return p;
            p = exe_dir.parent_path().parent_path() / "scripts" / filename;
            if (fs::exists(p)) return p;
        }
    }
#else
    char exe_buf[4096];
    ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) {
        exe_buf[n] = '\0';
        fs::path exe_dir = fs::path(exe_buf).parent_path();
        fs::path p = exe_dir / "scripts" / filename;
        fallback = p;
        if (fs::exists(p)) return p;
        p = exe_dir.parent_path() / "scripts" / filename;
        if (fs::exists(p)) return p;
    }
#endif
    return fallback;
}

fs::path find_lyrics_script() { return find_scripts_file("fetch_lyrics.py"); }

// The InnerTube-based search script (see OnlineSource::search()). Unlike
// the lyrics script, its absence is not an error condition anywhere --
// OnlineSource falls back to yt-dlp's own search whenever this path
// doesn't resolve to a real file, so an empty/missing result here is a
// normal, silent path for anyone who only has the core scripts installed.
fs::path find_fast_search_script() { return find_scripts_file("fast_yt_search.py"); }

// fpcalc is a BUILD product: CMake compiles tools/fpcalc.cpp and copies the
// result next to mousiki.exe and into that exe's scripts/, so it exists only
// beside the executable -- never in a checkout's scripts/ folder, and
// find_scripts_file() can very well hand out exactly that one (the working
// directory wins there on purpose, so edited .py files take effect without a
// rebuild). fetch_meta.py resolves fpcalc next to the script IT was given
// (resolve_fpcalc()), so with those two rules combined a fetch started from a
// checkout's root dies with "fpcalc not found" even though this build has a
// perfectly good helper next to its own exe. Handing that path down through
// MOUSIKI_FPCLC -- the script's own documented override -- keeps both rules
// intact, and never overwrites a path the user exported themselves.
void export_fpcalc_to_scripts() {
    if (std::getenv("MOUSIKI_FPCLC")) return; // an explicit override always wins
#if defined(_WIN32)
    const char* name = "fpcalc.exe";
#else
    const char* name = "fpcalc";
#endif
    fs::path exe_dir;
#if defined(__APPLE__)
    char exe_buf[4096];
    uint32_t size = sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &size) == 0) {
        std::error_code ec;
        fs::path resolved = fs::canonical(fs::path(exe_buf), ec);
        if (!ec) exe_dir = resolved.parent_path();
    }
#elif defined(_WIN32)
    std::string exe = win_executable_path();
    if (!exe.empty()) exe_dir = path_from_utf8(exe).parent_path();
#else
    char exe_buf[4096];
    ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) {
        exe_buf[n] = '\0';
        exe_dir = fs::path(exe_buf).parent_path();
    }
#endif
    if (exe_dir.empty()) return;
    // The same ladder find_scripts_file() walks for scripts/: beside the exe,
    // in its scripts/, then one and two levels up (a multi-config MSVC build
    // puts the exe in build\Release\).
    const fs::path dirs[] = {exe_dir, exe_dir / "scripts",
                             exe_dir.parent_path() / "scripts",
                             exe_dir.parent_path().parent_path() / "scripts"};
    for (const fs::path& dir : dirs) {
        std::error_code ec;
        fs::path candidate = dir / name;
        if (!fs::exists(candidate, ec)) continue;
        std::string value = path_utf8(candidate);
#if defined(_WIN32)
        _putenv_s("MOUSIKI_FPCLC", value.c_str());
#else
        setenv("MOUSIKI_FPCLC", value.c_str(), 1);
#endif
        return;
    }
}

} // namespace

std::filesystem::path locate_script(const std::string& filename) { return find_scripts_file(filename); }

// Names the rules of the kanji reader: a fingerprint of scripts/kanji_reading.py (plus a number for changes in how the program uses it).
// The readings it made are kept in kanji_readings_cache.txt under this name; when the script changes, the cache is made again by itself.
// Empty when the script cannot be found (then the cache is kept as it is).
static std::string kanji_rules_id() {
    const std::filesystem::path script = find_scripts_file("kanji_reading.py");
    if (script.empty()) return {};
    std::ifstream in(script, std::ios::binary);
    if (!in) return {};
    unsigned long long h = 1469598103934665603ULL;   // FNV-1a
    char buf[4096];
    while (in.read(buf, sizeof buf) || in.gcount() > 0) {
        for (std::streamsize i = 0; i < in.gcount(); ++i) { h ^= static_cast<unsigned char>(buf[i]); h *= 1099511628211ULL; }
        if (in.eof()) break;
    }
    char out[32];
    std::snprintf(out, sizeof out, "1-%016llx", h);   // "1": the version of how the program uses the reader
    return out;
}

App::App(Settings* shared, bool audiobook) : audiobook_(audiobook), settings_(shared ? *shared : own_settings_) {
    if (!shared) settings_ = load_settings();
    translit_set_learned_file(path_utf8(config_path().parent_path() / "kanji_readings_learned.txt"));   // what was learned before (tags, folders, MusicBrainz)
    translit_set_kanji_cache_file(path_utf8(config_path().parent_path() / "kanji_readings_cache.txt"),   // what pykakasi read before ...
                                  kanji_rules_id());                                                  // ... as long as the reader's rules are the same
    translit_load_readings(path_utf8(config_path().parent_path() / "kanji_readings.txt"));   // the user's reading dictionary (Latin letters display)
    // Still the colours of a built-in theme as an earlier version had them (with terminal-dependent colours 1-15 and empty
    // text colours)? Then the theme's fixed colours, which look the same in every terminal (see themes::renewed_builtins()).
    for (const auto& [was, now] : themes::renewed_builtins()) {
        if (!themes::file_holds(config_path(), was, false)) continue;
        theme_apply(settings_, now);
        save_settings(settings_);
        break;
    }
    // Point yt-dlp at the configured folder (if any) before anything can
    // ask where a download would go. Empty config = default cache folder,
    // which is what set_download_dir() restores on its own.
    cache_.set_download_dir(path_from_utf8(settings_.download_folder));
    if (audiobook_) history_.set_file_name("reading_history.json");   // the reading history is a file of its own
    history_.set_dir(settings_.history_path);
    history_.load(); // listening history: a missing/corrupt file is not fatal (see HistoryStore::load)
    set_emoji_mode(settings_.emoji_mode);
    player_.set_stereo(settings_.stereo);
    player_.set_device(settings_.audio_device);   // the sound output (Settings -> GENERAL -> AUDIO "Choose Audio Device")
    adev_applied_ = settings_.audio_device;
    player_.set_normalization(settings_.normalize,
                              static_cast<float>(settings_.normalize_target_lufs),
                              static_cast<float>(settings_.normalize_max_boost_db));
    eq_apply();
    lyrics_script_ = find_lyrics_script();
    fast_search_script_ = find_fast_search_script();
    meta_script_ = find_scripts_file("fetch_meta.py"); // AcoustID helper for Mode::MetaEdit
    cf_script_ = find_scripts_file("fetch_cover.py");  // MusicBrainz / Cover Art Archive helper for Mode::CoverFetch
    name_lookup_script_ = find_scripts_file("lookup_artist_name.py");
    kanji_script_ = find_scripts_file("kanji_reading.py");
    translate_script_ = find_scripts_file("translate_lyrics.py"); // lyrics translation of Mode::ReadLyrics
    export_fpcalc_to_scripts(); // ...and the fpcalc it runs (see why there)

    if (!audiobook_) {   // (the audio book player only looks at the AUDIO BOOK PATH: library_roots())
    // The cache folder is added to the library paths here, once per launch. The list is saved back to
    // config.txt on quit, so it must not gain a duplicate entry each time: the whole list is de-duplicated
    // first (order kept, so the saved file does not get reshuffled). LocalSource::scan() also skips
    // files it has already seen.
    {
        std::vector<std::string> deduped;
        deduped.reserve(settings_.local_music_paths.size());
        for (const auto& p : settings_.local_music_paths) {
            if (std::find(deduped.begin(), deduped.end(), p) == deduped.end()) deduped.push_back(p);
        }
        settings_.local_music_paths = std::move(deduped);
    }

    // Inject the cache directory into local music paths so streamed songs
    // automatically appear in the local view for seamless offline playback
    // -- only if it isn't already there (see the dedup note just above).
    std::string cache_dir_str = path_utf8(cache_.cache_dir());
    if (std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), cache_dir_str)
        == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(cache_dir_str);
    }
    // And the configured download folder, for the same reason: whatever
    // yt-dlp writes has to be scannable without a second LocalMusicPath
    // line, which is what the DOWNLOAD PATH setting promises. Skipped
    // while it is still the default -- that IS the cache dir just above.
    std::string download_dir_str = path_utf8(cache_.download_dir());
    if (download_dir_str != cache_dir_str
        && std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), download_dir_str)
               == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(download_dir_str);
    }

    }
    local_source_.set_include_video(settings_.video_mode);   // (Video mode: video files are part of the library)
    all_local_tracks_ = local_source_.scan(library_roots(), &local_scan_diagnostics_);
    exclude_book_files(all_local_tracks_);   // the audio books are not part of the music library
    scanned_book_path_ = settings_.audiobook_path;
    local_view_ = all_local_tracks_;
    if (audiobook_) book_refresh_view();   // the left pane lists the books
    launch_row_meta_resolver();
    start_device_worker();
}


// Names of the modifier combinations that can be bound (terminal_ui.h has no plain character for them). "ALT+L" is
// the OPTION key on a Mac; the label shown for it follows the platform (pretty_key()).
struct NamedComboKey { const char* name; int code; };
static const NamedComboKey kComboKeys[] = {
    {"ALT+L", kKeyAltL}, {"ALT+C", kKeyAltC}, {"ALT+V", kKeyAltV}, {"CTRL+L", kKeyCtrlL}, {"CTRL+SHIFT+U", kKeyCtrlShiftU}, {"CTRL+SHIFT+Z", kKeyCtrlShiftZ},
    {"CTRL+T", kKeyCtrlT}, {"CTRL+Z", kKeyCtrlZ},
    {"°", kKeyDegree},   // SHIFT+^ on a German keyboard: two UTF-8 bytes, joined into one key by poll_key()
};

// "" when `key` is not one of the bindable combinations.
std::string combo_key_name(int key) {
    for (const auto& c : kComboKeys) if (c.code == key) return c.name;
    return "";
}

// What the user typed into a key field, cleaned up: ALT/OPTION spelled either way, any case for the names ("enter",
// "ctrl+l", "up"). Single characters are left exactly as typed (a capital letter means SHIFT+letter).
std::string normalize_hotkey_string(std::string s) {
    if (s.size() <= 1) return s;
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (s.rfind("OPTION+", 0) == 0) s = "ALT+" + s.substr(7);
    if (s == "UP") return "ARROW_KEY_UP";
    if (s == "DOWN") return "ARROW_KEY_DOWN";
    if (s == "LEFT") return "ARROW_KEY_LEFT";
    if (s == "RIGHT") return "ARROW_KEY_RIGHT";
    if (s == "RETURN") return "ENTER";
    if (s == "ESCAPE") return "ESC";
    return s;
}

int App::hotkey_string_to_key(const std::string& s) {
    if (s == "ARROW_KEY_UP") return 'A';
    if (s == "ARROW_KEY_DOWN") return 'B';
    if (s == "ARROW_KEY_RIGHT") return 'C';
    if (s == "ARROW_KEY_LEFT") return 'D';
    if (s == "ENTER") return '\n';
    if (s == "TAB") return 9;
    if (s == "SPACE") return ' ';
    if (s == "ESC") return 27;
    if (s == "BACKSPACE") return 127;
    for (const auto& c : kComboKeys) if (s == c.name) return c.code;
    if (s.size() == 1) return static_cast<int>(s[0]);
    return 0;
}

bool App::key_is(const char* action, int key) const {
    const auto it = settings_.hotkeys.find(action);
    if (it == settings_.hotkeys.end() || it->second.empty()) return false;
    if (hotkey_string_to_key(it->second) != key) return false;
    // The arrows travel as 'A'..'D' (see resolve_hotkey_action()): a typed capital and an arrow are told apart here.
    if (key >= 'A' && key <= 'D') return last_key_was_arrow() == (it->second.rfind("ARROW_KEY_", 0) == 0);
    return true;
}

std::string App::resolve_hotkey_action(int key) const {
    // The arrow keys travel as the letters 'A'..'D', so a typed SHIFT+a..d is the same int as an arrow. The input
    // layer says which one it was; an arrow only matches ARROW_KEY_* bindings and a typed letter only the others.
    // (Without this, SHIFT+c read as Right and seeked forward, and a binding on "C" fired on the Right arrow.)
    const bool ambiguous = key >= 'A' && key <= 'D';
    const bool was_arrow = ambiguous && last_key_was_arrow();
    for (const auto& [action, key_str] : settings_.hotkeys) {
        if (hotkey_string_to_key(key_str) != key) continue;
        if (ambiguous && was_arrow != (key_str.rfind("ARROW_KEY_", 0) == 0)) continue;
        return action;
    }
    return "";
}

std::string App::hotkey_conflict(const std::string& key_str, const std::string& except_action) const {
    if (key_str.empty()) return "";
    // The karaoke size keys only work inside the karaoke overlay and the mode keys outside it, so each pair may share keys.
    auto is_size = [](const std::string& a) { return a == "HKeyKaraokeBigger" || a == "HKeyKaraokeSmaller"; };
    auto is_mode = [](const std::string& a) { return a == "HKeyModeNext" || a == "HKeyModePrev"; };
    for (const auto& [action, val] : settings_.hotkeys) {
        if (action == except_action) continue;
        if ((is_size(action) && is_mode(except_action)) || (is_mode(action) && is_size(except_action))) continue;
        if (shares_shift_b(action, except_action)) continue;   // one key, three places
        if (val == key_str) return action;
    }
    return "";
}

// Pushes a message into both the one-line status area (existing
// behavior) and the persistent console log (both the in-memory buffer
// the Console overlay reads and, via ConsoleLog, the on-disk
// console.log) -- so events are still visible after they've scrolled
// off the status line, after a terminal-session switch redraw wiped
// the screen, or after the session itself has ended.
// HKeyTranslit: the Latin letters display on / off. Also reachable from the karaoke and READ LYRICS overlays.
void App::toggle_latin_letters() {
    settings_.latin_letters = !settings_.latin_letters;
    set_translit_enabled(settings_.latin_letters);
    status_line_ = settings_.latin_letters ? "Latin letters: on (Japanese kana, Korean and Chinese text are shown in Latin letters)" : "Latin letters: off";
    force_redraw_ = true;
}

void App::log_event(const std::string& msg) {
    status_line_ = msg;
    ConsoleLog::instance().log_basic(msg);
}

// Tab layout: 0=Colors, 1=General (was On/Off), 2=Animation, 3=Paths, 4=Reference, 5=About App.
//
// The Paths tab holds the LOCAL PATH / DOWNLOAD PATH / PLAYLIST PATH
// sections (editable path rows -- see build_path_rows()).
//
// The Reference tab opens with a one-line read-only note pointing at the
// cheat sheet ('?') for the full command list, then every rebindable hotkey
// (kRefRows), grouped into categories via the optional `header` field -- set
// only on a category's first row, and rendered as a section title above it --
// and finally the read-only font-mapping table loaded from config.txt. Loudness normalisation is set in the SHIFT+v overlay of the main UI (see build_norm_menu_panel()),
// not here. This tab
// deliberately does NOT also list the app's literal/non-rebindable key
// commands (ESC, Y/N, the playlist and meta editors' own fixed navigation
// and text-editing keys, and so on): there is nothing to configure for
// those here, and keeping a second copy of them just meant this list and
// the cheat sheet's could quietly drift apart. The cheat sheet ('?') is the
// single, authoritative list of every command, rebindable or not -- hence
// the note. All sections scroll together as one list; see ref_display_row()
// below for how a selectable row (settings_row_) maps to the row it's
// actually drawn on, once the section headers/dividers are accounted for.
//
// IMPORTANT: kRefRows[row].action is looked up in settings_.hotkeys (a
// plain string->string map), so reordering/recategorizing rows here is
// always safe -- rebinding still keys off the action name, never off the
// row's position.
// How a key is shown in the cheat sheet and on the REFERENCE tab: a capital letter is a Shift press, so "T" reads
// "SHIFT+t" (the key stays "T" in config.txt and while it is being edited); literal labels such as "SHIFT+t" or
// "CTRL+SHIFT+z" get the same lower-case letter.
std::string pretty_key(const std::string& k) {
    if (k == "*") return mode_switch_key_label();   // the characters of the mode keys / karaoke size keys, named for this keyboard (keyboard_layout.h)
    if (k == "_") return mode_switch_back_label();
    if (k == "@SWITCHKEY") return mode_switch_key_label();   // the mode-switch key, named for this keyboard (keyboard_layout.h)
    if (k == "@SWITCHBACK") return mode_switch_back_label();   // ... and the one that goes back to the previous mode
    if (k == "(" || k == ")" || k == "=") return symbol_key_label(k);   // SHIFT+8 / SHIFT+9 / SHIFT+0 on a German keyboard
    if (k == "@KARAOKESIZE") {   // the karaoke size keys * and _, named for this keyboard
        const std::string up = mode_switch_key_label();
        return up + " / " + (up == "*" ? "_" : up == "SHIFT and +" ? "SHIFT and -" : "SHIFT+-");
    }
    if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z') return std::string("SHIFT+") + static_cast<char>(k[0] + 32);
    std::string o = k;
    if (o.rfind("ALT+", 0) == 0) o = std::string(MUISC_ALT_NAME_UC) + o.substr(3);   // ALT, or OPTION on a Mac
    if (o.size() >= 3 && o[o.size() - 2] == '+' && o[o.size() - 1] >= 'A' && o[o.size() - 1] <= 'Z' &&
        (k.rfind("CTRL+", 0) == 0 || k.rfind("ALT+", 0) == 0))
        o[o.size() - 1] = static_cast<char>(o[o.size() - 1] + 32);   // CTRL+L reads CTRL+l, like the SHIFT+x labels
    size_t p = 0;
    while ((p = o.find("SHIFT+", p)) != std::string::npos) {
        p += 6;
        if (p < o.size() && o[p] >= 'A' && o[p] <= 'Z' && (p + 1 >= o.size() || !std::isalpha(static_cast<unsigned char>(o[p + 1]))))
            o[p] = static_cast<char>(o[p] + 32);
    }
    return o;
}

// The settings screen's tab strip (two lines) for a pane that carries its own title on the top border:
//   ╭─ TITLE ──────┐  [CURRENT]  ┌──┐  OTHER  ┌───────────...───╮
//   │              └─────────────┘  └─────────┘                 │
// Only the two outer top corners are rounded (the box corner glyphs of the settings); the indents are square.
std::pair<std::string, std::string> menu_tab_strip(int W, const std::string& title, const std::vector<std::string>& labels,
                                                   int cur, const std::string& top_ansi, const std::string& bot_ansi,
                                                   const std::string& corner_ul, const std::string& corner_ur,
                                                   const std::string& vertical, int min_inner,
                                                   const std::string& line2, int line2_cols,
                                                   const std::string& tab_cur, const std::string& tab_oth) {
    const std::string R = "\x1b[0m";
    auto rep = [](const std::string& s, int n) { std::string o; for (int i = 0; i < n; ++i) o += s; return o; };
    const std::string Hz = "\u2500";
    const int tw0 = display_width(title);
    const int n0 = static_cast<int>(labels.size());
    int tabs_w = 2;   // exactly one tab carries the two "[ ]" characters
    for (const auto& l : labels) tabs_w += display_width(l) + 4;
    tabs_w += 4 * std::max(0, n0 - 1);
    const int natural = 1 + 1 + tw0 + 1 + std::max(5, 20 - (tw0 + 3));
    // tab names sit at the right end: exactly three "\u2500" remain before the last corner
    const int first_inner = std::max({natural, min_inner, W - 2 - tabs_w - 5});
    const int dashes = first_inner - (1 + 1 + tw0 + 1);
    std::string top = top_ansi + corner_ul + "\u2500 " + title + " " + rep(Hz, dashes) + "\u256e" + R;
    std::string bot = bot_ansi + vertical + R +
                      (line2.empty() ? std::string(static_cast<size_t>(first_inner), ' ')
                                     : line2 + std::string(static_cast<size_t>(std::max(0, first_inner - line2_cols)), ' ')) +
                      bot_ansi + "\u2570" + R;
    int used = first_inner + 2;
    const int n = static_cast<int>(labels.size());
    for (int i = 0; i < n; ++i) {
        const std::string label = i == cur ? "[" + labels[static_cast<size_t>(i)] + "]" : labels[static_cast<size_t>(i)];
        const int tw = display_width(label) + 4;
        top += "  " + (i == cur ? tab_cur : tab_oth) + label + R + "  ";
        bot += bot_ansi + rep(Hz, tw) + R;
        used += tw;
        if (i < n - 1) {
            top += top_ansi + "\u256d\u2500\u2500\u256e" + R;
            bot += bot_ansi + "\u256f  \u2570" + R;
            used += 4;
        } else {
            const int rest = std::max(0, W - used - 2);
            top += top_ansi + "\u256d" + rep(Hz, rest) + corner_ur + R;
            bot += bot_ansi + "\u256f" + R + std::string(static_cast<size_t>(rest), ' ') + bot_ansi + vertical + R;
        }
    }
    return {top, bot};
}

// The small "◀" that marks which pane/field within a panel has the
// keyboard focus. box_top() wraps the whole title string in `border_ansi`
// (one colour for the whole row), so to make just the triangle read in the
// header colour, the title text itself must switch colour for it and then
// switch back to `border_ansi` -- the same colour box_top() already has --
// so the dashes that follow the title stay in the border colour.

// SGR prefix for the key command legends (the "[ESC] close | [ENTER] ..."
// hint lines of the Settings panel, the big list / queue overlays and the
// playlist / meta / history screens): settings_.legend_color, the Colors
// tab's LEGEND row. The default "8" yields "\x1b[38;5;8m", the grey these lines
// were always drawn in. An explicit 0/empty means "no color" like everywhere
// else, i.e. the terminal's own text color -- an empty prefix, not a stray
// escape. Callers close the run with "\x1b[0m" themselves.
std::string legend_sgr(const Settings& s) {
    return ansi_for(s.legend_color, false);
}

// One row of a legend tree table (layout_legend_tree()) in the colours of the Colors tab: the row label ("Move", "Fetch" ...)
// keeps the HEADER colour, the keys use the LEGEND colour (KEY-CMD) and their descriptions the COMMAND colour. The tree lines
// and the bar are drawn in the border colour with their glyph right behind the escape, so the smooth border gradient (BorderGradient=smooth)
// recolours them row by row with the frame they belong to. (`fade` is kept for the callers; it no longer changes anything.)
std::string paint_legend(const Settings& s, const std::vector<LegendRow>& rows, size_t i, const std::string& border, int pad_to, bool fade) {
    (void)fade;
    return paint_legend_row(rows[i], border, header_sgr(s), legend_sgr(s), pad_to, ansi_for(s.legend_cmd_color, false));
}

// SGR prefix for "this field is actively being typed into" -- every
// in-place text edit across the Settings panel (a path row, a hex colour
// value, a hotkey) and, now, the meta editor's tag fields. The Colors
// tab's CHANGE row (settings_.change_text_color / change_bg_color);
// defaults to "7"/"1" (palette white on red).
std::string change_sgr(const Settings& s) {
    return ansi_for(s.change_text_color, false) + bg_ansi_for(s.change_bg_color);
}

// Keeps at most (term_rows - 1) lines of `frame` (the -1 leaves the
// terminal's last row untouched, so the final line's trailing '\n' can
// never itself trigger a scroll) and drops everything after that,
// escape-code prefixes and all -- this is the hard backstop described
// in term_rows_'s comment in app.h: whatever the panel-sizing logic
// above computed, the actual printed output can never exceed what the
// real terminal can show without scrolling. Content past the cutoff is
// simply not drawn this frame rather than causing any corruption.
std::string App::clamp_output_rows(const std::string& frame, int term_rows) const {
    int max_lines = std::max(1, term_rows - 1);
    int newlines_seen = 0;
    size_t keep = frame.size();
    for (size_t i = 0; i < frame.size(); ++i) {
        if (frame[i] == '\n') {
            ++newlines_seen;
            if (newlines_seen >= max_lines) { keep = i + 1; break; }
        }
    }
    // BorderGradient=smooth: every frame leaves through here, so this is where the border lines get their row's colour
    // (the cursor-positioned Settings screens do their own gradient and have no line breaks).
    if (settings_.border_smooth && newlines_seen >= 2) {
        static const std::vector<std::string> kGlyphs = {"\u2502", "\u2500", "\u256d", "\u256e", "\u2570", "\u256f", "\u251c", "\u2524", "\u252c", "\u2534", "\u253c", "\u250c", "\u2514"};
        std::vector<std::string> glyphs = kGlyphs;
        for (const std::string* g : {&settings_.box_upper_left, &settings_.box_upper_right, &settings_.box_lower_left,
                                     &settings_.box_lower_right, &settings_.box_vertical, &settings_.box_horizontal})
            if (!g->empty()) glyphs.push_back(*g);
        const std::string top = ansi_for(settings_.border_color, false), bottom = ansi_for(settings_.border_color_bottom, false);
        return border_gradient::apply(frame.substr(0, keep), top, bottom, glyphs, [&](float t) {
            return gradient_ansi(settings_.border_color, settings_.border_color_bottom, t, false);
        });
    }
    return keep == frame.size() ? frame : frame.substr(0, keep); // already within budget, or cut to it
}

// ---------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------

int App::run() {
    // Coming back from the radio the App is still alive (it was only suspended): nothing is initialised or restored again.
    const bool resuming = suspended_;
    suspended_ = false;
    g_settings_hifi_rows = settings_.bars_style == 1;
    g_settings_book_layout = audiobook_;   // (the settings screen shows this mode's rows)
    remote::start("player");
    if (resuming) sync_after_radio();
    remote_app_apply();   // the remote app's web server (Settings -> GENERAL -> REMOTE / UPDATES -> Remote App)   // the radio's settings may have changed some of ours meanwhile
    bool restored = false;
    if (!resuming) {
    ConsoleLog::instance().init(settings_.console_verbosity == 1 ? LogVerbosity::Verbose : LogVerbosity::Basic);
    // Flush what the constructor's local-library scan found before logging
    // was ready to record it -- see local_scan_diagnostics_'s declaration
    // for why this can't just be logged from inside scan() directly. Basic
    // level, not Verbose: "why does my library look wrong" is exactly the
    // kind of thing someone shouldn't need to raise console_verbosity to see.
    for (const auto& line : local_scan_diagnostics_) {
        ConsoleLog::instance().log_basic(line);
    }
    {
        // Verbose-only startup facts -- "what the OS provided" at the
        // very start of the session, before anything else has run.
#if defined(_WIN32)
        ConsoleLog::instance().log_verbose("os: Windows");
#else
        struct utsname uts{};
        if (uname(&uts) == 0) {
            ConsoleLog::instance().log_verbose(std::string("os: ") + uts.sysname + " " + uts.release + " " + uts.machine);
        }
#endif
        ConsoleLog::instance().log_verbose("home: " + std::string(std::getenv("HOME") ? std::getenv("HOME") : "(unset)"));
    }

    // Session snapshot restore -- only when the feature's on. A missing
    // or corrupt snapshot.json is treated identically to "no snapshot at
    // all" (load_snapshot() already guards that), so this always falls
    // back to the normal cold-start behavior on any failure.
    if (settings_.autosave_enabled && !audiobook_) {   // (the audio book player keeps its place in the .cue files instead)
        SnapshotData snap;
        if (load_snapshot(snap)) {
            restore_snapshot(snap);
            restored = true;
            // Consumed exactly once -- see snapshot.h's delete_snapshot()
            // comment for why this happens right after a successful
            // restore rather than only at the next autosave/exit.
            delete_snapshot();
            log_event("restored previous session");
        }
    }
    if (!restored && !local_view_.empty() && !audiobook_) {
        selected_ = 0;
        start_local_track(local_view_[0]);
    }
    // First start after an install: greet with the help wizard. Closing it
    // writes WizardSeen=true, so this happens exactly once -- and only on a
    // cold start, never when coming back from the radio (which keeps the
    // player suspended, not restarted).
    if (wizard::kEnabled && !settings_.wizard_seen && !audiobook_) wizard_open();
    }   // !resuming

    TerminalIO term;
    if (resuming) { quit_ = false; switch_mode_ = false; force_redraw_ = true; }
    last_frame_time_ = std::chrono::steady_clock::now();
    last_autosave_at_ = std::chrono::steady_clock::now();
    ConsoleLog::instance().log_verbose("terminal: " + std::to_string(term.rows()) + "x" + std::to_string(term.cols()) + " (rows x cols, raw ioctl)");

    std::string last_frame_str;
    std::vector<std::string> prev_lines;   // the main screen's lines as last sent (diff_frame_lines)
    std::string last_floating;             // the overlay part of the last frame
    auto last_frame_written_at = std::chrono::steady_clock::now();
    bool gfx_shown = false;
    unsigned long gfx_tick = 0;


    while (!quit_) {
        const auto frame_start = std::chrono::steady_clock::now();
        // the image styles (oscilloscope or spectrogram) need to know what the terminal speaks: ask once, and again when
        // the protocol setting changes
        // (the cover is a picture in the cover looks 0-4 -- the default is 2 -- so the terminal is asked at the start whatever the oscilloscope / spectrogram styles are: before, with braille styles the cover stayed braille until an image style was chosen once)
        // (the Settings previews, the meta editor and the cover fetch show pictures too: with braille styles in use nothing had asked the terminal yet, so they fell back to braille)
        const bool pictures_wanted = mode_ == Mode::Settings || mode_ == Mode::ColorEdit || mode_ == Mode::MetaEdit || mode_ == Mode::CoverFetch || mode_ == Mode::VideoFetch;
        if ((settings_.osci_style == 1 || settings_.spectro.style == 1 || audiobook_ || pictures_wanted || settings_.cover_mode <= 4) && gfx_probed_pref_ != settings_.gfx_protocol) {
            if (gfx_shown) { write_frame(gfx_clear(gfx_proto_)); gfx_shown = false; }
            gfx_proto_ = gfx_probe(settings_.gfx_protocol);
            gfx_probed_pref_ = settings_.gfx_protocol;
        }
        // Drain every key already queued before rendering, rather than
        // one per frame. A single keystroke can arrive as more than one
        // poll_key() call's worth of data -- any non-ASCII character (a
        // German umlaut, most concretely) is a multi-byte UTF-8 sequence
        // dispensed one byte per call, on both platforms (see
        // TerminalIO::poll_key() / win_poll_key()). Rendering between
        // those calls meant the frame in between showed a buffer ending
        // in a lone, incomplete lead byte -- which decodes as a
        // replacement glyph -- for one frame, before the next poll
        // completed the sequence and it snapped to the real character.
        // Draining first means the frame that actually renders always
        // has a complete, valid buffer. This never blocks waiting for
        // more input: poll_key() is non-blocking and returns 0 the
        // moment nothing already-received is left to hand back.
        // A single-line text field is being edited right now: hand Ctrl+C /
        // Ctrl+X / Ctrl+V to the app as keystrokes instead of letting the
        // console treat Ctrl+C as "kill this process" (Windows, where the
        // ENABLE_PROCESSED_INPUT bit is what does that) or swallow it via
        // ISIG (POSIX). Re-derived every frame rather than toggled on mode
        // entry/exit: mode_ can change in ways that skip a hand-off (ESC out
        // of an edit, a prompt), and leaving the flag stuck would mean
        // Ctrl+C no longer quits the app. Same key set both platforms, see
        // win_poll_key()/poll_key() for how it arrives as kKeyCtrlC/X/V.
        const bool playlist_text_field =
            mode_ == Mode::Playlist &&
            ((playlist_export_open_ && playlist_export_field_ == 0) ||
             (playlist_import_open_ && playlist_import_field_ == 0) ||
             (!playlist_export_open_ && !playlist_import_open_ &&
              ((playlist_tab_ == 0 && playlist_edit_focus_ <= 1) ||
               (playlist_tab_ == 1 && playlist_manage_focus_ == 0))));
        set_text_entry(mode_ == Mode::Search ||
                       mode_ == Mode::ColorEdit ||
                       (mode_ == Mode::BulkAdd && !bulk_add_results_ready_) ||
                       (mode_ == Mode::QueueToPlaylist && qtp_stage_ >= 1) ||
                       (mode_ == Mode::RetryLyrics && rl_text_ptr(rl_focus_) != nullptr) ||
                       (mode_ == Mode::ReadLyrics && rd_add_open_) ||   // add lyrics manually: the search fields (and CTRL+c clears the choice)
                       (mode_ == Mode::ReadLyrics && rd_te_open_ && rd_te_focus_ == 0 && !rd_te_ask_ && !rd_te_new_ask_) ||   // the translation editor (CTRL+c copies)
                       (audiobook_ && book_note_edit_ && book_note_typing_ && !book_note_discard_ask_) ||   // the note editor (CTRL+c copies)
                       (audiobook_ && book_note_shown_ && !book_note_edit_) ||                            // a note in the field: CTRL+c copies it
                       (audiobook_ && mode_ == Mode::BookOverlay && book_ov_kind_ == BookOv::Notes) ||     // the notes overlay (list, path fields, editor)
                       (mode_ == Mode::Cheatsheet && cheat_searching_) ||
                       (mode_ == Mode::Wizard && wizard_st_.searching) ||
                       (mode_ == Mode::MetaEdit && meta_tab_ == 0 &&
                        (meta_focus_ == 0 || meta_focus_ == 2)) ||
                       (mode_ == Mode::CoverFetch && cf_tab_ == 0 && cf_focus_ == 0) ||
                       (mode_ == Mode::VideoFetch && vf_focus_ == 0) ||
                       playlist_text_field);

        for (int key = term.poll_key(); key != 0; key = term.poll_key()) {
            const bool in_settings = mode_ == Mode::Settings || mode_ == Mode::ColorEdit;
            if (in_settings) status_line_.clear();   // a message stays until the next key; then the "unsaved changes" note shows again
            handle_key(key);
            if (mode_ == Mode::Settings || mode_ == Mode::ColorEdit) settings_update_dirty();
        }

        poll_pending_search();
        poll_pending_load();
        poll_pending_waveform();
        // A file that gave no length plays in the windowed buffer; its length is known once the decoder or the analysis pass
        // has been through it: from then on the time and the progress bar show it.
        if (current_pcm_ && total_sec_ == 0 && current_pcm_->windowed) {
            const size_t tf = current_pcm_->total_frames.load(std::memory_order_acquire);
            if (tf > 0) total_sec_ = tf / static_cast<size_t>(std::max(1, current_pcm_->sample_rate));
        }
        // A length that is far too short: the buffer holds the length + 25 %; whatever is longer is cut off. Not silently:
        if (current_pcm_ && current_pcm_.get() != pcm_cut_noted_ && current_pcm_->capacity_exceeded.load(std::memory_order_relaxed)) {
            pcm_cut_noted_ = current_pcm_.get();
            const int mins = static_cast<int>(current_pcm_->capacity_frames() / static_cast<size_t>(std::max(1, current_pcm_->sample_rate)) / 60);
            status_line_ = "this track is longer than its file says: only its first " + std::to_string(std::max(1, mins)) +
                           " minutes play -- converting it (e.g. ffmpeg -i in.ext out.mp3) fixes that";
            ConsoleLog::instance().log_basic("audio: " + status_line_);
        }
        poll_pending_bulk_add();
        poll_pending_atp();
        borrow_video_tags();
        poll_pending_row_meta_tags();
        poll_pending_meta_fetch(); // AcoustID results -> pending edits
        poll_cover_fetch();        // the cover fetch overlay: index, fetch list
        capture_status();          // the status messages go to the history shown in the console overlay
        poll_kanji_reader();       // kanji of Japanese texts read by pykakasi (when installed), in the background
        poll_name_lookup();        // the Latin name of an artist in Chinese characters (MusicBrainz, in the background)
        poll_meta_save();          // the next slice of a running save
        video_search_poll();       // a finished video search / download
        video_fetch_poll();        // FETCH VIDEOS: pairings made, the end of a batch
        if (audiobook_) book_poll();   // the loaded chapter's tags, a running tag save
        audio_device_apply();           // a changed sound output (overlay, Settings discard, the other mode) reaches the player
        readlyrics_poll();         // a finished lyrics translation
        maybe_autosave();
        remote_poll();             // `mousiki --cmd ...` and the status file

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_frame_time_).count();
        last_frame_time_ = now;
        viz_dt_ = dt;

        if (has_track_) {
            player_.poll_elapsed();
            // No auto-advance while a device handoff is still in flight:
            // finished_ belongs to the previous track until play() swaps the
            // new one in (see device_play_pending_gen_).
            if (!advancing_ && device_play_pending_gen_.load() == 0 && player_.finished()) {
                if (creator_show_) creator_preview_loop(); else advance_track();
            }
        }
        creator_preview_tick();
        sleep_timer_tick();
        creator_resume_tick();
        player_.set_scope_raw(settings_.osci().music);   // oscilloscope music mode: the scopes get the unprocessed signal
        scope_window_sync();
        spectro_window_sync();
        enforce_visual_switches();
        // Disk only spins while something is actually playing — frozen
        // when idle or paused, per instruction.
        if (audiobook_) {   // the audio book's own animations: the book moves while a chapter plays, the stack while none is loaded
            if (has_track_ && !player_.is_paused()) book_anim_t_ += dt;
            if (!has_track_) book_idle_t_ += dt;
        }
        if (has_track_ && !player_.is_paused()) {
            angle_ = std::fmod(angle_ + kAngularVelocity *settings_.disk_rotation_speed * dt,
                               2.0 * 3.14159265358979323846);
        }
        // The wizard's welcome disk has an angle of its own: a fixed 0.10,
        // turning whether or not anything is playing, so the first screen a
        // new user sees is never a frozen picture.
        if (mode_ == Mode::Wizard) {
            wizard_angle_ = std::fmod(wizard_angle_ + kAngularVelocity * 0.10 * dt,
                                      2.0 * 3.14159265358979323846);
        }
        // Listening history: accrue what was actually heard this frame.
        // Paused time never counts (that's the is_paused() check), and neither
        // does the handover gap between two tracks -- there advancing_ is set
        // or a device handoff is in flight while nothing is really sounding.
        // No-op when no play is live, so it can be called unconditionally.
        if (has_track_ && !player_.is_paused() && !advancing_ &&
            device_play_pending_gen_.load() == 0) {
            history_.add_listened(dt);
        }

        // A layout bug on some odd terminal size (a negative width reaching a
        // std::string, a vector sized from a negative row count, ...) would
        // throw out of render_frame(), through run(), and end the program --
        // which is exactly what a window being dragged smaller looks like from
        // the outside. Skip that one frame instead (the next resize event or
        // frame will usually lay out fine), log it, and force a full repaint
        // once rendering works again.
        terminal_black_background(settings_.black_background);   // Settings -> GENERAL -> TERMINAL (only writes when it changes)
        terminal_default_text(settings_.list_color);              // text without a colour of its own: the list text colour
        std::string frame_str;
        try {
            frame_str = render_frame(term);
        } catch (const std::exception& e) {
            ConsoleLog::instance().log_basic(std::string("render skipped (") + std::to_string(term.cols()) + "x" + std::to_string(term.rows()) + "): " + e.what());
            force_redraw_ = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }
        // The Style Option overlay opened from the main screen (SHIFT+s), and SEARCH YT while its thumbnail is a picture: the main screen behind it stands still. Its disk and
        // visualizations would change lines every frame, and each of those line writes lands next to the preview picture (Sixel
        // replaces the cells under it): the picture then flickers. Only the panel (and its preview picture) is live.
        bool sty_frozen = false;
        {
            static std::string frozen_body;
            static int frozen_cols = -1, frozen_rows = -1;
            const bool freeze = ((mode_ == Mode::MetaEdit && meta_sty_from_main_) || (mode_ == Mode::VideoSearch && vs_pic_ != 0)) && frame_main_len_ != std::string::npos && frame_main_len_ <= frame_str.size();   // (SEARCH YT with a thumbnail picture freezes the main screen too: the line writes of the live screen next to a Sixel picture wipe it)
            // (the look chosen in the panel is shown behind it at once: the frozen screen is taken again when the look changes)
            const std::string look_sig = std::to_string(eff_cover_mode()) + "|" + std::to_string(eff_cover_contrast()) + "|" + std::to_string(eff_cover_brightness()) + "|" +
                                         std::to_string(eff_cover_colors()) + "|" + (settings_.album_cover ? "a" : "d");
            static std::string frozen_sig;
            if (freeze && (frozen_body.empty() || frozen_cols != term.cols() || frozen_rows != term.rows() || frozen_sig != look_sig)) {
                frozen_body = frame_str.substr(0, frame_main_len_);
                frozen_cols = term.cols(); frozen_rows = term.rows();
                frozen_sig = look_sig;
            } else if (freeze) {
                sty_frozen = true;
                frame_str = frozen_body + frame_str.substr(frame_main_len_);
                frame_main_len_ = frozen_body.size();
            } else frozen_body.clear();
        }
        // Static screens (menus, settings, a paused player) produce the very
        // same bytes frame after frame; sending them again only gives the
        // terminal something to repaint. Skip identical frames, but still
        // resend one every couple of seconds so the screen heals itself if
        // anything outside the app (a terminal resize/restore) scribbled on it.
        // Sixel: the text skips the picture's cells (they stay as they are on screen), so the picture is only sent when
        // there is a new one, or when it is not on screen yet / the screen was cleared (then the current one at once).
        // spectrogram picture (tiles): where the frame put it, unless a big overlay covers the whole area
        const int sp_proto = gfx_proto_ == GfxProto::Kitty ? 1 : gfx_proto_ == GfxProto::Sixel ? 2 : 0;
        // Only a main-screen (or spectrogram full-screen) frame shows it: the full-screen takeovers (settings, cheat
        // sheet, playlists ...) also lay out the top panel for their sizing, which must not count as "drawn".
        const bool main_frame = frame_main_len_ != std::string::npos;
        const bool sp_img = main_frame && spectro_area_[0] >= 0 && sp_proto != 0 && !list_overlay_active() && !queue_overlay_active();
        const bool sp_creator = sp_img && creator_show_;   // the creator's preview has its own spectrogram, settings and picture
        // The spectrogram's analysis keeps running while a track is loaded, shown or not (a menu or another view over it, a pause): it
        // is cheap, and the history is there when the picture is opened again -- before, the feed was switched off whenever a frame did not
        // draw it, so what was played meanwhile was missing (gaps after an overlay, an empty picture when opened during a pause).
        if (has_track_) spectro().update(settings_.spectro);
        else if (!spectro_used_ || !main_frame) spectro().set_visible(false);
        // the Settings preview box: its spectrogram is a picture too (settings_preview.cpp)
        const bool pv_img = (mode_ == Mode::Settings || mode_ == Mode::ColorEdit) && pv_spectro_area_[0] >= 0 && sp_proto != 0 && settings_preview_spectro();
        if (frame_str.find("\x1b[2J") != std::string::npos) { spectro_gfx_.invalidate(); creator_spectro_gfx_.invalidate(); pv_spectro_gfx_.invalidate(); }
        const bool sixel_on = (gfx_ok_ && gfx_proto_ == GfxProto::Sixel && gfx_.active && gfx_.crop < gfx_.cols) || (sp_img && sp_proto == 2) || (pv_img && sp_proto == 2);
        if (sixel_on && (!gfx_shown || frame_str.find("\x1b[2J") != std::string::npos)) { gfx_.fresh = true; gfx2_.fresh = true; }
        const auto frame_now = std::chrono::steady_clock::now();
        std::string wire;   // ONE write per frame (text + picture): two writes let the terminal paint in between = flicker
        // (no heal frame under the frozen Style Option overlay: the whole screen written again every two seconds makes the picture blink)
        const bool heal = !sty_frozen && frame_now - last_frame_written_at > std::chrono::seconds(2);
        if (frame_str != last_frame_str || heal) {
            // The main screen: only the lines that changed (at 120 / 165 fps the whole screen every frame would be
            // megabytes per second for the terminal). Full frames: every 2 s, after a clear, or when an overlay changed.
            const size_t main_len = frame_main_len_ <= frame_str.size() ? frame_main_len_ : std::string::npos;
            const std::string floating = main_len == std::string::npos ? std::string() : frame_str.substr(main_len);
            bool diffed = false;
            if (main_len != std::string::npos && frame_str.compare(0, 3, "\x1b[H") == 0) {
                std::string d;
                const std::string body = frame_str.substr(3, main_len - 3);
                if (diff_frame_lines(body, prev_lines, d)) {
                    if (!heal && floating == last_floating) {
                        // what follows the lines continues where the whole frame would have left the cursor
                        const long nl = static_cast<long>(std::count(body.begin(), body.end(), '\n'));
                        wire = d + "\x1b[" + std::to_string(nl + 1) + ";1H\x1b[0m" + floating;
                        diffed = true;
                    }
                }
            } else prev_lines.clear();
            if (!diffed) {
                if (main_len != std::string::npos && frame_str.compare(0, 3, "\x1b[H") != 0) {
                    const size_t h = frame_str.find("\x1b[H");   // "\x1b[2J\x1b[H": remember the lines for the next diff
                    std::string ignore;
                    if (h != std::string::npos && h + 3 <= main_len) diff_frame_lines(frame_str.substr(h + 3, main_len - h - 3), prev_lines, ignore);
                }
                wire = frame_str;
            }
            // Style Option overlay: a whole frame (another row selected) writes the screen's text again, also in the picture's cells
            // (Sixel shows that text until the picture is sent): the pictures go out again in this same write.
            if (sty_frozen && sixel_on && !diffed) { gfx_.fresh = true; if (gfx2_.active) gfx2_.fresh = true; }
            // A floating panel (a question, the "<" menu, a small overlay) came or went: the text it left over the picture's cells
            // must be wiped. Sixel keeps its picture only while the cells are skipped, so then the cells are written as spaces and
            // the picture is sent again in the same write (nothing shows in between).
            // (under the Style Option overlay a change inside the panel -- another row selected -- leaves the picture's cells as they are)
            const bool floating_changed = floating != last_floating && !(sty_frozen && !floating.empty() && !last_floating.empty());
            // A panel came, went or moved over the main screen (the spectrogram options, a menu ...): the spectrogram picture is drawn again
            // completely in this same write. The tiles it touches are sent anyway, but a terminal may also drop parts of a picture next
            // to the text that was rewritten, and a tile that did not change (a paused track) would never be sent again.
            {
                static int last_rect[4] = {0, 0, 0, 0};
                if ((floating_changed && std::memcmp(last_rect, float_rect_, sizeof(last_rect)) != 0) || (heal && !floating.empty() && !sty_frozen)) { spectro_gfx_.invalidate(); creator_spectro_gfx_.invalidate(); }
                std::memcpy(last_rect, float_rect_, sizeof(last_rect));
            }
            if (sixel_on && floating_changed && gfx_.active) { gfx_.fresh = true; if (gfx2_.active) gfx2_.fresh = true; }   // (the second picture's cells are blanked too: it goes out again in the same write)
            last_floating = floating;
            last_frame_str = std::move(frame_str);
            last_frame_written_at = frame_now;
            gfx_fill_holes(wire, sixel_on && !(floating_changed && gfx_.active));
        }
        // The scope image goes after the text (Sixel replaces the cells; a Kitty image sits under the text and is
        // replaced in place). The text frame is often unchanged and then not sent at all, the picture still is.
        {
            ++gfx_tick;
            const int cap = gfx_picture_cap(gfx_proto_, settings_.image_scale);   // Kitty 60, Sixel 30 (full) / 60 (half, third)
            const unsigned long every = static_cast<unsigned long>(std::max(1, (settings_.osci().frame_rate + cap / 2) / cap));
            gfx_due_ = (gfx_tick % every) == 0;
            if (gfx_ok_ && gfx_.active && gfx_proto_ != GfxProto::None) {
                const bool visible = gfx_.crop < gfx_.cols;
                if (visible && (gfx_.fresh || !gfx_shown)) {
                    wire += gfx_emit(gfx_proto_, gfx_);
                    gfx_shown = true;
                    // the oscilloscope picture covers its cells: the caption box goes on top of it again
                    if (caption_rect_[2] > 0 && !caption_text_.empty())
                        wire += "\x1b[" + std::to_string(caption_rect_[0] + 1) + ";" + std::to_string(caption_rect_[1] + 1) + "H" + caption_text_ + "\x1b[0m";
                }
                else if (!visible && gfx_shown) { wire += gfx_clear(gfx_proto_); gfx_shown = false; }
            } else if (gfx_shown) {
                wire += gfx_clear(gfx_proto_);
                gfx_shown = false;
                // Sixel has no delete: the picture stays until its cells are written over, and a line diff that finds the cells
                // unchanged (spaces before, spaces now -- the Settings cover preview) never rewrites them. Send the next frame whole.
                if (gfx_proto_ == GfxProto::Sixel) {
                    last_frame_str.clear();
                    prev_lines.clear();
                }
            }
            if (gfx_proto_ == GfxProto::None) gfx_shown = false;
            // the overlay's second picture (the XY box) goes with the first one
            if (gfx_ok_ && gfx_.active && gfx2_.active && gfx_proto_ != GfxProto::None) {
                if (gfx2_.crop >= gfx2_.cols) {
                    if (gfx2_shown_) { wire += gfx_clear(gfx_proto_, 1); gfx2_shown_ = false; }
                } else if (gfx2_.fresh || !gfx2_shown_) { wire += gfx_emit(gfx_proto_, gfx2_); gfx2_shown_ = true; }
            } else if (gfx2_shown_) {
                wire += gfx_clear(gfx_proto_, 1);
                gfx2_shown_ = false;
                // Sixel has no way to delete a picture: the overlay's two pictures stay on screen until their cells are written
                // over. The main screen's line diff and the picture holes would leave them, so the next frame is a full one
                // and the main picture is sent again (it erases its own cells first).
                if (gfx_proto_ == GfxProto::Sixel) {
                    last_frame_str.clear();
                    prev_lines.clear();
                    gfx_shown = false;
                }
            }
        }
        if (sp_img) {
            int cw = 10, chh = 20;
            gfx_cell_pixels(settings_.cell_pixels, cw, chh);
            const bool floating = float_rect_[2] > 0 && float_rect_[3] > 0;
            const bool caption = !floating && caption_rect_[2] > 0;   // the "fetching lyrics" box: a hole in the picture
            SpectroGfx& sgfx = sp_creator ? creator_spectro_gfx_ : spectro_gfx_;
            wire += sgfx.emit(sp_proto, sp_creator ? creator_spectro_set_ : settings_.spectro, sp_creator ? creator_spectro_ : spectro(), spectro_area_[0], spectro_area_[1], spectro_area_[2], spectro_area_[3],
                                      cw, chh, true, floating ? float_rect_[1] : caption ? caption_rect_[0] : 0,
                                      floating ? float_rect_[0] : caption ? caption_rect_[1] : 0,
                                      floating ? float_rect_[3] : caption ? 1 : 0, floating ? float_rect_[2] : caption ? caption_rect_[2] : 0);
        } else {
            if (spectro_gfx_.shown()) wire = spectro_gfx_.clear(sp_proto) + wire;   // before the new screen's text
            if (creator_spectro_gfx_.shown()) wire = creator_spectro_gfx_.clear(sp_proto) + wire;
        }
        if (pv_img) {
            int cw = 10, chh = 20;
            gfx_cell_pixels(settings_.cell_pixels, cw, chh);
            wire += pv_spectro_gfx_.emit(sp_proto, pv_spectro_set_, *settings_preview_spectro(), pv_spectro_area_[0], pv_spectro_area_[1], pv_spectro_area_[2], pv_spectro_area_[3],
                                         cw, chh, true, 0, 0, 0, 0);
        } else if (pv_spectro_gfx_.shown()) {
            wire = pv_spectro_gfx_.clear(sp_proto) + wire;
        }
        if (!wire.empty()) write_frame(wire);
        // 25fps (was 12.5fps) — the 700ms waveform reveal animation only
        // got ~9 frames to work with at the old 80ms cadence, which
        // showed as a handful of visible ~11% jumps rather than a smooth
        // continuous expansion. Also smooths disk rotation and the
        // visualizer's motion generally. Text-frame rendering is cheap
        // enough that doubling the rate here is not a meaningful CPU/
        // battery concern.
        {   // sleep in short slices so a changed terminal size is picked up at once, not after a whole frame
            const auto until = frame_start + std::chrono::microseconds(1000000 / std::max(1, settings_.osci().frame_rate));
            const int r0 = term.rows(), c0 = term.cols();
            while (std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                if (term.rows() != r0 || term.cols() != c0) break;
            }
        }
    }

    if (gfx_shown) { write_frame(gfx_clear(gfx_proto_)); gfx_shown = false; }   // the picture must not outlive the mode
    if (gfx2_shown_) { write_frame(gfx_clear(gfx_proto_, 1)); gfx2_shown_ = false; }
    if (spectro_gfx_.shown()) write_frame(spectro_gfx_.clear(gfx_proto_ == GfxProto::Kitty ? 1 : 2));
    if (creator_spectro_gfx_.shown()) write_frame(creator_spectro_gfx_.clear(gfx_proto_ == GfxProto::Kitty ? 1 : 2));
    if (pv_spectro_gfx_.shown()) write_frame(pv_spectro_gfx_.clear(gfx_proto_ == GfxProto::Kitty ? 1 : 2));
    if (switch_mode_) {
        // Switching to the radio: the player is only SUSPENDED (kept in memory, so coming back is instant and shows
        // the very same screen). Playback is paused, the terminal is handed over without a flash of the shell.
        if (has_track_ && !player_.is_paused()) player_.pause();
        terminal_hold_alt_screen();
        term.restore();
        save_settings(settings_);
        if (settings_.autosave_enabled && !audiobook_) save_snapshot(build_snapshot());
        suspended_ = true;
        remote::stop();
        return kExitSwitchMode;
    }
    remote::stop();
    term.restore();
    shutdown();
    std::cout << "\nbye.\n";
    return 0;
}

// Everything that has to happen when the player really ends (quit, or quit from the radio while the player was
// suspended): close the play in the history, stop the audio, save, join the threads.
bool App::wait_background(int ms) {
    bg_->quit.store(true);                                  // the long loops stop at their next file
    if (current_pcm_) current_pcm_->cancelled.store(true);  // a decode that still runs lets go of its file
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (bg_->running.load() > 0 && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const int left = bg_->running.load();
    if (left > 0) ConsoleLog::instance().log_basic("quit: " + std::to_string(left) + " background task(s) still busy -- left to end with the program");
    return left == 0;
}

void App::shutdown() {
    if (suspended_) sync_after_radio();   // quit from the radio: its rows may have changed them
    suspended_ = false;
    history_end_current_play();
    history_.save();
    device_stop();          // closed by the thread that opened it
    stop_device_worker();
    save_settings(settings_);
    if (settings_.autosave_enabled && !audiobook_) {
        save_snapshot(build_snapshot());
        ConsoleLog::instance().log_basic("saved session snapshot on exit");
    }
    if (convert_overlay_open_) convert_overlay_close();
    converter_.cancel();
    if (vd_.active) { std::lock_guard<std::mutex> lk(vd_.child_m); if (vd_.child) vd_.child->kill(); }
    if (vd_.thread.joinable()) vd_.thread.join();
    video_fetch_cancel_and_join();
    if (load_thread_.joinable()) load_thread_.join();
    if (search_thread_.joinable()) search_thread_.join();
    if (device_worker_thread_.joinable()) device_worker_thread_.join();
    if (bulk_add_thread_.joinable()) bulk_add_thread_.join();
    meta_fetch_cancel_.request();   // quitting must not wait for a long batch; what had arrived is kept below
    if (meta_fetch_thread_.joinable()) meta_fetch_thread_.join();
    poll_pending_meta_fetch();
    meta_persist();
    cf_close();
}

} // namespace muisc
