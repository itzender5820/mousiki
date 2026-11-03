#pragma once
// Shared single-line text editing (caret, Shift+arrow marking, Home/End, Delete, Ctrl+C/X/V, painting a field) and the
// typo-tolerant search score. Used by the music player (src/app.cpp) AND the radio (src_radio/): one copy for both.
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include "terminal_ui.h"
#include "utf8_util.h"

namespace muisc {

// Every text-entry field below (search box, bulk-add link field, color
// hex field, the retry-lyrics title/artist/type fields) only accepted
// key values 32-126 -- plain printable ASCII. This is not just a
// Windows gap: a typed umlaut, accented letter, or any other non-ASCII
// character arrives as a multi-byte UTF-8 sequence whose individual byte
// values are all >= 0x80 (continuation bytes 0x80-0xBF, lead bytes
// 0xC2-0xF4), every one of which failed this check and simply
// never reach the buffer -- on POSIX with a real UTF-8 terminal just as
// much as on Windows. win_poll_key() (Windows) and the POSIX raw-input
// path both hand such a keystroke over one UTF-8 byte per call already;
// this is what actually lets any of those bytes through.
inline bool is_text_key(int key) {
    return (key >= 32 && key < 127) || (key >= 0x80 && key <= 0xFF);
}

// ---------------------------------------------------------------------------
// Single-line text editing: caret, selection, and how a field is painted
//
// A text field in this app is a plain std::string plus two BYTE offsets (see
// edit_caret_/edit_anchor_ in app.h): the caret, and the other end of the
// selection -- equal to the caret when nothing is marked, which makes
// "caret != anchor" the selection test itself. Offsets are in bytes because
// that is what the buffers hold, but every movement and every deletion is
// quantised to the codepoint boundaries below, so a multi-byte character is
// never split in half.
//
// Two editors share this code and are never active at the same time:
// Mode::ColorEdit (the Settings' in-place value editing) and the meta
// editor's field editor. Both own their offsets and pass them in.
// ---------------------------------------------------------------------------

// Every byte offset at which `s` may be split without breaking UTF-8: 0,
// then the start of each codepoint, then s.size() itself.
inline std::vector<size_t> utf8_cuts(const std::string& s) {
    std::vector<size_t> cuts;
    cuts.reserve(s.size() + 1);
    cuts.push_back(0);
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 1;
        if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        i += n;
        if (i > s.size()) i = s.size(); // truncated lead byte: treat the tail as one char
        // Combining marks (a voiced-sound mark after Japanese は = ば, accents, variation selectors, ZWJ ...) have no width
        // of their own: they stay with the character before them, so the caret, the block cursor and DEL treat the pair
        // as one character instead of splitting it.
        while (i < s.size()) {
            const unsigned char d = static_cast<unsigned char>(s[i]);
            size_t m = 1;
            if ((d & 0xE0) == 0xC0) m = 2;
            else if ((d & 0xF0) == 0xE0) m = 3;
            else if ((d & 0xF8) == 0xF0) m = 4;
            if (d < 0x80 || i + m > s.size() || display_width(s.substr(i, m)) != 0) break;
            i += m;
        }
        cuts.push_back(i);
    }
    return cuts;
}

inline size_t utf8_prev_cut(const std::string& s, size_t off) {
    if (off == 0) return 0;
    size_t best = 0;
    for (size_t c : utf8_cuts(s)) {
        if (c >= off) break;
        best = c;
    }
    return best;
}

inline size_t utf8_next_cut(const std::string& s, size_t off) {
    if (off >= s.size()) return s.size();
    for (size_t c : utf8_cuts(s)) if (c > off) return c;
    return s.size();
}

inline void le_clamp(const std::string& s, size_t& caret, size_t& anchor) {
    if (caret > s.size()) caret = s.size();
    if (anchor > s.size()) anchor = s.size();
}

inline void le_range(size_t caret, size_t anchor, size_t& a, size_t& b) {
    a = std::min(caret, anchor);
    b = std::max(caret, anchor);
}

inline bool le_has_sel(size_t caret, size_t anchor) { return caret != anchor; }

inline void le_erase_selection(std::string& s, size_t& caret, size_t& anchor) {
    size_t a = 0, b = 0;
    le_range(caret, anchor, a, b);
    if (a == b) return;
    s.erase(a, b - a);
    caret = anchor = a;
}

