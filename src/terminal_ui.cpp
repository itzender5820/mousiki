#include "terminal_ui.h"
#include "indic_handler.h"
#include "utf8_util.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <conio.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif
#include <cwchar>

namespace muisc {

#if defined(_WIN32)
static DWORD g_orig_in_mode = 0;
static DWORD g_orig_out_mode = 0;
static UINT g_orig_in_cp = 0;
static UINT g_orig_out_cp = 0;
static bool g_win_modes_saved = false;

struct interval {
    uint32_t first;
    uint32_t last;
};

static bool bisearch(uint32_t ucs, const struct interval *table, int max) {
    int min = 0;
    int mid;
    if (ucs < table[0].first || ucs > table[max].last)
        return false;
    while (max >= min) {
        mid = (min + max) / 2;
        if (ucs > table[mid].last)
            min = mid + 1;
        else if (ucs < table[mid].first)
            max = mid - 1;
        else
            return true;
    }
    return false;
}

static int portable_wcwidth(uint32_t ucs) {
    static const struct interval combining[] = {
        { 0x0300, 0x036F }, { 0x0483, 0x0486 }, { 0x0488, 0x0489 },
        { 0x0591, 0x05BD }, { 0x05BF, 0x05BF }, { 0x05C1, 0x05C2 },
        { 0x05C4, 0x05C5 }, { 0x05C7, 0x05C7 }, { 0x0610, 0x0615 },
        { 0x064B, 0x065E }, { 0x0670, 0x0670 }, { 0x06D6, 0x06DC },
        { 0x06DF, 0x06E4 }, { 0x06E7, 0x06E8 }, { 0x06EA, 0x06ED },
        { 0x0711, 0x0711 }, { 0x0730, 0x074A }, { 0x07A6, 0x07B0 },
        { 0x07EB, 0x07F3 }, { 0x20D0, 0x20FF }, { 0xFE00, 0xFE0F },
        { 0xFE20, 0xFE23 }
    };
    static const struct interval wide[] = {
        { 0x1100, 0x115F }, { 0x2329, 0x232A }, { 0x2E80, 0x303E },
        { 0x3040, 0xA4CF }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF },
        { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE6F }, { 0xFF00, 0xFF60 },
        { 0xFFE0, 0xFFE6 }, { 0x1F300, 0x1F64F }, { 0x1F680, 0x1F6FF },
        { 0x20000, 0x2FFFD }, { 0x30000, 0x3FFFD }
    };

    if (ucs == 0) return 0;
    if (ucs < 32 || (ucs >= 0x7f && ucs < 0xa0)) return 0;
    if (bisearch(ucs, combining, sizeof(combining)/sizeof(combining[0]) - 1))
        return 0;
    if (bisearch(ucs, wide, sizeof(wide)/sizeof(wide[0]) - 1))
        return 2;
    return 1;
}
#define wcwidth(cp) portable_wcwidth(cp)

TerminalIO::TerminalIO() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);

    if (GetConsoleMode(hIn, &g_orig_in_mode) && GetConsoleMode(hOut, &g_orig_out_mode)) {
        g_orig_in_cp = GetConsoleCP();
        g_orig_out_cp = GetConsoleOutputCP();
        g_win_modes_saved = true;

        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);

        DWORD out_mode = g_orig_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, out_mode);

        DWORD in_mode = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
        SetConsoleMode(hIn, in_mode);
    }
    _setmode(_fileno(stdout), _O_BINARY);
    raw_mode_active_ = true;
    std::cout << "\x1b[?1049h" << "\x1b[?25l" << std::flush;
}

TerminalIO::~TerminalIO() { restore(); }

void TerminalIO::restore() {
    if (raw_mode_active_) {
        std::cout << "\x1b[?25h" << "\x1b[?1049l" << std::flush;
        std::fflush(stdout);
        _setmode(_fileno(stdout), _O_TEXT);
        if (g_win_modes_saved) {
            SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), g_orig_in_mode);
            SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_orig_out_mode);
            SetConsoleCP(g_orig_in_cp);
            SetConsoleOutputCP(g_orig_out_cp);
        }
        raw_mode_active_ = false;
    }
}

