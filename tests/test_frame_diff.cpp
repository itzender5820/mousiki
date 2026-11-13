// diff_frame_lines() sends only the part of a changed line that differs (terminal_ui.h). A wrong column or a colour that is not set again would corrupt the
// screen for good (there is no full redraw while the screen moves), so the result is checked against a small terminal: the screen after the short
// output must be the screen after the whole changed lines, for many random frames.
#include <cstdint>
#include <string>
#include <vector>

#include "terminal_ui.h"
#include "test_framework.h"

using namespace muisc;

namespace {

struct Cell {
    std::string ch = " ";
    std::string style;
    bool cont = false;   // the right half of a wide glyph
    bool operator==(const Cell& o) const { return ch == o.ch && style == o.style && cont == o.cont; }
};

// Just enough terminal: the cursor position (CUP), cursor forward (CUF), SGR (the colours in force = the sequences since the last reset), glyphs of one
// column, wide glyphs (two), and combining marks (they join the glyph before them).
struct Screen {
    int w, h;
    std::vector<Cell> cells;
    int row = 0, col = 0;
    std::string style;
    Screen(int w_, int h_) : w(w_), h(h_), cells(static_cast<size_t>(w_) * h_) {}
    Cell& at(int r, int c) { return cells[static_cast<size_t>(r) * w + c]; }
    void blank(int r, int c) { at(r, c) = Cell(); }
    void put(const std::string& ch, int width) {
        if (row < 0 || row >= h || col < 0 || col + width > w) { col += width; return; }
        // writing over one half of a wide glyph blanks the other half
        for (int k = 0; k < width; ++k) {
            const int c = col + k;
            if (at(row, c).cont && c > 0) blank(row, c - 1);
            if (c + 1 < w && at(row, c + 1).cont && k == width - 1) blank(row, c + 1);
        }
        at(row, col).ch = ch; at(row, col).style = style; at(row, col).cont = false;
        if (width == 2) { at(row, col + 1).ch.clear(); at(row, col + 1).style = style; at(row, col + 1).cont = true; }
        col += width;
    }
    void feed(const std::string& d) {
        size_t i = 0;
        while (i < d.size()) {
            const unsigned char c = static_cast<unsigned char>(d[i]);
            if (c == 0x1B && i + 1 < d.size() && d[i + 1] == '[') {
                size_t j = i + 2;
                while (j < d.size() && !(d[j] >= 0x40 && d[j] <= 0x7E)) ++j;
                const std::string params = d.substr(i + 2, j - i - 2);
                const char fin = d[j];
                if (fin == 'H') {
                    const size_t semi = params.find(';');
                    row = (semi == std::string::npos ? std::stoi(params.empty() ? "1" : params) : std::stoi(params.substr(0, semi))) - 1;
                    col = (semi == std::string::npos ? 1 : std::stoi(params.substr(semi + 1))) - 1;
                } else if (fin == 'm') {
                    if (params.empty() || params == "0") style.clear();
                    else style += d.substr(i, j - i + 1);
                } else if (fin == 'C') {
                    col += params.empty() ? 1 : std::stoi(params);
                }
                i = j + 1;
                continue;
            }
            size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            const std::string g = d.substr(i, len);
            uint32_t cp = c < 0x80 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
            for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(d[i + k]) & 0x3Fu);
            if (cp >= 0x300 && cp <= 0x36F) {   // joins the glyph before it
                int cc = col - 1;
                if (cc >= 0 && cc < w && row >= 0 && row < h) {
                    if (at(row, cc).cont && cc > 0) --cc;
                    at(row, cc).ch += g;
                }
            } else {
                put(g, (cp >= 0x3000 && cp <= 0x9FFF) ? 2 : 1);
            }
            i += len;
        }
    }
};

struct Glyph { std::string text; int width; };
struct LCell { Glyph g; int style; };

uint32_t rng_state = 1;
uint32_t rnd() { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

const char* kStyles[] = {"", "\x1b[38;2;10;20;30m", "\x1b[38;2;200;100;50m", "\x1b[38;5;99m", "\x1b[1m", "\x1b[1m\x1b[38;2;1;2;3m"};
constexpr int kNumStyles = 6;

std::string utf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    return s;
}