// dir: -1 left, +1 right. shift keeps anchor where it was (marking), which
// is exactly the difference between Shift+Left and Left.
inline void le_move(std::string& s, size_t& caret, size_t& anchor, int dir, bool shift) {
    le_clamp(s, caret, anchor);
    caret = (dir < 0) ? utf8_prev_cut(s, caret) : utf8_next_cut(s, caret);
    if (!shift) anchor = caret;
}

inline void le_backspace(std::string& s, size_t& caret, size_t& anchor) {
    le_clamp(s, caret, anchor);
    if (le_has_sel(caret, anchor)) { le_erase_selection(s, caret, anchor); return; }
    size_t p = utf8_prev_cut(s, caret);
    s.erase(p, caret - p);
    caret = anchor = p;
}

inline void le_delete_forward(std::string& s, size_t& caret, size_t& anchor) {
    le_clamp(s, caret, anchor);
    if (le_has_sel(caret, anchor)) { le_erase_selection(s, caret, anchor); return; }
    size_t n = utf8_next_cut(s, caret);
    s.erase(caret, n - caret);
    anchor = caret;
}

// `limit` is the field's maximum byte length -- the same cap the old
// "append at the end" code enforced with its inline size() checks.
inline void le_insert(std::string& s, size_t& caret, size_t& anchor, char c, size_t limit) {
    le_clamp(s, caret, anchor);
    le_erase_selection(s, caret, anchor);
    if (s.size() >= limit) return;
    s.insert(caret, 1, c);
    caret = anchor = caret + 1;
}

// Paste. Clipboard text can be multi-line and arbitrary length: control
// characters are dropped (a newline inside a single-line row would corrupt
// the layout) and whatever still doesn't fit is trimmed, codepoint-aligned,
// against the same limit.
inline void le_paste(std::string& s, size_t& caret, size_t& anchor, const std::string& text, size_t limit) {
    le_clamp(s, caret, anchor);
    le_erase_selection(s, caret, anchor);
    std::string t;
    t.reserve(text.size());
    for (unsigned char c : text) {
        if (c >= 32 && c != 127) t.push_back(static_cast<char>(c));
    }
    if (t.empty()) return;
    const size_t room = (s.size() >= limit) ? 0 : limit - s.size();
    while (t.size() > room && !t.empty()) t.erase(utf8_prev_cut(t, t.size()), std::string::npos);
    if (t.empty()) return;
    s.insert(caret, t);
    caret = anchor = caret + t.size();
}

// Word movement (Ctrl+Left / Right): a word is a run of non-blank characters. Left goes to the start of the word before the caret,
// right to the start of the next one (or the end of the text). Both stop only at blanks or the ends, so never inside a character.
inline size_t le_word_left(const std::string& s, size_t caret) {
    size_t i = std::min(caret, s.size());
    while (i > 0 && s[i - 1] == ' ') --i;
    while (i > 0 && s[i - 1] != ' ') --i;
    return i;
}
inline size_t le_word_right(const std::string& s, size_t caret) {
    size_t i = std::min(caret, s.size());
    while (i < s.size() && s[i] != ' ') ++i;
    while (i < s.size() && s[i] == ' ') ++i;
    return i;
}

