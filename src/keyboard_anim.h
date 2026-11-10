// The keyboard of the "<" menus overlay (organigram.h): a keyboard WITHOUT a number pad, German (QWERTZ) or English (QWERTY), that shows
// how to type the key of the selected menu. The keys of the combination light up in turn like a hand pressing them: the modifiers
// (SHIFT / CTRL / ALT, AltGr where the layout needs it) first, then the key itself, then all let go.
// Header-only and free of app state, like organigram.h (the music player, the audio book and the radio share it).
//
//   parse_combo()  : a key as written in config.txt ("G", "CTRL+SHIFT+U", "ALT+L", "<", "*", "SPACE") -> what has to be pressed on
//                    THIS layout (a "*" is SHIFT and + on a German keyboard, SHIFT+8 on an English one)
//   plan()         : the keyboard as rows of cells, each with its text, width and state (0 = up, 1 = modifier down, 2 = the key)
#pragma once

#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace muisc {
namespace kbd {

constexpr int kWidth = 60;    // columns of the keyboard
constexpr int kRows = 5;      // rows of the keyboard

struct Key {
    const char* label;   // what the cap shows (letters upper case)
    const char* base;    // the character it types, "" = none
    const char* shift;   // with SHIFT
    const char* altgr;   // with AltGr ("" = none)
    int w;               // width in columns, with the gap after it
    char mod;            // 's' shift, 'c' ctrl, 'a' alt, 'g' AltGr, 'w' super, 0 = a normal key
};

// ---- German (QWERTZ) --------------------------------------------------------------------------------------------------------------
inline const Key kDeRow1[] = {
    {"^", "^", "\xc2\xb0", "", 4, 0}, {"1", "1", "!", "", 4, 0}, {"2", "2", "\"", "\xc2\xb2", 4, 0}, {"3", "3", "\xc2\xa7", "\xc2\xb3", 4, 0},
    {"4", "4", "$", "", 4, 0}, {"5", "5", "%", "", 4, 0}, {"6", "6", "&", "", 4, 0}, {"7", "7", "/", "{", 4, 0}, {"8", "8", "(", "[", 4, 0},
    {"9", "9", ")", "]", 4, 0}, {"0", "0", "=", "}", 4, 0}, {"\xc3\x9f", "\xc3\x9f", "?", "\\", 4, 0}, {"\xc2\xb4", "\xc2\xb4", "`", "", 4, 0},
    {"Back", "\b", "", "", 8, 0}};
inline const Key kDeRow2[] = {
    {"Tab", "\t", "", "", 6, 0}, {"Q", "q", "Q", "@", 4, 0}, {"W", "w", "W", "", 4, 0}, {"E", "e", "E", "\xe2\x82\xac", 4, 0}, {"R", "r", "R", "", 4, 0},
    {"T", "t", "T", "", 4, 0}, {"Z", "z", "Z", "", 4, 0}, {"U", "u", "U", "", 4, 0}, {"I", "i", "I", "", 4, 0}, {"O", "o", "O", "", 4, 0},
    {"P", "p", "P", "", 4, 0}, {"\xc3\xbc", "\xc3\xbc", "\xc3\x9c", "", 4, 0}, {"+", "+", "*", "~", 4, 0}, {"Ent", "\n", "", "", 6, 0}};
inline const Key kDeRow3[] = {
    {"Caps", "", "", "", 7, 0}, {"A", "a", "A", "", 4, 0}, {"S", "s", "S", "", 4, 0}, {"D", "d", "D", "", 4, 0}, {"F", "f", "F", "", 4, 0},
    {"G", "g", "G", "", 4, 0}, {"H", "h", "H", "", 4, 0}, {"J", "j", "J", "", 4, 0}, {"K", "k", "K", "", 4, 0}, {"L", "l", "L", "", 4, 0},
    {"\xc3\xb6", "\xc3\xb6", "\xc3\x96", "", 4, 0}, {"\xc3\xa4", "\xc3\xa4", "\xc3\x84", "", 4, 0}, {"#", "#", "'", "", 4, 0}};
inline const Key kDeRow4[] = {
    {"Shift", "", "", "", 8, 's'}, {"<", "<", ">", "|", 4, 0}, {"Y", "y", "Y", "", 4, 0}, {"X", "x", "X", "", 4, 0}, {"C", "c", "C", "", 4, 0},
    {"V", "v", "V", "", 4, 0}, {"B", "b", "B", "", 4, 0}, {"N", "n", "N", "", 4, 0}, {"M", "m", "M", "\xc2\xb5", 4, 0}, {",", ",", ";", "", 4, 0},
    {".", ".", ":", "", 4, 0}, {"-", "-", "_", "", 4, 0}, {"Shift", "", "", "", 8, 's'}};
inline const Key kDeRow5[] = {
    {"Ctrl", "", "", "", 7, 'c'}, {"Win", "", "", "", 6, 'w'}, {"Alt", "", "", "", 6, 'a'}, {"Space", " ", "", "", 13, 0},
    {"AltGr", "", "", "", 8, 'g'}, {"Win", "", "", "", 6, 'w'}, {"Menu", "", "", "", 7, 0}, {"Ctrl", "", "", "", 7, 'c'}};

// ---- English (QWERTY, US) ------------------------------------------------------------------------------------------------------------
inline const Key kUsRow1[] = {
    {"`", "`", "~", "", 4, 0}, {"1", "1", "!", "", 4, 0}, {"2", "2", "@", "", 4, 0}, {"3", "3", "#", "", 4, 0}, {"4", "4", "$", "", 4, 0},
    {"5", "5", "%", "", 4, 0}, {"6", "6", "^", "", 4, 0}, {"7", "7", "&", "", 4, 0}, {"8", "8", "*", "", 4, 0}, {"9", "9", "(", "", 4, 0},
    {"0", "0", ")", "", 4, 0}, {"-", "-", "_", "", 4, 0}, {"=", "=", "+", "", 4, 0}, {"Back", "\b", "", "", 8, 0}};
inline const Key kUsRow2[] = {
    {"Tab", "\t", "", "", 6, 0}, {"Q", "q", "Q", "", 4, 0}, {"W", "w", "W", "", 4, 0}, {"E", "e", "E", "", 4, 0}, {"R", "r", "R", "", 4, 0},
    {"T", "t", "T", "", 4, 0}, {"Y", "y", "Y", "", 4, 0}, {"U", "u", "U", "", 4, 0}, {"I", "i", "I", "", 4, 0}, {"O", "o", "O", "", 4, 0},
    {"P", "p", "P", "", 4, 0}, {"[", "[", "{", "", 4, 0}, {"]", "]", "}", "", 4, 0}, {"\\", "\\", "|", "", 6, 0}};
inline const Key kUsRow3[] = {
    {"Caps", "", "", "", 7, 0}, {"A", "a", "A", "", 4, 0}, {"S", "s", "S", "", 4, 0}, {"D", "d", "D", "", 4, 0}, {"F", "f", "F", "", 4, 0},
    {"G", "g", "G", "", 4, 0}, {"H", "h", "H", "", 4, 0}, {"J", "j", "J", "", 4, 0}, {"K", "k", "K", "", 4, 0}, {"L", "l", "L", "", 4, 0},
    {";", ";", ":", "", 4, 0}, {"'", "'", "\"", "", 4, 0}, {"Enter", "\n", "", "", 9, 0}};
inline const Key kUsRow4[] = {
    {"Shift", "", "", "", 10, 's'}, {"Z", "z", "Z", "", 4, 0}, {"X", "x", "X", "", 4, 0}, {"C", "c", "C", "", 4, 0}, {"V", "v", "V", "", 4, 0},
    {"B", "b", "B", "", 4, 0}, {"N", "n", "N", "", 4, 0}, {"M", "m", "M", "", 4, 0}, {",", ",", "<", "", 4, 0}, {".", ".", ">", "", 4, 0},
    {"/", "/", "?", "", 4, 0}, {"Shift", "", "", "", 10, 's'}};
inline const Key kUsRow5[] = {
    {"Ctrl", "", "", "", 7, 'c'}, {"Win", "", "", "", 6, 'w'}, {"Alt", "", "", "", 6, 'a'}, {"Space", " ", "", "", 15, 0},
    {"Alt", "", "", "", 6, 'a'}, {"Win", "", "", "", 6, 'w'}, {"Menu", "", "", "", 7, 0}, {"Ctrl", "", "", "", 7, 'c'}};

struct RowRef { const Key* k; int n; };
template <size_t N> constexpr RowRef row_of(const Key (&a)[N]) { return RowRef{a, static_cast<int>(N)}; }

inline void rows_of(bool qwertz, RowRef out[kRows]) {
    if (qwertz) { out[0] = row_of(kDeRow1); out[1] = row_of(kDeRow2); out[2] = row_of(kDeRow3); out[3] = row_of(kDeRow4); out[4] = row_of(kDeRow5); }
    else        { out[0] = row_of(kUsRow1); out[1] = row_of(kUsRow2); out[2] = row_of(kUsRow3); out[3] = row_of(kUsRow4); out[4] = row_of(kUsRow5); }
}

// ---- what to press -------------------------------------------------------------------------------------------------------------------
struct Combo {
    bool ok = false;        // false: nothing is known about this key (unbound, or not on this keyboard)
    bool shift = false, ctrl = false, alt = false, altgr = false;
    int row = -1, col = -1; // the key itself on the keyboard (-1 = it is not drawn: an arrow, F1 ...)
};

inline std::string upper(std::string s) { for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; }

// Where the character `ch` (UTF-8) is typed on this layout: the key and whether SHIFT / AltGr are needed.
inline bool locate(bool qwertz, const std::string& ch, int& row, int& col, bool& shift, bool& altgr) {
    RowRef rows[kRows];
    rows_of(qwertz, rows);
    for (int r = 0; r < kRows; ++r)
        for (int c = 0; c < rows[r].n; ++c) {
            const Key& k = rows[r].k[c];
            if (k.mod) continue;
            if (ch == k.base && !ch.empty()) { row = r; col = c; shift = false; altgr = false; return true; }
        }
    for (int r = 0; r < kRows; ++r)
        for (int c = 0; c < rows[r].n; ++c) {
            const Key& k = rows[r].k[c];
            if (k.mod) continue;
            if (k.shift[0] && ch == k.shift) { row = r; col = c; shift = true; altgr = false; return true; }
            if (k.altgr[0] && ch == k.altgr) { row = r; col = c; shift = false; altgr = true; return true; }
        }
    return false;
}

// `text` as written in config.txt. An upper case letter on its own is SHIFT and that letter ("G" = SHIFT+g); with CTRL / ALT in front the
// letter is just the letter ("ALT+L"). Names: ENTER, SPACE, TAB, BACKSPACE. Anything with an '@' in front (a key named for the layout) and
// every key that is not on the keyboard shown (ESC, arrows, F1 ...) light only their modifiers.
inline Combo parse_combo(const std::string& text, bool qwertz) {
    Combo c;
    std::string s = text;
    if (s.empty() || s == "-") return c;
    bool explicit_shift = false;
    for (;;) {
        const std::string u = upper(s.substr(0, 6));
        if (u.rfind("CTRL+", 0) == 0 && s.size() > 5) { c.ctrl = true; s.erase(0, 5); }
        else if (u.rfind("ALT+", 0) == 0 && s.size() > 4) { c.alt = true; s.erase(0, 4); }
        else if (u.rfind("OPTION+", 0) == 0 && s.size() > 7) { c.alt = true; s.erase(0, 7); }
        else if (u.rfind("SHIFT+", 0) == 0 && s.size() > 6) { c.shift = true; explicit_shift = true; s.erase(0, 6); }
        else break;
    }
    const std::string up = upper(s);
    std::string ch;
    if (up == "ENTER" || up == "RETURN") ch = "\n";
    else if (up == "SPACE") ch = " ";
    else if (up == "TAB") ch = "\t";
    else if (up == "BACKSPACE") ch = "\b";
    else if (!s.empty() && s[0] != '@') {
        // one character (UTF-8: 1 .. 4 bytes)
        const unsigned char b = static_cast<unsigned char>(s[0]);
        const size_t len = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
        if (s.size() == len) ch = s;
    }
    if (ch.empty()) { c.ok = c.shift || c.ctrl || c.alt; return c; }   // a key that is not drawn
    if (ch.size() == 1 && ch[0] >= 'A' && ch[0] <= 'Z') {
        if (!c.ctrl && !c.alt && !explicit_shift) c.shift = true;   // "G" = SHIFT+g
        ch[0] = static_cast<char>(ch[0] + 32);
    }
    int r = -1, k = -1;
    bool sh = false, ag = false;
    if (!locate(qwertz, ch, r, k, sh, ag)) { c.ok = c.shift || c.ctrl || c.alt; return c; }
    c.row = r; c.col = k;
    if (sh) c.shift = true;
    if (ag) c.altgr = true;
    c.ok = true;
    return c;
}

// ---- the picture ---------------------------------------------------------------------------------------------------------------------
struct Cell { int x = 0, w = 0; std::string text; int state = 0; };   // text is `w` columns wide

constexpr double kCycle = 2.2;   // seconds for one press

// The state of the animation at `time`: 0 = at rest, 1 = the modifiers are down, 2 = the key is down as well.
inline int phase_at(const Combo& c, double time) {
    const bool mods = c.shift || c.ctrl || c.alt || c.altgr;
    const double t = std::fmod(std::max(0.0, time), kCycle);
    if (t < 0.35) return 0;
    if (t < 0.95) return mods ? 1 : 2;
    if (t < 1.75) return 2;
    return 0;
}

inline std::string centred(const std::string& s, int w) {
    int cols = 0;
    for (unsigned char ch : s) if ((ch & 0xC0) != 0x80) ++cols;
    if (cols > w) return s.substr(0, static_cast<size_t>(w));
    const int left = (w - cols) / 2;
    return std::string(static_cast<size_t>(left), ' ') + s + std::string(static_cast<size_t>(w - cols - left), ' ');
}

// The keyboard as rows of cells for the combination `c` at `time`. Which of the two SHIFT / CTRL keys goes down: the one on the other
// side of the keyboard than the key itself (the way two hands do it); a lone modifier of a key that is not drawn: the left one.
inline std::vector<std::vector<Cell>> plan(const Combo& c, bool qwertz, double time) {
    RowRef rows[kRows];
    rows_of(qwertz, rows);
    const int ph = c.ok ? phase_at(c, time) : 0;
    int key_x = -1;
    if (c.row >= 0) { int x = 0; for (int i = 0; i < c.col; ++i) x += rows[c.row].k[i].w; key_x = x + rows[c.row].k[c.col].w / 2; }
    const bool right_hand_key = key_x >= kWidth / 2;
    std::vector<std::vector<Cell>> out(kRows);
    for (int r = 0; r < kRows; ++r) {
        int x = 0;
        // the German Enter is one key over two rows (the upper part is the cell in row 2, the lower one a block at the end of row 3)
        for (int i = 0; i < rows[r].n; ++i) {
            const Key& k = rows[r].k[i];
            Cell cell;
            cell.x = x; cell.w = k.w - 1;
            cell.text = centred(k.label, cell.w - 2);   // (the brackets, or the blanks of a key that is down, go round it: see state below)
            if (c.ok && ph > 0) {
                bool down = false;
                if (k.mod == 's' && c.shift) { const bool left_key = i == 0; down = ph >= 1 && (c.row < 0 ? left_key : right_hand_key == left_key); }
                else if (k.mod == 'c' && c.ctrl) { const bool left_key = i == 0; down = ph >= 1 && (c.row < 0 ? left_key : right_hand_key == left_key); }
                else if (k.mod == 'a' && c.alt) down = ph >= 1 && (qwertz ? i == 2 : (c.row < 0 || right_hand_key ? i == 2 : i == 4));
                else if (k.mod == 'g' && c.altgr) down = ph >= 1;
                if (down) cell.state = 1;
                if (ph >= 2 && c.row == r && c.col == i) cell.state = 2;
            }
            cell.text = (cell.state == 0 ? "[" : " ") + cell.text + (cell.state == 0 ? "]" : " ");
            out[static_cast<size_t>(r)].push_back(cell);
            x += k.w;
        }
        if (qwertz && r == 2) {   // the lower part of the German Enter
            Cell e; e.x = x; e.w = kWidth - x - 1;
            if (c.ok && ph >= 2 && c.row == 1 && c.col == rows[1].n - 1) e.state = 2;
            e.text = (e.state == 0 ? "[" : " ") + std::string(static_cast<size_t>(std::max(0, e.w - 2)), ' ') + (e.state == 0 ? "]" : " ");
            out[static_cast<size_t>(r)].push_back(e);
        }
    }
    return out;
}

}  // namespace kbd
}  // namespace muisc