Glyph random_glyph(bool allow_wide) {
    const uint32_t r = rnd() % 100;
    if (r < 25) return {std::string(1, static_cast<char>('a' + rnd() % 26)), 1};
    if (r < 40) return {" ", 1};
    if (r < 50) return {utf8(0x2500 + rnd() % 0x80), 1};
    if (r < 75) return {utf8(0x2800 + rnd() % 256), 1};
    if (r < 82) return {std::string(1, static_cast<char>('a' + rnd() % 26)) + utf8(0x300 + rnd() % 8), 1};   // a letter with an accent
    if (allow_wide && r < 92) return {utf8(0x3042 + rnd() % 20), 2};
    return {utf8(0x2800 + rnd() % 256), 1};
}

// `n` columns of random glyphs
void fill(std::vector<LCell>& v, int n, int style_mode) {
    int left = n;
    int style = static_cast<int>(rnd() % kNumStyles);
    while (left > 0) {
        if (rnd() % 100 < static_cast<uint32_t>(style_mode)) style = static_cast<int>(rnd() % kNumStyles);
        Glyph g = random_glyph(left >= 2);
        if (g.width > left) g = {" ", 1};
        v.push_back({g, style});
        left -= g.width;
    }
}

std::string render(const std::vector<LCell>& v) {
    std::string s;
    int cur = 0;   // the style in force (0 = none)
    for (const LCell& c : v) {
        if (c.style != cur) {
            s += "\x1b[0m";
            s += kStyles[c.style];
            cur = c.style;
        }
        s += c.g.text;
    }
    return s + "\x1b[0m";
}

std::string body_of(const std::vector<std::string>& lines) {
    std::string b;
    for (size_t i = 0; i < lines.size(); ++i) { b += lines[i]; if (i + 1 < lines.size()) b += "\n"; }
    return b;
}

}  // namespace