// ---- undo for text fields ---------------------------------------------------------------------------------------------
// CTRL+z (and CTRL+y, the key that sits there on a QWERTZ keyboard) takes back the last change of the field that has the keyboard:
// typed characters of one run count as one step, a deletion, a paste or a cut each as its own. The history belongs to the buffer
// (kept by its address); when the text was changed from outside between two keys (another value loaded) it starts over.
struct LeUndoStep { std::string text; size_t caret = 0; };
struct LeUndoHist { std::vector<LeUndoStep> steps; std::string last; int kind = 0; };
inline std::unordered_map<const void*, LeUndoHist>& le_undo_map() {
    static std::unordered_map<const void*, LeUndoHist> m;
    return m;
}
inline bool le_is_undo_key(int key) { return key == kKeyCtrlZ || key == 25; }   // 25 = the control byte of CTRL+y
// For the editors that handle their keys by hand: records the change a key made when the scope ends (whatever way the handler returns).
struct LeUndoScope {
    const void* id; const std::string& buf; std::string before; size_t caret; int key;
    LeUndoScope(const void* i, const std::string& b, size_t c, int k) : id(i), buf(b), before(b), caret(c), key(k) {}
    ~LeUndoScope();
};
// An undo key: takes the last step back. Returns true when `key` was an undo key (consumed, changed or not); *changed tells if the text changed.
inline bool le_undo_key(const void* id, std::string& buf, size_t& caret, size_t& anchor, int key, std::string* status, bool* changed = nullptr) {
    if (changed) *changed = false;
    if (!le_is_undo_key(key)) return false;
    LeUndoHist& h = le_undo_map()[id];
    if (h.last != buf) { h.steps.clear(); h.kind = 0; }
    if (h.steps.empty()) { if (status) *status = "nothing to undo"; return true; }
    LeUndoStep st = std::move(h.steps.back());
    h.steps.pop_back();
    buf = st.text;
    caret = anchor = std::min(st.caret, buf.size());
    h.last = buf;
    h.kind = 0;
    if (status) *status = "UNDONE";
    if (changed) *changed = true;
    return true;
}
// After a key was handled: records what it changed. `before` / `caret_before`: the field before the key. Characters typed one after the
// other make one step; any other change (or a caret move in between) starts a new one.
inline void le_undo_note(const void* id, const std::string& buf, const std::string& before, size_t caret_before, int key) {
    LeUndoHist& h = le_undo_map()[id];
    if (h.last != before) { h.steps.clear(); h.kind = 0; }
    if (buf != before) {
        const int kind = (is_text_key(key) && !last_key_was_arrow()) ? 1 : 2;
        if (!(kind == 1 && h.kind == 1)) {
            h.steps.push_back({before, caret_before});
            if (h.steps.size() > 100) h.steps.erase(h.steps.begin());
        }
        h.kind = kind;
    } else {
        h.kind = 0;   // a caret move or a copy: the next typing is a new step
    }
    h.last = buf;
}

inline LeUndoScope::~LeUndoScope() { le_undo_note(id, buf, before, caret, key); }

// One keystroke, every single-line text field in the app.
//
// Caret movement, Shift+arrows marking, Home/End, Ctrl+C/X/V and typing --
// the same key set the Settings' ColorEdit and the meta editor's field editor
// grew first, factored out here so the four search/filter boxes (the main
// UI's "/", the meta editor's Search line, the playlist editor's name and
// library boxes and the playlist list's own search) can offer exactly the
// same thing instead of a fifth hand-rolled copy.
//
// Returns true only when the TEXT really changed, which is what tells the
// caller to re-run whatever filter the box drives; pure caret/clipboard keys
// return false. `status` (nullable) receives the short feedback these keys
// produce ("COPIED"/"CUT"/"PASTED"), because a box with no status line of
// its own should not go silent on a copy.
inline bool edit_text_key_core(std::string& buf, size_t& caret, size_t& anchor, int key,
                               size_t limit, std::string* status) {
    auto say = [&](const char* s) { if (status) *status = s; };
    // Modifier combinations arrive as their own sentinel values
    // (terminal_ui.h) precisely because the bare ones are already taken: an
    // arrow press collapses to the letter it would otherwise type, and a
    // typed 'c' has to stay a typed 'c' inside a word.
    if (key == kKeyHome) { caret = anchor = 0; return false; }
    if (key == kKeyEnd) { caret = anchor = buf.size(); return false; }
    if (key == kKeyShiftLeft) { le_move(buf, caret, anchor, -1, true); return false; }
    if (key == kKeyShiftRight) { le_move(buf, caret, anchor, +1, true); return false; }
    if (key == kKeyCtrlLeft || key == kKeyCtrlShiftLeft) { le_clamp(buf, caret, anchor); caret = le_word_left(buf, caret); if (key == kKeyCtrlLeft) anchor = caret; return false; }
    if (key == kKeyCtrlRight || key == kKeyCtrlShiftRight) { le_clamp(buf, caret, anchor); caret = le_word_right(buf, caret); if (key == kKeyCtrlRight) anchor = caret; return false; }
    if (key == kKeyDelete) { le_delete_forward(buf, caret, anchor); return true; }
    if (key == kKeyCtrlC) {
        size_t a = 0, b = 0;
        le_range(caret, anchor, a, b);
        std::string t = (a == b) ? buf : buf.substr(a, b - a);
        clipboard_set(t);
        say(t.empty() ? "nothing selected" : "COPIED");
        return false;
    }
    if (key == kKeyCtrlX) {
        size_t a = 0, b = 0;
        le_range(caret, anchor, a, b);
        if (a == b) return false; // no selection: cut must never empty the field
        clipboard_set(buf.substr(a, b - a));
        le_erase_selection(buf, caret, anchor);
        say("CUT");
        return true;
    }
    if (key == kKeyCtrlV) { le_paste(buf, caret, anchor, clipboard_get(), limit); say("PASTED"); return true; }
    if (key == 127 || key == 8) { le_backspace(buf, caret, anchor); return true; }
    // A genuinely typed A-D goes into the buffer ("C:\" has no way past this
    // check otherwise), while an arrow press moves the caret Left/Right -- and
    // Up/Down is dropped here: every field this is used on is a single line,
    // and the boxes that DO want Up/Down for the list below them claim those
    // keys before calling in.
    if (last_key_was_arrow() && (key == 'A' || key == 'B')) return false;
    if (last_key_was_arrow() && (key == 'C' || key == 'D')) {
        le_move(buf, caret, anchor, key == 'D' ? -1 : +1, false);
        return false;
    }
    if (is_text_key(key)) { le_insert(buf, caret, anchor, static_cast<char>(key), limit); return true; }
    return false;
}