void TerminalIO::reassert_raw_mode() {
    if (!raw_mode_active_ || !g_win_modes_saved) return;
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD in_mode = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
    SetConsoleMode(hIn, in_mode);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleMode(hOut, g_orig_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

int TerminalIO::poll_key() {
    reassert_raw_mode();
    if (!_kbhit()) return 0;
    int c = _getch();
    if (c == 0 || c == 224) {
        if (!_kbhit()) return 0;
        int ext = _getch();
        switch (ext) {
            case 72: return 'A'; // up
            case 80: return 'B'; // down
            case 77: return 'C'; // right
            case 75: return 'D'; // left
            default: return 0;
        }
    }
    if (c == 27) {
        if (_kbhit()) {
            int seq1 = _getch();
            if (seq1 == '[') {
                if (_kbhit()) {
                    int seq2 = _getch();
                    switch (seq2) {
                        case 'A': return 'A';
                        case 'B': return 'B';
                        case 'C': return 'C';
                        case 'D': return 'D';
                    }
                }
            }
        }
        return 27;
    }
    return c;
}

int TerminalIO::rows() const {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        int r = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        if (r > 0) return r;
    }
    return 40;
}

int TerminalIO::cols() const {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        int c = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        if (c > 0) return c;
    }
    return 155;
}

#else

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
    std::cout << "\x1b[?1049h" << "\x1b[?25l" << std::flush; // enter alt-screen, hide cursor
}

TerminalIO::~TerminalIO() { restore(); }

void TerminalIO::restore() {
    if (raw_mode_active_) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        std::cout << "\x1b[?25h" << "\x1b[?1049l" << std::flush; // show cursor, leave alt-screen
        raw_mode_active_ = false;
    }
}

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

#endif

static int codepoint_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (is_indic_codepoint(cp)) return indic_codepoint_width(cp);
    int w = wcwidth(static_cast<wchar_t>(cp));
    return w < 0 ? 0 : w;
}
// w
// int display_width(const std::string& s) {
//     std::string clean = sanitize_lyric_text(s);
// 
//     int cols = 0;
//     size_t i = 0;
//     uint32_t prev_cp = 0;
// 
//     while (i < clean.size()) {
//         uint32_t cp = utf8_decode(clean, i);
//         int w = codepoint_width(cp);

int display_width(const std::string& s) {
    int cols = 0;
    size_t i = 0;
    uint32_t prev_cp = 0;
    while (i < s.size()) {
        uint32_t cp = utf8_decode(s, i);
        int w = codepoint_width(cp);
        // Virama conjunct subtraction: if previous char was a Virama and current is a consonant (width 1),
        // they form a conjunct ligature that fits in the same cell. Subtract 1 to compensate.
        if (is_indic_codepoint(cp)) {
            if (prev_cp == 0x094D || prev_cp == 0x09CD || prev_cp == 0x0A4D || 
                prev_cp == 0x0ACD || prev_cp == 0x0B4D || prev_cp == 0x0BCD || 
                prev_cp == 0x0C4D || prev_cp == 0x0CCD || prev_cp == 0x0D4D) {
                if (w == 1) {
                    cols -= 1; // Merge with previous cell
                }
            }
        }
        
        cols += w;
        prev_cp = cp;
    }
    return cols;
}

std::string utf8_take(const std::string& s, int width) {
    std::string out;
    size_t i = 0;
    int used = 0;
    uint32_t prev_cp = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = utf8_decode(s, i);
        int w = codepoint_width(cp);
        
        if (is_indic_codepoint(cp)) {
            if (prev_cp == 0x094D || prev_cp == 0x09CD || prev_cp == 0x0A4D || 
                prev_cp == 0x0ACD || prev_cp == 0x0B4D || prev_cp == 0x0BCD || 
                prev_cp == 0x0C4D || prev_cp == 0x0CCD || prev_cp == 0x0D4D) {
                if (w == 1) {
                    used -= 1; // Merge with previous cell
                }
            }
        }
        
        if (used + w > width) {
            if (w == 0) {
                out += s.substr(start, i - start);
                used += w;
                prev_cp = cp;
                continue;
            }
            break;
        }
        out += s.substr(start, i - start);
        used += w;
        prev_cp = cp;
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