TEST(frame_diff_gives_the_same_screen_as_sending_the_changed_lines_whole) {
    const int W = 40, H = 7;
    long saved_steps = 0, steps = 0, total_diff = 0, total_full = 0;
    for (uint32_t seed = 1; seed <= 400; ++seed) {
        rng_state = seed * 7919u;
        std::vector<std::vector<LCell>> frame(H);
        for (auto& line : frame) fill(line, W, 25);
        std::vector<std::string> lines(H);
        for (int r = 0; r < H; ++r) lines[r] = render(frame[r]);
        Screen s_full(W, H), s_diff(W, H);
        for (int r = 0; r < H; ++r) {
            const std::string whole = "\x1b[" + std::to_string(r + 1) + ";1H\x1b[0m" + lines[r];
            s_full.feed(whole);
            s_diff.feed(whole);
        }
        std::vector<std::string> prev = lines;
        for (int step = 0; step < 30; ++step) {
            // change a few lines
            std::vector<std::string> next = prev;
            const int nchange = 1 + static_cast<int>(rnd() % 3);
            for (int k = 0; k < nchange; ++k) {
                const int r = static_cast<int>(rnd() % H);
                auto& cells = frame[r];
                switch (rnd() % 5) {
                    case 0: {   // new glyphs in a few cells (a cell is replaced by cells of the same width)
                        const int n = 1 + static_cast<int>(rnd() % 4);
                        for (int q = 0; q < n; ++q) {
                            const size_t i = rnd() % cells.size();
                            const int w0 = cells[i].g.width;
                            if (w0 == 1) cells[i].g = random_glyph(false);
                            else { Glyph g = random_glyph(true); if (g.width == 1) g = {" ", 1}; cells[i].g = g; }
                        }
                        break;
                    }
                    case 1: {   // another style over a span
                        size_t a = rnd() % cells.size(), b = rnd() % cells.size();
                        if (a > b) std::swap(a, b);
                        const int st = static_cast<int>(rnd() % kNumStyles);
                        for (size_t i = a; i <= b; ++i) cells[i].style = st;
                        break;
                    }
                    case 2: {   // a run of cells replaced by new ones of the same total width
                        size_t a = rnd() % cells.size(), len = 1 + rnd() % 8;
                        if (a + len > cells.size()) len = cells.size() - a;
                        int width = 0;
                        for (size_t i = a; i < a + len; ++i) width += cells[i].g.width;
                        std::vector<LCell> repl;
                        fill(repl, width, 40);
                        cells.erase(cells.begin() + static_cast<long>(a), cells.begin() + static_cast<long>(a + len));
                        cells.insert(cells.begin() + static_cast<long>(a), repl.begin(), repl.end());
                        break;
                    }
                    case 3: {   // the whole line
                        cells.clear();
                        fill(cells, W, 25);
                        break;
                    }
                    default: {  // one glyph at the start or at the end (a border)
                        const size_t i = (rnd() & 1) ? 0 : cells.size() - 1;
                        if (cells[i].g.width == 1) cells[i].g = random_glyph(false);
                        break;
                    }
                }
                next[r] = render(cells);
            }
            std::string whole_out, diff_out;
            for (int r = 0; r < H; ++r)
                if (next[r] != prev[r]) whole_out += "\x1b[" + std::to_string(r + 1) + ";1H\x1b[0m" + next[r];
            std::vector<std::string> prev_for_diff = prev;
            CHECK(diff_frame_lines(body_of(next), prev_for_diff, diff_out));
            CHECK(prev_for_diff == next);   // (the lines it remembers are the whole lines)
            s_full.feed(whole_out);
            s_diff.feed(diff_out);
            const bool same = s_full.cells == s_diff.cells;
            if (!same) {   // (one line of detail for the first failure)
                for (int r = 0; r < H && !same; ++r)
                    for (int c = 0; c < W; ++c)
                        if (!(s_full.at(r, c) == s_diff.at(r, c))) { CHECK_EQ(r * 1000 + c, -1); break; }
            }
            CHECK(same);
            CHECK(diff_out.size() <= whole_out.size());
            ++steps;
            total_diff += static_cast<long>(diff_out.size());
            total_full += static_cast<long>(whole_out.size());
            if (diff_out.size() < whole_out.size()) ++saved_steps;
            prev = next;
        }
    }
    CHECK(saved_steps * 2 > steps);              // (the test is not empty: most steps are shorter)
    CHECK(total_diff < total_full);              // ... and the total is smaller (random frames: most lines change all over, so not by much)
}

TEST(frame_diff_sends_one_changed_glyph_at_its_column) {
    std::vector<std::string> prev;
    std::string out;
    const std::string a = "\x1b[0m\x1b[38;2;9;9;9m| hello world |\x1b[0m", b = "\x1b[0m\x1b[38;2;9;9;9m| hellO world |\x1b[0m";
    diff_frame_lines(a + "\n" + a, prev, out);   // (first call: nothing remembered, shape differs)
    CHECK(out.empty());
    CHECK(diff_frame_lines(a + "\n" + b, prev, out));
    // the second line: the cursor goes to column 7 (the "O" is the 7th glyph), the colour is set again, only the "O" follows
    CHECK_EQ(out, std::string("\x1b[2;7H\x1b[0m\x1b[38;2;9;9;9mO\x1b[0m"));
}

TEST(frame_diff_whole_lines_sends_every_changed_line_whole_and_still_remembers_the_frame) {
    std::vector<std::string> prev;
    std::string out;
    const std::string a = "\x1b[0m\x1b[38;2;9;9;9m| hello world |\x1b[0m", b = "\x1b[0m\x1b[38;2;9;9;9m| hellO world |\x1b[0m";
    diff_frame_lines(a + "\n" + a, prev, out);
    CHECK(diff_frame_lines(a + "\n" + b, prev, out, true));
    CHECK_EQ(out, "\x1b[2;1H\x1b[0m" + b);   // (the whole line, not the one glyph)
    out.clear();
    CHECK(diff_frame_lines(a + "\n" + b, prev, out, true));   // unchanged lines are still left alone
    CHECK(out.empty());
    CHECK(diff_frame_lines(a + "\n" + a, prev, out));   // and the next part-wise diff builds on what was sent
    CHECK_EQ(out, std::string("\x1b[2;7H\x1b[0m\x1b[38;2;9;9;9mo\x1b[0m"));
}