inline bool edit_text_key(std::string& buf, size_t& caret, size_t& anchor, int key,
                          size_t limit, std::string* status) {
    bool changed = false;
    if (le_undo_key(&buf, buf, caret, anchor, key, status, &changed)) return changed;   // CTRL+z / CTRL+y
    const std::string before = buf;
    const size_t caret_before = caret;
    const bool r = edit_text_key_core(buf, caret, anchor, key, limit, status);
    le_undo_note(&buf, buf, before, caret_before, key);
    return r;
}

// (App::edit_focus() lives below, outside this anonymous namespace.)

// What one field looks like on screen after all of that.
struct EditPaint {
    std::string s;  // the windowed text, selection wrapped in reverse video
    int cols = 0;   // display columns s really occupies (ANSI escapes not counted)
    int caret = 0;  // 0-based column of the caret inside that window
};

// Paints a field into `w` columns: a window scrolled so the caret stays
// visible (the buffer can be far longer than the field), the selected range
// in reverse video, and -- with `block` -- the caret itself drawn as a █,
// which is how the meta editor shows a cursor. The Settings tabs instead move
// the REAL terminal cursor to `caret` (they already do that today) and pass
// block = false.
//
// `restore` is re-emitted after the selection so the field keeps its own
// colour; it must be exactly the SGR the caller has already put in effect,
// because display_width() cannot see through ANSI escapes -- colouring first
// and measuring afterwards would miscount every row.
inline EditPaint paint_edit_field(const std::string& s, size_t caret, size_t anchor, int w,
                                  const std::string& restore, bool block) {
    EditPaint out;
    if (w < 1) w = 1;
    le_clamp(s, caret, anchor);
    if (anchor > caret) std::swap(anchor, caret);

    const std::vector<size_t> cuts = utf8_cuts(s);
    std::vector<int> col(cuts.size(), 0); // col[i] = display column of cuts[i]
    for (size_t i = 1; i < cuts.size(); ++i) {
        col[i] = col[i - 1] + display_width(s.substr(cuts[i - 1], cuts[i] - cuts[i - 1]));
    }
    auto idx_of = [&](size_t off) -> size_t {
        for (size_t i = 0; i < cuts.size(); ++i) if (cuts[i] == off) return i;
        return cuts.size() - 1;
    };
    const size_t caret_i = idx_of(caret);
    const size_t anchor_i = idx_of(anchor);

    // The leftmost window that still keeps the caret inside it: starting any
    // further left would push the caret past the right edge, starting further
    // right would hide text the caret is sitting on.
    const int caret_col = col[caret_i];
    const int target = caret_col - (w - 1);
    size_t lo_i = 0;
    for (size_t i = 0; i <= caret_i; ++i) {
        if (col[i] >= target) { lo_i = i; break; }
        lo_i = caret_i;
    }
    size_t hi_i = lo_i;
    while (hi_i + 1 < cuts.size() && col[hi_i + 1] - col[lo_i] <= w) ++hi_i;
    if (hi_i < caret_i) hi_i = caret_i;

    out.caret = caret_col - col[lo_i];
    if (out.caret < 0) out.caret = 0;
    if (out.caret > w - 1) out.caret = w - 1;

    auto emit = [&](size_t from, size_t to, bool sel, const char* glyph) {
        std::string body = glyph ? std::string(glyph) : s.substr(from, to - from);
        if (body.empty()) return;
        if (sel) out.s += "\x1b[7m" + body + "\x1b[0m" + restore;
        else out.s += body;
    };
    for (size_t i = lo_i; i < hi_i; ++i) {
        const bool sel = (i >= anchor_i && i < caret_i);
        if (block && i == caret_i) {
            // The block covers as many columns as the character it replaces: a double-width one (Japanese, Chinese,
            // Korean, emoji) drawn as a single block made the row one column short and pushed the right border in.
            const int cw = std::max(1, col[i + 1] - col[i]);
            std::string blk;
            for (int k = 0; k < cw; ++k) blk += "\u2588";
            emit(cuts[i], cuts[i + 1], sel, blk.c_str());
        }
        else emit(cuts[i], cuts[i + 1], sel, nullptr);
    }
    // Caret at (or beyond) the end of the window: there is no character to
    // replace, so the block is appended -- which only fits because the window
    // above reserves the column (col[caret_i] - col[lo_i] <= w-1).
    if (block && caret_i >= hi_i) emit(0, 0, false, "\u2588");

    out.cols = col[hi_i] - col[lo_i];
    if (block && caret_i >= hi_i) out.cols += 1;
    return out;
}



