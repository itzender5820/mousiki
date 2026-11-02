// Console (log) screen and the karaoke screen with its big-lyrics renderer. Split out of app.cpp.
// Member functions of App (declared in app.h); the helpers shared with app.cpp are in app_internal.h.
#include "app.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
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
#include "app_internal.h"

namespace muisc {

void App::build_console_screen(std::ostringstream& frame, int W, int target_height) const {
    std::string border = ansi_for(settings_.border_color, false);
    // Two panes: the log (what the app ran) above and the status messages (with their times) below. Together exactly as tall as the
    // player view (target_height). The status pane has 8 rows; on a small terminal it gets what is left after 3 log rows, and
    // below 2 rows it is left out.
    const int avail = target_height - 4;                       // minus the borders of the two boxes
    int log_rows = std::max(3, avail - 8);
    int status_rows = avail - log_rows;
    const bool two = status_rows >= 2;
    if (!two) { log_rows = std::max(1, target_height - 2); status_rows = 0; }

    // A message longer than the pane is wrapped onto more rows instead of being cut off; the rows after the first are indented to
    // sit under the text after the "[hh:mm:ss] " time.
    auto wrapped = [&](const std::vector<std::string>& in) {
        const int inner = std::max(10, W - 4);
        std::vector<std::string> out;
        for (const std::string& s : in) {
            if (display_width(s) <= inner || s.find('\x1b') != std::string::npos) { out.push_back(s); continue; }
            size_t ind = 0;
            if (s.size() > 3 && s[0] == '[') { const size_t c = s.find("] "); if (c != std::string::npos && c <= 12) ind = c + 2; }
            const std::vector<std::string> first = wrap_lines(s, inner, 40);
            if (ind == 0 || first.size() < 2 || static_cast<int>(ind) > inner / 2) { for (const std::string& l : first) out.push_back(l); continue; }
            // the first row as it is, the rest of the text wrapped to the narrower width under it
            std::string head = first[0];
            size_t at = head.size();
            while (at < s.size() && s[at] == ' ') ++at;
            out.push_back(head);
            for (const std::string& l : wrap_lines(s.substr(at), inner - static_cast<int>(ind), 40)) out.push_back(std::string(ind, ' ') + l);
        }
        return out;
    };
    auto pane = [&](const std::string& title, const std::vector<std::string>& lines_in, int rows, int pane_no, const std::string& hint) {
        const std::vector<std::string> lines = wrapped(lines_in);
        const int total = static_cast<int>(lines.size());
        const int max_up = std::max(0, total - rows);
        int& up = console_up_[pane_no];
        up = std::clamp(up, 0, max_up);                         // (a pane that is not following the newest keeps its place)
        const int start = max_up - up;
        const bool focus = two && console_pane_ == pane_no;
        frame << box_top(title + (focus ? " \xE2\x97\x80" : ""), W, border) << "\n";
        for (int r = 0; r < rows; ++r) {
            const int idx = start + r;
            frame << box_line(idx < total ? lines[static_cast<size_t>(idx)] : std::string(), W, border) << "\n";
        }
        std::string foot = hint;
        if (up > 0) foot = hint.empty() ? "scrolled up " + std::to_string(up) : "scrolled up " + std::to_string(up) + " | " + hint;
        frame << box_bottom(W, foot, border) << "\n";
    };
    pane("CONSOLE / LOGS", ConsoleLog::instance().lines(), log_rows, 0,
         two ? "" : "UP/DOWN scroll | HOME / END | [t / ESC] close");   // (with two panes the one legend sits under the second)
    if (two) pane("STATUS MESSAGES", status_history_, status_rows, 1, W >= 90 ? "[TAB] other pane | UP/DOWN scroll (SHIFT: page) | HOME / END | [t / ESC] close" : "[TAB] pane | UP/DOWN | HOME/END | [t] close");
}

// ---------------------------------------------------------------------
// Cheatsheet overlay (HKeyCheatsheet)
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Karaoke overlay (Mode::Karaoke, HKeyKaraoke = "k")
// ---------------------------------------------------------------------

// The karaoke art on the left (braille, 30 rows x 28 cells). Blank U+2800
// cells stay uncoloured; everything else wears the disk colours with a
// moving colour wave, like the radio's ON AIR sign.
static constexpr int kKaraokeArtW = 28;
static constexpr int kKaraokeArtRows = 30;
static const char* const kKaraokeArt[kKaraokeArtRows] = {
    "⠀⠀⠀⢠⡞⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⢦⠀⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⢀⡠⠤⡆⠀⠀⠀⠀⠀⠀⠀⢀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⢸⠀⢀⡇⠀⠀⠀⠀⠀⠀⢤⠜⠣⡤⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⢀⡤⣼⠀⠻⠃⠀⠀⠀⠀⠀⠀⠠⠕⠺⠄⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠈⠉⠁⠀⠀⠀⠀⣿⣿⡆⠀⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⢰⣿⣿⣿⣿⣿⣿⣿⣿⣷⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠉⠉⣿⣿⡿⠉⢹⣿⣿⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⢀⣼⣿⡟⠁⠀⢸⣿⡿⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠸⣿⡿⠋⠀⢾⣿⣿⣿⠃⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠉⠉⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⢰⣿⣿⣿⣿⣿⣿⡷⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⢀⣤⣤⣤⣤⣤⣤⣤⣤⡄⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠈⠛⠛⠛⠛⠛⢻⣿⣿⠇⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⣀⣴⣾⣿⠏⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠰⣿⣿⠿⠋⠁⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠀⢠⣤⣄⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⢀⣤⣤⣤⣤⣼⣿⣿⣤⣤⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠈⠛⠛⢻⣿⣿⣼⣿⠛⠛⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⢀⣴⣿⣿⢿⣿⣿⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠸⣿⡿⠛⣡⣼⣿⡟⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠛⠛⠛⠁⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⣰⣿⣷⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⣰⣿⣿⣿⣿⣿⣿⣿⣿⠆⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⢾⣿⡟⠁⠀⣹⣿⡏⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠉⠀⠀⣠⣿⣿⠁⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⣤⣾⣿⡿⠁⠀⠀⢰⠲⣄⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠈⠛⠁⠀⠀⠀⠀⠸⠀⡸⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣰⠒⡇⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠈⠉⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⠈⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠋⠀⠀⠀⠀⠀",
};

// Decodes UTF-8 into code points (invalid bytes become U+FFFD, which is not in the font).
static std::vector<char32_t> karaoke_codepoints(const std::string& s) {
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + static_cast<size_t>(len) > s.size()) { out.push_back(0xFFFD); ++i; continue; }
        char32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
        out.push_back(cp);
        i += static_cast<size_t>(len);
    }
    return out;
}