TEST(frame_diff_sends_a_line_whole_when_it_has_another_escape_sequence_or_a_wide_glyph_in_front) {
    std::vector<std::string> prev;
    std::string out;
    const std::string hole_a = "ab\x1b[3Ccd efg", hole_b = "ab\x1b[3Ccd efX";   // (a cursor move: the Sixel picture's cells)
    diff_frame_lines(hole_a, prev, out);
    out.clear();
    diff_frame_lines(hole_b, prev, out);
    CHECK_EQ(out, "\x1b[1;1H\x1b[0m" + hole_b);
    const std::string wide_a = "\xe3\x81\x82 hello", wide_b = "\xe3\x81\x82 hellX";   // "あ hello": the wide glyph in front is not counted as one column
    prev.clear(); out.clear();
    diff_frame_lines(wide_a, prev, out);
    diff_frame_lines(wide_b, prev, out);
    CHECK_EQ(out, "\x1b[1;1H\x1b[0m" + wide_b);
}

TEST(frame_diff_leaves_identical_lines_alone_and_asks_for_a_full_frame_when_the_shape_changes) {
    std::vector<std::string> prev;
    std::string out;
    diff_frame_lines("one\ntwo\nthree", prev, out);
    out.clear();
    CHECK(diff_frame_lines("one\ntwo\nthree", prev, out));
    CHECK(out.empty());
    CHECK(!diff_frame_lines("one\ntwo", prev, out));   // another number of lines: the caller sends the whole frame
    CHECK(out.empty());
    CHECK_EQ(prev.size(), 2u);
}

TEST(frame_diff_sends_a_small_part_of_a_line_whose_middle_animates) {
    // a row of the main screen: border, blank space, the turning disk (braille in colours), the fixed text, the border
    const int W = 140;
    Screen s_full(W, 3), s_diff(W, 3);
    std::vector<std::string> prev;
    auto frame = [&](int t) {
        std::vector<std::string> lines;
        for (int r = 0; r < 3; ++r) {
            std::string l = "\x1b[0m\x1b[38;2;80;80;80m\xe2\x94\x82\x1b[0m    ";   // "│" and 4 spaces
            for (int c = 0; c < 12; ++c) {   // (a colour for every third glyph: as the disk's rows are)
                if (c % 3 == 0) l += "\x1b[38;2;" + std::to_string((t * 7 + c * 5 + r * 11) % 256) + ";120;" + std::to_string(200 - (t + c) % 100) + "m";
                l += utf8(0x2800 + (t * 13 + c * 29 + r * 3) % 256);
            }
            l += "\x1b[0m       File      : Song One" + std::string(W - 5 - 12 - 7 - 18 - 1, ' ') + "\x1b[38;2;80;80;80m\xe2\x94\x82\x1b[0m";
            lines.push_back(l);
        }
        return lines;
    };
    std::vector<std::string> cur = frame(0);
    for (int r = 0; r < 3; ++r) {
        const std::string whole = "\x1b[" + std::to_string(r + 1) + ";1H\x1b[0m" + cur[r];
        s_full.feed(whole);
        s_diff.feed(whole);
    }
    std::string sink;
    diff_frame_lines(body_of(cur), prev, sink);
    size_t diff_bytes = 0, full_bytes = 0;
    for (int t = 1; t <= 40; ++t) {
        const std::vector<std::string> next = frame(t);
        std::string out, whole;
        for (int r = 0; r < 3; ++r) if (next[r] != cur[r]) whole += "\x1b[" + std::to_string(r + 1) + ";1H\x1b[0m" + next[r];
        CHECK(diff_frame_lines(body_of(next), prev, out));
        s_full.feed(whole);
        s_diff.feed(out);
        CHECK(s_full.cells == s_diff.cells);
        diff_bytes += out.size();
        full_bytes += whole.size();
        cur = next;
    }
    CHECK(diff_bytes * 100 < full_bytes * 60);   // (the border, the blanks and the fixed text are not sent again)
}