// Lowercases AND folds common word-separator punctuation (hyphen,
// underscore, dot, slash) down to plain spaces. Used only for search
// matching (fuzzy_score below), never for display.
//
// Without this, a tag/filename like "X-Files" is one glued-together
// token "x-files" as far as matching is concerned, while a query typed
// as "X Files" is two separate words ["x", "files"]. Tier 1 (exact
// substring) fails because "x-files" never contains the literal text
// "x files". Tier 2 (per-word fuzzy) also fails: comparing whole word
// "x" against whole word "x-files" gives a huge edit distance (adding
// "-files"), well below the 0.55 quality floor -- even though every
// individual word the user typed is actually present. Folding the
// separator to a space first turns "x-files" into "x files" so both
// tiers line up with "X Files" the same way they already would for a
// title that happens to use a plain space.
// The Latin letters with accents / umlauts (U+00C0 .. U+017F) as plain ASCII, so "Ärzte", "arzte" and "Aerzte"-style typing find each
// other ("é" -> "e", "ß" -> "ss", "æ" -> "ae" ...). nullptr: not a letter (the multiplication and division signs).
inline const char* const* latin_fold_table() {
    static const char* const t[192] = {"a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i","d","n","o","o","o","o","o",nullptr,"o","u","u","u","u","y","th","ss","a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i","d","n","o","o","o","o","o",nullptr,"o","u","u","u","u","y","th","y","a","a","a","a","a","a","c","c","c","c","c","c","c","c","d","d","d","d","e","e","e","e","e","e","e","e","e","e","g","g","g","g","g","g","g","g","h","h","h","h","i","i","i","i","i","i","i","i","i","i",nullptr,nullptr,"j","j","k","k","k","l","l","l","l","l","l",nullptr,nullptr,"l","l","n","n","n","n","n","n","n","n","n","o","o","o","o","o","o","oe","oe","r","r","r","r","r","r","s","s","s","s","s","s","s","s","t","t","t","t","t","t","u","u","u","u","u","u","u","u","u","u","u","u","w","w","y","y","y","z","z","z","z","z","z","s"};
    return t;
}