// One lyric line in BIG letters (karaoke lyrics size 2..5), drawn in braille.
// Latin text uses the built-in 5x7 pixel font (karaoke_font.h) scaled by
// `scale` = size - 1, so a letter is 3*scale columns wide and 2*scale rows
// tall. A line with characters that font lacks (Japanese, Chinese, Korean,
// Cyrillic, Greek ...) takes its glyphs from an installed font instead
// (glyph_raster.h); those are bigger, because a kanji needs about 16 dots to
// stay readable: the cell is 8*scale + 8 dots high, i.e. 4 / 6 / 8 / 10 rows
// for the sizes 2 / 3 / 4 / 5, and a full-width character is as many dots wide.
// Words wrap like the normal lyrics (a word with no spaces, like a Japanese
// line, breaks wherever the row is full), each row is centred in `width`
// columns, and the colours follow the same rules as render_lyric_line_wrapped()
// (sung words in the active-word colour, the word being sung bold, the rest of
// the active line in the active-line colour, other lines in the inactive colour).
// Returns false -- and leaves `out` alone -- when the line holds a character no
// font has (emoji, scripts that need shaping, or no CJK font installed); the
// caller then shows that line as normal text instead.
static bool render_big_lyric_line(const LyricLine& line, double elapsed, int width, bool is_active, int scale,
                                  const Settings& st, std::vector<std::string>& out) {
    struct Word { std::vector<char32_t> cps; double t; bool has_ts; };
    std::vector<Word> words;
    if (!line.words.empty()) {
        for (const auto& wt : line.words) {
            std::istringstream iss(wt.second);
            std::string w;
            while (iss >> w) words.push_back({karaoke_codepoints(w), wt.first, true});
        }
    } else {
        std::istringstream iss(line.full_text);
        std::string w;
        while (iss >> w) words.push_back({karaoke_codepoints(w), line.start_time, false});
    }
    bool pixel_font = true;   // every character is in the built-in font
    for (const auto& w : words) for (char32_t cp : w.cps) if (!karaoke_glyph(cp)) pixel_font = false;
    if (words.empty()) { out.push_back(std::string(static_cast<size_t>(std::max(0, width)), ' ')); return true; }

    // Cell height in dots, and the glyph (width + dots) of every character of the line.
    const int cell_h = pixel_font ? 8 * scale : 8 * scale + 8;
    std::unordered_map<char32_t, RasterGlyph> glyphs;
    if (!pixel_font) {
        if (!glyph_font_available()) return false;
        for (const auto& w : words) for (char32_t cp : w.cps) {
            if (!glyphs.count(cp) && !glyph_rasterize(cp, cell_h, glyphs[cp])) return false;
        }
        glyphs.erase(U' ');
    }
    // The separator between two words: a pixel-font space, or a quarter of the cell height.
    const int space_w = pixel_font ? 6 * scale : std::max(3, cell_h / 4);
    auto char_w = [&](char32_t cp) { return pixel_font ? 6 * scale : glyphs.at(cp).w; };
    auto text_w = [&](const std::vector<char32_t>& v, size_t n) {
        int w = 0;
        for (size_t i = 0; i < n; ++i) w += char_w(v[i]);
        return w;
    };

    const int cap = std::max(2, width * 2);                // dots per output row (2 dots per braille column)
    // State per word: 0 inactive line, 1 active line (not sung yet), 2 sung, 3 being sung.
    int singing = -1;
    if (is_active) for (size_t i = 0; i < words.size(); ++i) if (words[i].has_ts && words[i].t <= elapsed) singing = static_cast<int>(i);
    auto state_of = [&](size_t i) -> int {
        if (!is_active) return 0;
        if (!words[i].has_ts) return 1;
        if (static_cast<int>(i) == singing) return 3;
        return words[i].t <= elapsed ? 2 : 1;
    };
    const std::string sgr[4] = {
        ansi_for(st.inactive_line_color) + bg_ansi_for(st.inactive_line_bg_color),
        ansi_for(st.active_line_color) + bg_ansi_for(st.active_line_bg_color),
        ansi_for(st.active_word_color) + bg_ansi_for(st.active_word_bg_color),
        "\x1b[1m" + ansi_for(st.active_word_color) + bg_ansi_for(st.active_word_bg_color),
    };

    // Word wrap into rows of at most `cap` dots (a word longer than a row is split where the row is full).
    struct Piece { std::vector<char32_t> cps; int state; bool space; };
    std::vector<std::vector<Piece>> rows(1);
    int used = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        std::vector<char32_t> rest = words[i].cps;
        while (!rest.empty()) {
            const int need = used + (used > 0 ? space_w : 0) + text_w(rest, rest.size());   // row length with this word
            if (need <= cap) {
                if (used > 0) { rows.back().push_back({{}, 0, true}); used += space_w; }
                rows.back().push_back({rest, state_of(i), false});
                used += text_w(rest, rest.size());
                rest.clear();
            } else if (used > 0) {
                rows.emplace_back(); used = 0;
            } else {
                size_t n = 0;
                int w = 0;
                while (n < rest.size() && w + char_w(rest[n]) <= cap) { w += char_w(rest[n]); ++n; }
                n = std::max<size_t>(n, 1);
                rows.back().push_back({std::vector<char32_t>(rest.begin(), rest.begin() + static_cast<long>(n)), state_of(i), false});
                rest.erase(rest.begin(), rest.begin() + static_cast<long>(n));
                rows.emplace_back(); used = 0;
            }
        }
    }
    if (rows.back().empty() && rows.size() > 1) rows.pop_back();

    for (const auto& row : rows) {
        // Pixel grid of the row: cell_h dots tall; each dot keeps its word's state (+1, 0 = off).
        int pw = 0;
        for (const auto& p : row) pw += p.space ? space_w : text_w(p.cps, p.cps.size());
        const int ph = cell_h;
        std::vector<int8_t> px(static_cast<size_t>(std::max(1, pw) * ph), 0);
        int x0 = 0;
        for (const auto& p : row) {
            if (p.space) { x0 += space_w; continue; }
            for (char32_t cp : p.cps) {
                if (pixel_font) {
                    const uint8_t* g = karaoke_glyph(cp);
                    for (int gy = 0; g && gy < 8; ++gy)
                        for (int gx = 0; gx < 5; ++gx)
                            if (g[gy] & (1 << (4 - gx)))
                                for (int sy = 0; sy < scale; ++sy)
                                    for (int sx = 0; sx < scale; ++sx)
                                        px[static_cast<size_t>((gy * scale + sy) * pw + x0 + gx * scale + sx)] = static_cast<int8_t>(p.state + 1);
                } else {
                    const RasterGlyph& g = glyphs.at(cp);
                    for (int gy = 0; gy < g.h && gy < ph; ++gy)
                        for (int gx = 0; gx < g.w; ++gx)
                            if (g.dots[static_cast<size_t>(gy * g.w + gx)])
                                px[static_cast<size_t>(gy * pw + x0 + gx)] = static_cast<int8_t>(p.state + 1);
                }
                x0 += char_w(cp);
            }
        }
        const int cols = (pw + 1) / 2;
        const int left = std::max(0, (width - cols) / 2);
        static const int kBit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
        for (int cy = 0; cy < (ph + 3) / 4; ++cy) {
            std::string line(static_cast<size_t>(left), ' ');
            std::string last;
            for (int cx = 0; cx < cols; ++cx) {
                int bits = 0, state = 0;
                for (int dy = 0; dy < 4; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int x = cx * 2 + dx, y = cy * 4 + dy;
                        if (x >= pw || y >= ph) continue;
                        const int v = px[static_cast<size_t>(y * pw + x)];
                        if (v) { bits |= kBit[dy][dx]; state = std::max(state, v); }
                    }
                if (!bits) {
                    if (!last.empty()) { line += "\x1b[0m"; last.clear(); }
                    line += ' ';
                    continue;
                }
                const std::string& a = sgr[state - 1];
                if (a != last) { line += "\x1b[0m" + a; last = a; }
                const int cp = 0x2800 + bits;
                line += static_cast<char>(0xE0 | (cp >> 12));
                line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                line += static_cast<char>(0x80 | (cp & 0x3F));
            }
            if (!last.empty()) line += "\x1b[0m";
            line += std::string(static_cast<size_t>(std::max(0, width - left - cols)), ' ');
            out.push_back(line);
        }
    }
    return true;
}

