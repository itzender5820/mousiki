#include "terminal_ui.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace muisc {

static struct termios g_orig_termios;

TerminalIO::TerminalIO() {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    raw_mode_active_ = true;
    std::cout << "\x1b[?25l" << std::flush; // hide cursor
}

TerminalIO::~TerminalIO() { restore(); }

void TerminalIO::restore() {
    if (raw_mode_active_) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        std::cout << "\x1b[?25h" << std::flush;
        raw_mode_active_ = false;
    }
}

// Subprocesses we spawn are supposed to never touch our stdin at all
// (see process_util.cpp's run_capture() and waveform.cpp's ffmpeg
// fallback — both redirect the child's stdin to /dev/null specifically
// because of this). But that fix lives in the spawn call sites, and
// this is the one place that actually NEEDS raw+non-blocking mode to
// keep working no matter what: re-applying our own termios settings on
// every poll is cheap (one syscall, ~25x/sec) and means that even if
// something unexpected resets the terminal to canonical/line-buffered
// mode, we're never more than one frame away from correcting it,
// instead of the read() call silently becoming blocking and stalling
// the entire render loop until a keypress+Enter happens to satisfy it.
void TerminalIO::reassert_raw_mode() {
    if (!raw_mode_active_) return;
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

int TerminalIO::poll_key() {
    reassert_raw_mode();
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) return 0;

    if (c == '\x1b') {
        unsigned char seq[2] = {0, 0};
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return 27;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return 27;
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'A';
                case 'B': return 'B';
                case 'C': return 'C';
                case 'D': return 'D';
            }
        }
        return 27;
    }
    return c;
}

int TerminalIO::rows() const {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    return 40;
}

int TerminalIO::cols() const {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 155;
}

// Byte length of the UTF-8 codepoint starting at s[i].
static int utf8_seq_len(unsigned char lead) {
    if ((lead & 0x80) == 0x00) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1; // invalid lead byte — treat as 1 to avoid getting stuck
}

// Decodes the codepoint starting at s[i], advancing i past it.
static uint32_t utf8_decode(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int len = utf8_seq_len(c);
    len = static_cast<int>(std::min<size_t>(static_cast<size_t>(len), s.size() - i));
    uint32_t cp;
    if (len <= 1) {
        cp = c;
    } else {
        cp = c & (0xFF >> (len + 1));
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    i += static_cast<size_t>(len);
    return cp;
}

// Terminal display width of one codepoint: 0 (combining/control), 1
// (normal), or 2 (East Asian Wide/Fullwidth + most emoji blocks). This is
// what was missing before — a title containing CJK text or an emoji has
// codepoints that occupy 2 terminal columns each while still counting as
// a single codepoint, and the old codepoint-count-only width function
// undercounted those, throwing every column after it out of alignment
// (and, since rows are built by direct concatenation in a couple of
// places, bleeding into the panel next to it).
static int codepoint_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0; // control chars
    if ((cp >= 0x0300 && cp <= 0x036F) ||   // combining diacritical marks
        (cp >= 0x200B && cp <= 0x200F) ||   // zero-width space/joiners/marks
        cp == 0xFEFF) {
        return 0;
    }

    static const struct { uint32_t lo, hi; } wide_ranges[] = {
        {0x1100, 0x115F},    // Hangul Jamo
        {0x2E80, 0x303E},    // CJK Radicals / Kangxi / CJK punctuation
        {0x3041, 0x33FF},    // Hiragana..CJK compatibility
        {0x3400, 0x4DBF},    // CJK Extension A
        {0x4E00, 0x9FFF},    // CJK Unified Ideographs
        {0xA000, 0xA4CF},    // Yi
        {0xAC00, 0xD7A3},    // Hangul Syllables
        {0xF900, 0xFAFF},    // CJK Compatibility Ideographs
        {0xFE30, 0xFE4F},    // CJK Compatibility Forms
        {0xFF00, 0xFF60},    // Fullwidth Forms
        {0xFFE0, 0xFFE6},
        {0x1F300, 0x1F64F},  // Misc Symbols & Pictographs, Emoticons
        {0x1F680, 0x1F6FF},  // Transport & Map
        {0x1F900, 0x1F9FF},  // Supplemental Symbols & Pictographs
        {0x1FA70, 0x1FAFF},
        {0x20000, 0x3FFFD},  // CJK Extension B and beyond
    };
    for (const auto& r : wide_ranges) {
        if (cp >= r.lo && cp <= r.hi) return 2;
    }
    return 1;
}

int display_width(const std::string& s) {
    int cols = 0;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = utf8_decode(s, i);
        cols += codepoint_width(cp);
    }
    return cols;
}

// Longest byte-valid prefix of s whose total display WIDTH doesn't exceed
// `width` (stops before a wide character that would overflow it, rather
// than including it and busting the budget).
static std::string utf8_take(const std::string& s, int width) {
    std::string out;
    size_t i = 0;
    int used = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = utf8_decode(s, i);
        int w = codepoint_width(cp);
        if (used + w > width) break;
        out += s.substr(start, i - start);
        used += w;
    }
    return out;
}

std::string pad_right(const std::string& s, int width) {
    if (width <= 0) return "";
    int w = display_width(s);
    if (w >= width) return utf8_take(s, width);
    return s + std::string(width - w, ' ');
}

std::string pad_left(const std::string& s, int width) {
    if (width <= 0) return "";
    int w = display_width(s);
    if (w >= width) return utf8_take(s, width);
    return std::string(width - w, ' ') + s;
}

std::string truncate_str(const std::string& s, int width) {
    if (width <= 0) return "";
    int w = display_width(s);
    if (w <= width) return s;
    if (width <= 3) return utf8_take(s, width);
    return utf8_take(s, width - 3) + "...";
}

} // namespace muisc