// Lowercases (also the accented Latin letters), folds those to ASCII and folds common word-separator punctuation (hyphen,
// underscore, dot, slash) down to plain spaces. Used only for search matching (fuzzy_score below), never for display.
//
// The separators: without this, a tag/filename like "X-Files" is one glued-together token "x-files" as far as matching is
// concerned, while a query typed as "X Files" is two separate words ["x", "files"].
inline std::string normalize_for_search(std::string s) {
    s = ascii_lower_str(std::move(s));
    std::string o;
    o.reserve(s.size());
    const char* const* tab = latin_fold_table();
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xC3 && i + 1 < s.size()) {   // U+00C0 .. U+00FF
            const unsigned cp = 0xC0u + (static_cast<unsigned char>(s[i + 1]) - 0x80u);
            if (cp >= 0xC0 && cp <= 0xFF && tab[cp - 0xC0]) { o += tab[cp - 0xC0]; ++i; continue; }
        } else if ((c == 0xC4 || c == 0xC5) && i + 1 < s.size()) {   // U+0100 .. U+017F
            const unsigned cp = (c == 0xC4 ? 0x100u : 0x140u) + (static_cast<unsigned char>(s[i + 1]) - 0x80u);
            if (cp >= 0x100 && cp <= 0x17F && tab[cp - 0xC0]) { o += tab[cp - 0xC0]; ++i; continue; }
        }
        o += (c == '-' || c == '_' || c == '.' || c == '/') ? ' ' : static_cast<char>(c);
    }
    return o;
}

inline std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> words;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) words.push_back(cur);
    return words;
}

// Standard edit distance (single-char insert/delete/substitute cost 1).
// Small strings only (track titles/artists/search words) — the O(n*m)
// DP table is negligible at this scale.
inline int levenshtein(const std::string& a, const std::string& b) {   // (an adjacent swap counts as one edit: "teh" -> "the")
    size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = 0; i <= n; ++i) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= m; ++j) dp[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({dp[i - 1][j] + 1, dp[i][j - 1] + 1, dp[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) dp[i][j] = std::min(dp[i][j], dp[i - 2][j - 2] + 1);
        }
    }
    return dp[n][m];
}

// How well a typed word fits a word of the text, 0..1 (below ~0.55 = not at all): the edit distance, but a word typed only
// as far as it goes ("cov" for "cover3") counts as a good fit, and a trailing number of the text's word ("cover3") is ignored.
inline double word_pair_quality(const std::string& qw, const std::string& tw) {
    const size_t max_len = std::max(qw.size(), tw.size());
    if (max_len == 0) return 0.0;
    double q = 1.0 - static_cast<double>(levenshtein(qw, tw)) / static_cast<double>(max_len);
    if (qw.size() >= 2 && tw.size() > qw.size() && tw.compare(0, qw.size(), qw) == 0)
        q = std::max(q, 0.80 + 0.15 * static_cast<double>(qw.size()) / static_cast<double>(tw.size()));
    if (!tw.empty() && tw.back() >= '0' && tw.back() <= '9' && !(qw.back() >= '0' && qw.back() <= '9')) {
        size_t e = tw.size();
        while (e > 1 && tw[e - 1] >= '0' && tw[e - 1] <= '9') --e;
        if (e < tw.size()) q = std::max(q, word_pair_quality(qw, tw.substr(0, e)) - 0.02);
    }
    return q;
}