void App::build_karaoke_screen(std::ostringstream& frame, int W, int H) {
    const std::string R = "\x1b[0m";
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border + settings_.box_vertical + R;
    H = std::max(8, H);
    W = std::max(20, W);
    const int body_h = H - 4;                       // top border, bottom border, two legend rows
    const int inner = W - 4;

    // --- title: what is playing ---
    std::string title = "KARAOKE";
    if (has_track_) {
        std::string name = metadata_.name.empty() || metadata_.name == "-" ? path_utf8(current_path_.stem()) : metadata_.name;
        title += " · " + name;
        if (!metadata_.artist.empty() && metadata_.artist != "-") title += " – " + metadata_.artist;
    }
    title = shown_text(title);
    frame << box_top(truncate_str(title, std::max(10, W - 8)), W, border) << "\n";

    // --- layout: the art on BOTH sides, 4 columns between art and lyrics, the lyrics centred in between. Recomputed
    // every frame from the terminal size, so a resize simply re-flows everything. When the lyrics would get narrower
    // than 30 columns the two pictures are left out and the lyrics take the whole width.
    constexpr int kGap = 4;
    // Big Japanese / Chinese / Korean letters (sizes 4 and 5) need the room: with the pictures on both sides only
    // about 50 columns are left, which holds two or three letters per row, so the lyrics take the whole width then.
    bool wide_script = false;
    if (settings_.karaoke_lyrics_size >= 4) {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (lyrics_ready_ && lyrics_path_ == current_path_) {
            for (const auto& l : lyrics_result_.lines) {
                for (char32_t cp : karaoke_codepoints(shown_text(l.full_text))) {
                    if (cp >= 0x1100 && !karaoke_glyph(cp) && (cp <= 0x11FF || (cp >= 0x2E80 && cp <= 0xD7FF) || (cp >= 0xF900 && cp <= 0xFFEF))) { wide_script = true; break; }
                }
                if (wide_script) break;
            }
        }
    }
    const bool show_art = !wide_script && inner - 2 * (kKaraokeArtW + kGap) >= 30;
    const int side_w = show_art ? kKaraokeArtW + kGap : 0;     // art + gap, on each side
    const int lyr_w = inner - 2 * side_w;

    std::vector<std::string> art_rows(static_cast<size_t>(body_h), std::string(static_cast<size_t>(kKaraokeArtW), ' '));
    if (show_art) {
        // Wave from the left to the right edge: the gradient runs disk TOP colour -> BOTTOM colour across the art,
        // a ripple rides on it and moves to the right (0.25 cycles per second, the ON AIR sign's default).
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const float phase = static_cast<float>(std::fmod(now * 0.25, 1000.0));
        const std::string c_end = settings_.disk_color_gradient && !settings_.disk_color_end.empty()
                                  ? settings_.disk_color_end : settings_.disk_color;
        const int first = std::max(0, (kKaraokeArtRows - body_h) / 2);           // crop evenly on a short terminal
        const int top = std::max(0, (body_h - kKaraokeArtRows) / 2);             // centre on a tall one
        for (int y = first; y < kKaraokeArtRows && top + (y - first) < body_h; ++y) {
            std::string line, last;
            int x = 0;
            const char* p = kKaraokeArt[y];
            while (*p && x < kKaraokeArtW) {
                const unsigned char c0 = static_cast<unsigned char>(*p);
                const int len = c0 < 0x80 ? 1 : (c0 >> 5) == 0x6 ? 2 : (c0 >> 4) == 0xE ? 3 : 4;
                const std::string g(p, static_cast<size_t>(len));
                p += len;
                if (g == "⠀" || g == " ") { line += ' '; ++x; continue; }
                const float d = static_cast<float>(x) / static_cast<float>(kKaraokeArtW - 1);
                const float ripple = 0.5f + 0.5f * std::sin(6.2831853f * (d * 1.5f - phase));
                float t = 0.65f * d + 0.35f * ripple;
                t = std::round(std::clamp(t, 0.0f, 1.0f) * 32.0f) / 32.0f;
                const std::string a = gradient_ansi(settings_.disk_color, c_end, t);
                if (a != last) { line += a; last = a; }
                line += g;
                ++x;
            }
            if (!last.empty()) line += R;
            line += std::string(static_cast<size_t>(std::max(0, kKaraokeArtW - x)), ' ');
            art_rows[static_cast<size_t>(top + (y - first))] = line;
        }
    }

    // --- centre: the lyrics, karaoke style ---
    std::vector<std::string> lyr_rows(static_cast<size_t>(body_h), std::string(static_cast<size_t>(lyr_w), ' '));
    auto centred = [&](const std::string& text, const std::string& sgr) {
        const std::string t = truncate_str(text, lyr_w);
        const int pad = std::max(0, (lyr_w - display_width(t)) / 2);
        return sgr + std::string(static_cast<size_t>(pad), ' ') + t + R + std::string(static_cast<size_t>(std::max(0, lyr_w - pad - display_width(t))), ' ');
    };
    std::vector<LyricLine> lines;
    double delay = 0.0;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (lyrics_ready_ && lyrics_path_ == current_path_) {   // the lyrics engine runs for the overlay in any case
            lines = lyrics_result_.lines;
            latinize_lyrics(lines);
            delay = lyrics_result_.delay;
            message = lyrics_result_.message;
        }
    }
    const double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    if (!has_track_) {
        message = "Nothing is playing -- start a track and the lyrics appear here.";
    } else if (!lyrics_ready_ || lyrics_path_ != current_path_) {
        message = "fetching lyrics ...";
    } else if (lines.empty() && message.empty()) {
        message = "No lyrics found for this track.";
    }
    const int size = std::clamp(settings_.karaoke_lyrics_size, 1, 5);
    if (lines.empty()) {
        lyr_rows[static_cast<size_t>(body_h / 2)] = centred(message, legend_sgr(settings_));
    } else {
        Settings ks = settings_;          // karaoke look: centred, whole lines, the sung words highlighted
        ks.lyrics_alignment = 0;
        ks.lyrics_animation = 0;
        const double lt = elapsed - delay;
        bool synced = false;
        for (const auto& l : lines) if (l.start_time > 0.0) { synced = true; break; }
        int active = 0;
        if (synced) {
            for (size_t i = 0; i < lines.size(); ++i) { if (lines[i].start_time <= lt) active = static_cast<int>(i); else break; }
        } else if (total_sec_ > 0) {   // plain lyrics: scroll along with the song
            active = std::clamp(static_cast<int>(elapsed / static_cast<double>(total_sec_) * lines.size()), 0, static_cast<int>(lines.size()) - 1);
        }
        // One lyric line -> its rows: big letters from size 2 on (normal text for a line the font cannot draw).
        auto render_line = [&](int li) {
            const bool is_active = synced && li == active;
            const double t = (!synced && li == active) ? 1e12 : lt;   // no timing: the current line in the active colour
            const bool hl = is_active || (!synced && li == active);
            std::vector<std::string> rows;
            // A line that would be taller than the screen (a long Japanese line at size 5) steps down a size at a
            // time, and ends up as normal text when even the smallest big letters are too tall for it.
            const int max_rows = std::max(6, body_h);
            for (int sc = size - 1; size >= 2 && sc >= 1; --sc) {
                rows.clear();
                if (!render_big_lyric_line(lines[static_cast<size_t>(li)], t, lyr_w, hl, sc, ks, rows)) break;
                if (static_cast<int>(rows.size()) <= max_rows) return rows;
            }
            return render_lyric_line_wrapped(lines[static_cast<size_t>(li)], t, lyr_w, hl, ks);
        };
        // The lines around the active one, a blank row between two lines; the active one sits in the middle.
        std::vector<std::string> flat;
        int active_row = 0, active_count = 1;
        const int line_rows = size >= 2 ? 2 * (size - 1) + 1 : 2;   // rough height of one line incl. the gap
        const int reach = body_h / line_rows + 1;
        const int lo = std::max(0, active - reach), hi = std::min(static_cast<int>(lines.size()) - 1, active + reach);
        for (int li = lo; li <= hi; ++li) {
            auto rows = render_line(li);
            if (li == active) { active_row = static_cast<int>(flat.size()); active_count = static_cast<int>(rows.size()); }
            for (auto& r : rows) flat.push_back(std::move(r));
            flat.push_back(std::string(static_cast<size_t>(lyr_w), ' '));
        }
        const int start = active_row + active_count / 2 - body_h / 2;
        for (int r = 0; r < body_h; ++r) {
            const int idx = start + r;
            if (idx >= 0 && idx < static_cast<int>(flat.size())) lyr_rows[static_cast<size_t>(r)] = flat[static_cast<size_t>(idx)];
        }
    }

    const std::string gap(static_cast<size_t>(kGap), ' ');
    for (int r = 0; r < body_h; ++r) {
        frame << bar << " ";
        if (show_art) frame << art_rows[static_cast<size_t>(r)] << gap;
        frame << lyr_rows[static_cast<size_t>(r)];
        if (show_art) frame << gap << art_rows[static_cast<size_t>(r)];
        frame << " " << bar << "\n";
    }

    std::string footer;
    if (has_track_) footer = "[ " + format_mmss(elapsed) + " / " + format_mmss(static_cast<double>(total_sec_)) + " ]";
    if (std::chrono::steady_clock::now() < karaoke_flash_until_)
        footer += std::string(footer.empty() ? "" : "  ") + "[ lyrics size " + std::to_string(size) + " / 5 ]";
    frame << box_bottom(W, footer, border_bottom) << "\n";
    const std::string up = hotkey_text("HKeyKaraokeBigger", "*");
    const std::string down = hotkey_text("HKeyKaraokeSmaller", "_");
    const std::string size_keys = (up == "SHIFT and +" && down == "SHIFT and -") ? "SHIFT and +/-" : up + " / " + down;   // "[SHIFT and +/-]" rather than "[SHIFT and + / SHIFT and -]"
    // The legend takes the last TWO terminal rows (no information line here): this overlay's own keys first, then
    // what still works from the main player underneath.
    std::vector<LegendGroup> groups = {
        {"Overlay", {"[ESC/" + hotkey_text("HKeyKaraoke", "k") + "] Close", "[" + size_keys + "] Lyrics size\001Size", "[" + hotkey_text("HKeyLyricsTiming", "ALT+L") + "] Lyrics timing\001Timing", "[" + hotkey_text("HKeyRetryLyrics", "l") + "] Re-fetch lyrics\001Re-fetch", "[a] Add / Assign lyrics manually\001Add / Assign"}},
        {"Player", {"[" + hotkey_text("HKeyTogglePlayPause", "p") + "] Play/Pause", "[" + hotkey_text("HKeyPlayNextSong", "n") + "/" + hotkey_text("HKeyPlayPreviousSong", "b") + "] Next/Prev",
                    "[\u2190\u2192] Seek", "[" + hotkey_text("HKeyIncreaseVolume", "+") + "/" + hotkey_text("HKeyDecreaseVolume", "-") + "] Volume", "[" + hotkey_text("HKeyToggleMute", "x") + "] Mute"}},
    };
    const auto rows = layout_legend_tree(groups, W - 1, 2);
    for (size_t i = 0; i < rows.size(); ++i)
        frame << " " << paint_legend(settings_, rows, i, border, 0, true) << (i + 1 < rows.size() ? "\n" : "");
}

// ---------------------------------------------------------------------
// Help wizard (Mode::Wizard, HKeyWizard = SHIFT+w) -- see app.h for the
// state and src/wizard.h for the text, its markup and the key handling.
// This only draws what wizard::render() hands over.
// ---------------------------------------------------------------------


} // namespace muisc