// A word typed as the run-together starts of several words of the text: "testcover" finds "test song cover3" (test + cover, the
// word in between skipped), "boh rhap" typed as "bohrhap" finds "bohemian rhapsody". Each part has to start at the start of a word and
// run on from there. `q` has no spaces; `t` is the normalised text. Returns the number of skipped words, -1 = no such way.
inline int abbreviation_skips(const std::string& q, const std::string& t) {
    if (q.size() < 4 || q.size() > 40 || t.size() < q.size()) return -1;
    {   // cheap exit first: some word has to start with the query's first character
        bool any = false;
        for (size_t i = 0; i < t.size() && !any; ++i) any = t[i] == q[0] && (i == 0 || t[i - 1] == ' ' || t[i - 1] == '\t');
        if (!any) return -1;
    }
    std::vector<std::pair<size_t, size_t>> w;   // [from, to) of the text's words
    for (size_t i = 0; i < t.size();) {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
        const size_t b = i;
        while (i < t.size() && t[i] != ' ' && t[i] != '\t') ++i;
        if (i > b) w.push_back({b, i});
        if (w.size() > 24) break;
    }
    if (w.size() < 2) return -1;
    // best[qi][wi]: fewest skipped words when the first qi characters are matched and the last one ended a stretch inside word wi
    // (any position in it: the next character either goes on in this word at `at` or starts a later word)
    const int INF = 1 << 20;
    // state: (qi chars matched, word wi, chars consumed of wi = ci). Plain DP over qi.
    std::vector<std::vector<std::vector<int>>> dp(q.size() + 1, std::vector<std::vector<int>>(w.size()));
    for (auto& r : dp) for (size_t i = 0; i < w.size(); ++i) r[i].assign(w[i].second - w[i].first + 1, INF);
    for (size_t wi = 0; wi < w.size(); ++wi)   // the first character starts a word (the words before it are not "skipped": the match starts there)
        if (t[w[wi].first] == q[0]) dp[1][wi][1] = 0;
    for (size_t qi = 1; qi < q.size(); ++qi)
        for (size_t wi = 0; wi < w.size(); ++wi)
            for (size_t ci = 1; ci < dp[qi][wi].size(); ++ci) {
                const int cur = dp[qi][wi][ci];
                if (cur >= INF) continue;
                const size_t len = w[wi].second - w[wi].first;
                if (ci < len && t[w[wi].first + ci] == q[qi]) dp[qi + 1][wi][ci + 1] = std::min(dp[qi + 1][wi][ci + 1], cur);   // on in this word
                for (size_t wj = wi + 1; wj < w.size(); ++wj)                                                                    // a later word starts
                    if (t[w[wj].first] == q[qi]) dp[qi + 1][wj][1] = std::min(dp[qi + 1][wj][1], cur + static_cast<int>(wj - wi - 1));
            }
    int best = INF;
    for (size_t wi = 0; wi < w.size(); ++wi)
        for (size_t ci = 1; ci < dp[q.size()][wi].size(); ++ci) best = std::min(best, dp[q.size()][wi][ci]);
    return best >= INF ? -1 : best;
}

// Typo-tolerant match score: higher is better, negative means "not a
// match at all" (excluded from results). An exact literal substring
// match always outranks every fuzzy match, regardless of how good the
// fuzzy quality is — so "another love" typed exactly always sits above
// a merely-close fuzzy hit. Within the fuzzy tier, each query word must
// find a reasonably close word somewhere in the target (a query word
// that's wildly different from everything in the target means this
// isn't really a match, even if some OTHER query word happens to fit)
// — this is what lets "anogher lobe" (typo'd "another love") still find
// the track: word-level edit distance tolerates the substituted
// characters that a plain substring or subsequence check would miss
// entirely (neither "anogher" nor "lobe" appears anywhere in "another
// love" as literal text).
inline double fuzzy_score(const std::string& query, const std::string& target) {
    std::string q = normalize_for_search(query), t = normalize_for_search(target);
    if (q.empty()) return 0.0;

    size_t pos = t.find(q);
    if (pos != std::string::npos) {
        return 1000.0 - std::min<double>(static_cast<double>(pos), 900.0); // tier 1: exact substring, earlier position ranks higher
    }

    auto q_words = split_words(q);
    auto t_words = split_words(t);
    if (q_words.empty() || t_words.empty()) return -1.0;

    double fuzzy = -1.0;
    {
        double total_quality = 0.0;
        bool ok = true;
        for (auto& qw : q_words) {
            double best_quality = -1.0;
            for (auto& tw : t_words) best_quality = std::max(best_quality, word_pair_quality(qw, tw));
            if (best_quality < 0.55) { ok = false; break; }   // this query word doesn't fit anywhere close enough -- not a real match
            total_quality += best_quality;
        }
        if (ok) fuzzy = (total_quality / static_cast<double>(q_words.size())) * 100.0; // tier 2: fuzzy, always below tier 1's range
    }
    // tier 2b: the query as one run-together word ("testcover" for "test_song-cover3"); the fewer words skipped, the better
    std::string compact;
    for (const auto& w : q_words) compact += w;
    const int skips = abbreviation_skips(compact, t);
    if (skips >= 0) fuzzy = std::max(fuzzy, std::max(60.0, 90.0 - 5.0 * skips));
    return fuzzy;
}


} // namespace muisc
