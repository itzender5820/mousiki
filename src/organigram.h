// The "<" menu index as an organigram: a tree of menus (Main -> categories -> menus -> lower menus) in boxes, drawn in the colours
// of the other big overlays. Header-only and free of any app state, so the music player, the audio book and the radio draw and steer
// it the same way. A row of siblings that is too wide for the window grows downwards as a list (tree branches) instead of scrolling.
//
//   Tree   : nodes in pre-order, each with its depth (0 = Main). A node with an `action` opens that menu (the caller maps the
//            action to a key); a node without one is a group that only has children.
//   steer  : move() - UP goes to the parent, DOWN to the first child, LEFT / RIGHT to the neighbours (round and round).
//   render : the inside of the overlay: one string per row, each exactly W columns wide (the caller draws the frame, title and legend).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace muisc {
namespace org {

struct Node {
    int depth;
    const char* title;      // the full name (info line)
    const char* action;     // nullptr = a group
    const char* fallback;   // the default key (the caller may ignore it)
    const char* label = nullptr;   // shorter text for the box (default: the title)
    char column = 0;               // classic view, categories only: 'L' = always the left column
};
struct Tree { const Node* n; int count; };

template <size_t N>
inline Tree make_tree(const Node (&a)[N]) { return Tree{a, static_cast<int>(N)}; }

inline int parent_of(const Tree& t, int i) {
    for (int k = i - 1; k >= 0; --k) if (t.n[k].depth < t.n[i].depth) return k;
    return -1;
}
inline std::vector<int> children_of(const Tree& t, int i) {
    std::vector<int> v;
    if (i < 0 || i >= t.count) return v;
    for (int k = i + 1; k < t.count && t.n[k].depth > t.n[i].depth; ++k)
        if (t.n[k].depth == t.n[i].depth + 1) v.push_back(k);
    return v;
}
inline int clamp_sel(const Tree& t, int sel) { return std::clamp(sel, 1, std::max(1, t.count - 1)); }

// dir: 0 up (parent), 1 down (first child), 2 left, 3 right. Main itself is never selected.
inline int move(const Tree& t, int sel, int dir) {
    sel = clamp_sel(t, sel);
    if (dir == 0) { const int p = parent_of(t, sel); return p > 0 ? p : sel; }
    if (dir == 1) { const auto c = children_of(t, sel); return c.empty() ? sel : c.front(); }
    const auto sib = children_of(t, parent_of(t, sel));
    if (sib.size() < 2) return sel;
    size_t at = 0;
    for (size_t i = 0; i < sib.size(); ++i) if (sib[i] == sel) at = i;
    return sib[(at + (dir == 3 ? 1 : sib.size() - 1)) % sib.size()];
}

struct Theme {
    std::string line = "\x1b[2m";   // the branches and the boxes that are off the path (the overlays' border colour)
    std::string path = "\x1b[1m";   // the boxes on the path from Main to the selection (the overlays' header colour)
    std::string key = "";           // the keys shown in the boxes (the overlays' legend colour)
    std::string walked;             // the branches on the path (the progress bar's "played" colour; empty = `path`)
    std::string pending;            // the branches and boxes that are not on the path (the progress bar's "remaining" colour; empty = `line`)
    std::function<std::string(const Node&)> key_text;   // the key shown with a node ("" = none)
};

namespace detail {
struct Cell { std::string g = " "; const std::string* sgr = nullptr; };

struct Canvas {
    int W, H;
    std::vector<Cell> c;
    std::deque<std::string> styles;   // (the cells point into it; a deque keeps them in place)
    Canvas(int w, int h) : W(w), H(h), c(static_cast<size_t>(w) * static_cast<size_t>(h)) {}
    const std::string* style(const std::string& s) {
        for (const auto& x : styles) if (x == s) return &x;
        styles.push_back(s);
        return &styles.back();
    }
    Cell* at(int x, int y) { return x < 0 || y < 0 || x >= W || y >= H ? nullptr : &c[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)]; }
    void put(int x, int y, const std::string& g, const std::string& sgr) {
        if (Cell* p = at(x, y)) { p->g = g; p->sgr = style(sgr); }
    }
};

inline std::vector<std::string> glyphs(const std::string& s) {
    std::vector<std::string> v;
    for (size_t i = 0; i < s.size();) {
        const unsigned char ch = static_cast<unsigned char>(s[i]);
        const size_t n = ch < 0x80 ? 1 : ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : 2;
        v.push_back(s.substr(i, n));
        i += n;
    }
    return v;
}
inline int width_of(const std::string& s) { return static_cast<int>(glyphs(s).size()); }
inline std::string clip(const std::string& s, int w) {
    const auto g = glyphs(s);
    if (static_cast<int>(g.size()) <= w) return s;
    std::string o;
    for (int i = 0; i < w - 1 && i < static_cast<int>(g.size()); ++i) o += g[static_cast<size_t>(i)];
    return o + "\xe2\x80\xa6";   // …
}
inline void put_text(Canvas& cv, int x, int y, const std::string& s, const std::string& sgr) {
    for (const auto& g : glyphs(s)) { cv.put(x, y, g, sgr); ++x; }
}

// Box-drawing glyph for a junction from which lines go up / down / left / right.
inline const char* junction(bool u, bool d, bool l, bool r) {
    static const char* t[16] = {" ", "\xe2\x95\xb5", "\xe2\x95\xb7", "\xe2\x94\x82", "\xe2\x95\xb4", "\xe2\x94\x98", "\xe2\x94\x90", "\xe2\x94\xa4",
                                "\xe2\x95\xb6", "\xe2\x94\x94", "\xe2\x94\x8c", "\xe2\x94\x9c", "\xe2\x94\x80", "\xe2\x94\xb4", "\xe2\x94\xac", "\xe2\x94\xbc"};
    return t[(u ? 1 : 0) | (d ? 2 : 0) | (l ? 4 : 0) | (r ? 8 : 0)];   // bit 0 up, 1 down, 2 left, 3 right
}
}  // namespace detail

namespace detail {
// The classic view: two columns of lines (the categories as headers, their entries below). The categories keep their order; a category marked
// 'L' stands in the left column, the others are split where the two columns come out most even.
struct CCell { int node = -1; bool header = false; };   // node -1 = a blank line
inline std::vector<std::vector<CCell>> classic_columns(const Tree& t) {
    std::vector<std::vector<CCell>> cats;
    std::vector<int> ids;
    for (int cat : children_of(t, 0)) {
        std::vector<CCell> v{CCell{cat, true}};
        for (int k = cat + 1; k < t.count && t.n[k].depth > t.n[cat].depth; ++k) v.push_back(CCell{k, false});
        cats.push_back(std::move(v));
        ids.push_back(cat);
    }
    const size_t n = cats.size();
    int best_split = static_cast<int>(n), best_diff = 1 << 20;
    for (size_t split = 1; split <= n; ++split) {   // the first `split` categories (and every 'L' one) go left
        int a = 0, b = 0;
        for (size_t k = 0; k < n; ++k) {
            const bool left = k < split || t.n[ids[k]].column == 'L';
            (left ? a : b) += static_cast<int>(cats[k].size()) + 1;
        }
        const int diff = std::abs(a - b);
        if (diff < best_diff) { best_diff = diff; best_split = static_cast<int>(split); }
    }
    std::vector<std::vector<CCell>> cols(2);
    for (size_t k = 0; k < n; ++k) {
        auto& col = cols[(static_cast<int>(k) < best_split || t.n[ids[k]].column == 'L') ? 0 : 1];
        if (!col.empty()) col.push_back(CCell{});
        col.insert(col.end(), cats[k].begin(), cats[k].end());
    }
    return cols;
}
}  // namespace detail

// The classic view has no choosable headers: a selected category becomes its first entry.
inline int classic_sel(const Tree& t, int sel) {
    sel = clamp_sel(t, sel);
    return t.n[sel].depth == 1 && sel + 1 < t.count ? sel + 1 : sel;
}
// It steers like the earlier index: UP / DOWN the previous / next entry (round and round), LEFT / RIGHT the other column, nearest row.
inline int move_classic(const Tree& t, int sel, int dir) {
    sel = classic_sel(t, sel);
    std::vector<int> ids;   // the choosable entries in reading order
    for (int i = 1; i < t.count; ++i) if (t.n[i].depth >= 2) ids.push_back(i);
    if (ids.empty()) return sel;
    if (dir == 0 || dir == 1) {
        size_t at = 0;
        for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == sel) at = i;
        return ids[(at + (dir == 1 ? 1 : ids.size() - 1)) % ids.size()];
    }
    const auto cols = detail::classic_columns(t);
    int col = 0;
    size_t row = 0;
    for (int c = 0; c < 2; ++c)
        for (size_t r = 0; r < cols[static_cast<size_t>(c)].size(); ++r) if (cols[static_cast<size_t>(c)][r].node == sel) { col = c; row = r; }
    const auto& other = cols[static_cast<size_t>(1 - col)];
    for (size_t d = 0; d < std::max(cols[0].size(), cols[1].size()); ++d) {
        if (row >= d && row - d < other.size() && other[row - d].node >= 0 && !other[row - d].header) return other[row - d].node;
        if (row + d < other.size() && other[row + d].node >= 0 && !other[row + d].header) return other[row + d].node;
    }
    return sel;
}

namespace detail {
struct Level { std::vector<int> nodes; int active = -1; int parent = -1; };
// how a level is drawn: boxes in a row when they fit, else columns of entries hanging from a bar under the parent
enum Kind { Row, Zig, Cols };
struct Plan { Kind kind = Row; int gap = 3; int pad = 4; bool keys = true; int lw = 0, colw = 0, cols = 1, rows = 1; };
struct Layout { std::vector<int> chain; std::vector<Level> levels; std::vector<Plan> plan; };

// Zigzag: the boxes of a row that is too long alternate between an upper and a lower row, so the keys can stay in them. A lower box hangs on a
// branch that passes through the gap between two upper boxes; the centre lines of the upper boxes stay free below them for the next level.
struct ZigPos { std::vector<int> x; int total = 0; };
inline ZigPos zig_layout(const std::vector<int>& w) {
    ZigPos z;
    const size_t n = w.size();
    z.x.assign(n, 0);
    int up_right = 0, up_cx = 0, low_right = -1000, low_cx = -1000;
    bool have_low = false;
    for (size_t i = 0; i < n; ++i) {
        const int wi = w[i];
        if (i % 2 == 0) {   // upper
            int left = i == 0 ? 0 : up_right + 2;
            if (have_low) { left = std::max(left, low_cx + 2); left = std::max(left, low_right + 2 - wi / 2); }
            z.x[i] = left; up_right = left + wi; up_cx = left + wi / 2;
        } else {            // lower
            int left = std::max(up_right + 2 - wi / 2, up_cx + 2);
            if (have_low) left = std::max(left, low_right + 1);
            z.x[i] = left; low_right = left + wi; low_cx = left + wi / 2; have_low = true;
        }
    }
    int mn = 1 << 20, mx = -(1 << 20);
    for (size_t i = 0; i < n; ++i) { mn = std::min(mn, z.x[i]); mx = std::max(mx, z.x[i] + w[i]); }
    for (size_t i = 0; i < n; ++i) z.x[i] -= mn;
    z.total = n ? mx - mn : 0;
    return z;
}
inline std::string label_of(const Tree& t, const Theme& th, int i, bool with_key) {
    const Node& n = t.n[i];
    std::string s = n.label ? n.label : n.title;
    if (with_key && n.action && th.key_text) { const std::string k = th.key_text(n); if (!k.empty()) s += " [" + k + "]"; }
    return s;
}
inline std::string key_of(const Tree& t, const Theme& th, int i) { const Node& n = t.n[i]; return n.action && th.key_text ? th.key_text(n) : std::string(); }

inline Layout make_layout(const Tree& t, const Theme& th, int sel, int room, bool prefer_zig = false) {
    Layout L;
    for (int k = sel; k >= 0; k = parent_of(t, k)) L.chain.insert(L.chain.begin(), k);
    L.levels.push_back(Level{{0}, 0, -1});
    for (size_t i = 1; i < L.chain.size(); ++i) L.levels.push_back(Level{children_of(t, L.chain[i - 1]), L.chain[i], L.chain[i - 1]});
    if (!children_of(t, sel).empty()) L.levels.push_back(Level{children_of(t, sel), -1, sel});
    L.plan.assign(L.levels.size(), Plan());
    for (size_t li = 0; li < L.levels.size(); ++li) {
        const Level& lv = L.levels[li];
        auto total = [&](int gap, int pad, bool keys) {
            int s = 0;
            for (size_t i = 0; i < lv.nodes.size(); ++i) s += std::max(9, width_of(label_of(t, th, lv.nodes[i], keys)) + pad) + (i ? gap : 0);
            return s;
        };
        Plan p;
        bool any_key = false;
        for (int nn : lv.nodes) if (!key_of(t, th, nn).empty()) any_key = true;
        auto zig_total = [&](int pad) {
            std::vector<int> w;
            for (int nn : lv.nodes) w.push_back(std::max(9, width_of(label_of(t, th, nn, true)) + pad));
            return zig_layout(w).total;
        };
        // boxes in a row, narrow (the keys in them); a zigzag of two rows when the row is long and the rows below have the space (prefer_zig);
        // then, when it does not fit, the zigzag anyway, a row without the keys, columns
        const bool long_row = lv.nodes.size() >= 4 && total(2, 2, true) * 5 > room * 2;   // (more than two fifths of the window)
        if (prefer_zig && any_key && long_row && zig_total(2) <= room) { p.kind = Zig; p.pad = 2; }
        else if (total(2, 2, true) <= room) { p.gap = 2; p.pad = 2; }
        else if (any_key && lv.nodes.size() >= 3 && zig_total(4) <= room) { p.kind = Zig; p.pad = 4; }
        else if (any_key && lv.nodes.size() >= 3 && zig_total(2) <= room) { p.kind = Zig; p.pad = 2; }
        else if (!any_key && total(3, 4, false) <= room) { p.gap = 3; p.pad = 4; p.keys = false; }
        else if (!any_key && total(2, 2, false) <= room) { p.gap = 2; p.pad = 2; p.keys = false; }
        else if (!any_key && total(1, 2, false) <= room) { p.gap = 1; p.pad = 2; p.keys = false; }
        else {
            p.kind = Cols;
            int lw = 0, kw = 0;
            for (int n : lv.nodes) { lw = std::max(lw, width_of(label_of(t, th, n, false))); const int k = width_of(key_of(t, th, n)); kw = std::max(kw, k ? k + 2 : 0); }
            p.lw = lw;
            p.colw = 3 + lw + (kw ? 2 + kw : 0);
            const int n = static_cast<int>(lv.nodes.size());
            const int cmax = std::max(1, std::min(n, (room + 3) / (p.colw + 3)));
            p.cols = cmax;
            for (int c = 1; c <= cmax; ++c) if ((n + c - 1) / c <= 4) { p.cols = c; break; }
            p.rows = (n + p.cols - 1) / p.cols;
        }
        L.plan[li] = p;
    }
    return L;
}
// The rows a layout takes: a row of boxes 4 (3 + the branches) or 2 when short of room, columns 1 + their rows; Main has no branches above it.
inline int layout_height(const Layout& L, bool tall) {
    int h = 0;
    for (size_t li = 0; li < L.levels.size(); ++li) h += L.plan[li].kind == Cols ? 1 + L.plan[li].rows : L.plan[li].kind == Zig ? (tall ? 7 : 3) : (tall ? 4 : 2);
    return h - 1;
}
inline std::vector<std::string> to_rows(const Canvas& cv) {
    const std::string R = "\x1b[0m";
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(cv.H));
    for (int yy = 0; yy < cv.H; ++yy) {
        std::string line;
        const std::string* prev = nullptr;
        for (int xx = 0; xx < cv.W; ++xx) {
            const Cell& c = cv.c[static_cast<size_t>(yy) * static_cast<size_t>(cv.W) + static_cast<size_t>(xx)];
            if (c.sgr != prev && !(c.sgr && prev && *c.sgr == *prev)) {
                line += R;
                if (c.sgr) line += *c.sgr;
                prev = c.sgr;
            }
            line += c.g;
        }
        line += R;
        out.push_back(std::move(line));
    }
    return out;
}
}  // namespace detail

// The inside of the overlay, W x H cells. The last two rows hold the path ("Main > Sound > Equalizer") and the selected menu with its key.
// The tree starts on the same row whatever is selected, so it only grows downwards.
inline std::vector<std::string> render(const Tree& t, int sel, int W, int H, double time, const Theme& th, const std::string& info) {
    using namespace detail;
    (void)time;
    W = std::max(20, W);
    H = std::max(6, H);
    Canvas cv(W, H);
    sel = clamp_sel(t, sel);
    std::vector<int> chain;   // Main .. the selection
    for (int k = sel; k >= 0; k = parent_of(t, k)) chain.insert(chain.begin(), k);
    auto on_path = [&](int node) { return std::find(chain.begin(), chain.end(), node) != chain.end(); };
    const std::string sel_sgr = th.path + "\x1b[1m";   // the selection: the header colour in bold
    const std::string pend = th.pending.empty() ? th.line : th.pending;   // not on the path: the progress bar's "remaining" colour
    const std::string walk = th.walked.empty() ? th.path : th.walked;      // the branches on the path: its "played" colour
    const char* tl = "\xe2\x95\xad", *tr = "\xe2\x95\xae", *bl = "\xe2\x95\xb0", *br = "\xe2\x95\xaf";   // ╭ ╮ ╰ ╯
    const char* hz = "\xe2\x94\x80", *vt = "\xe2\x94\x82";

    {
        const int room = W - 4;
        const int y0 = 1;
        const int avail = H - 3 - y0;   // rows for the boxes (a spare row, the path and the selected menu below)
        bool tall = true;   // the boxes' height is the same whatever is selected: tall only if the deepest tree of all fits
        for (int s = 1; s < t.count && tall; ++s) if (layout_height(make_layout(t, th, s, room), true) > avail) tall = false;
        Layout L = make_layout(t, th, sel, room, true);   // the zigzag where the rows below have the space for it
        if (layout_height(L, tall) > avail) L = make_layout(t, th, sel, room, false);
        const int nl = static_cast<int>(L.levels.size());
        const int bh = tall ? 3 : 1;
        int y = y0;
        int parent_cx = W / 2;
        for (int li = 0; li < nl; ++li) {
            const Level& lv = L.levels[static_cast<size_t>(li)];
            const Plan& pl = L.plan[static_cast<size_t>(li)];
            const int n = static_cast<int>(lv.nodes.size());
            if (pl.kind == Cols) {
                // columns hanging from a bar under the parent; the entries go along the rows, so RIGHT is the next column of the same row
                const int c = pl.cols, rows = pl.rows;
                const int bw = c * pl.colw + (c - 1) * 3;
                const int x0 = c == 1 ? std::clamp(parent_cx, 2, std::max(2, W - 2 - pl.colw)) : std::clamp(parent_cx - bw / 2, 2, std::max(2, W - 2 - bw));
                auto trunk_of = [&](int col) { return x0 + col * (pl.colw + 3); };
                int act = -1;
                for (int i = 0; i < n; ++i) if (lv.nodes[static_cast<size_t>(i)] == lv.active) act = i % c;
                const int act_x = act >= 0 ? trunk_of(act) : -1;
                if (li > 0) {   // the bar from the parent's box to the columns
                    const int a = std::min(parent_cx, trunk_of(0)), b = std::max(parent_cx, trunk_of(c - 1));
                    for (int x = a; x <= b; ++x) {
                        bool down = false;
                        for (int k = 0; k < c; ++k) if (trunk_of(k) == x) down = true;
                        const bool lit = act_x >= 0 && x >= std::min(parent_cx, act_x) && x <= std::max(parent_cx, act_x);
                        cv.put(x, y, junction(x == parent_cx, down, x > a, x < b), lit ? walk : pend);
                    }
                    ++y;
                }
                for (int i = 0; i < n; ++i) {
                    const int col = i % c, row = i / c;
                    const int node = lv.nodes[static_cast<size_t>(i)];
                    const bool is_sel = node == sel, path = on_path(node);
                    const bool last = i + c >= n;
                    const int tx = trunk_of(col), ry = y + row;
                    put_text(cv, tx, ry, last ? "\xe2\x94\x94\xe2\x94\x80" : "\xe2\x94\x9c\xe2\x94\x80", path ? walk : pend);   // └─ / ├─
                    put_text(cv, tx + 2, ry, is_sel ? "\xe2\x96\xb8" : " ", th.path);   // ▸ marks the selection
                    put_text(cv, tx + 3, ry, label_of(t, th, node, false), is_sel ? sel_sgr : path ? th.path : "");
                    const std::string key = key_of(t, th, node);
                    if (!key.empty()) put_text(cv, tx + 3 + pl.lw + 2, ry, "[" + key + "]", th.key);
                }
                int next_parent = parent_cx;
                if (act >= 0) {   // the open entry's trunk runs on to the level below
                    next_parent = act_x;
                    int cnt = 0;
                    for (int i = 0; i < n; ++i) if (i % c == act) ++cnt;
                    for (int r = cnt; r < rows; ++r) cv.put(act_x, y + r, vt, walk);
                }
                y += rows;
                parent_cx = next_parent;
                continue;
            }
            if (pl.kind == Zig) {
                std::vector<int> w(static_cast<size_t>(n));
                for (int i = 0; i < n; ++i) w[static_cast<size_t>(i)] = std::max(9, width_of(label_of(t, th, lv.nodes[static_cast<size_t>(i)], true)) + pl.pad);
                const ZigPos z = zig_layout(w);
                const int base = std::clamp(parent_cx - z.total / 2, 3, std::max(3, W - 3 - z.total));
                int mn = parent_cx, mx = parent_cx, act_x = -1;
                for (int i = 0; i < n; ++i) {
                    const int cx = base + z.x[static_cast<size_t>(i)] + w[static_cast<size_t>(i)] / 2;
                    mn = std::min(mn, cx); mx = std::max(mx, cx);
                    if (lv.nodes[static_cast<size_t>(i)] == lv.active) act_x = cx;
                }
                if (li > 0) {   // the long bar from the parent's box
                    for (int cx = mn; cx <= mx; ++cx) {
                        bool down = false;
                        for (int i = 0; i < n; ++i) if (base + z.x[static_cast<size_t>(i)] + w[static_cast<size_t>(i)] / 2 == cx) down = true;
                        const bool lit = act_x >= 0 && cx >= std::min(parent_cx, act_x) && cx <= std::max(parent_cx, act_x);
                        cv.put(cx, y, junction(cx == parent_cx, down, cx > mn, cx < mx), lit ? walk : pend);
                    }
                    ++y;
                }
                int next_parent = parent_cx;
                for (int i = 0; i < n; ++i) {
                    const int node = lv.nodes[static_cast<size_t>(i)], pw = w[static_cast<size_t>(i)];
                    const int px = base + z.x[static_cast<size_t>(i)], cx = px + pw / 2;
                    const bool lower = i % 2 == 1, is_sel = node == sel, path = on_path(node);
                    const std::string edge = path ? th.path : pend;   // (the box on the path: its border in the header colour like its text; the others in the "remaining" colour)
                    const int ry = lower ? y + bh : y;
                    if (lower) for (int d = 0; d < bh; ++d) cv.put(cx, y + d, vt, path ? walk : pend);   // its branch runs down between the upper boxes
                    const std::string lab = label_of(t, th, node, false);
                    const std::string key = key_of(t, th, node);
                    const std::string kt = key.empty() ? std::string() : " [" + key + "]";
                    const int lw = width_of(lab) + width_of(kt);
                    const int tx = px + (pw - lw) / 2;
                    auto text = [&](int row_y) {
                        put_text(cv, tx, row_y, lab, is_sel ? sel_sgr : path ? th.path : "");
                        put_text(cv, tx + width_of(lab), row_y, kt, th.key);
                    };
                    if (tall) {
                        for (int dx = 0; dx < pw; ++dx) {
                            cv.put(px + dx, ry, dx == 0 ? tl : dx == pw - 1 ? tr : hz, edge);
                            cv.put(px + dx, ry + 2, dx == 0 ? bl : dx == pw - 1 ? br : hz, edge);
                        }
                        text(ry + 1);
                        cv.put(px, ry + 1, vt, edge); cv.put(px + pw - 1, ry + 1, vt, edge);
                    } else {
                        text(ry);
                        cv.put(px, ry, "(", edge); cv.put(px + pw - 1, ry, ")", edge);
                    }
                    if (node == lv.active || (lv.active < 0 && li + 1 == nl)) {
                        next_parent = cx;
                        if (!lower && li + 1 < nl) for (int d = 0; d < bh; ++d) cv.put(cx, y + bh + d, vt, walk);   // down past the lower row
                    }
                }
                y += 2 * bh;
                parent_cx = next_parent;
                continue;
            }
            // a row of boxes
            std::vector<int> w(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) w[static_cast<size_t>(i)] = std::max(9, width_of(label_of(t, th, lv.nodes[static_cast<size_t>(i)], pl.keys)) + pl.pad);
            int total = 0;
            for (int i = 0; i < n; ++i) total += w[static_cast<size_t>(i)] + (i ? pl.gap : 0);
            int x = std::clamp(parent_cx - total / 2, 3, std::max(3, W - 3 - total));
            struct Placed { int node, x, w; };
            std::vector<Placed> row;
            for (int i = 0; i < n; ++i) { row.push_back(Placed{lv.nodes[static_cast<size_t>(i)], x, w[static_cast<size_t>(i)]}); x += w[static_cast<size_t>(i)] + pl.gap; }
            if (li > 0) {   // the branches from the parent's box
                int mn = parent_cx, mx = parent_cx, act_x = -1;
                for (const auto& p : row) { mn = std::min(mn, p.x + p.w / 2); mx = std::max(mx, p.x + p.w / 2); if (p.node == lv.active) act_x = p.x + p.w / 2; }
                for (int cx = mn; cx <= mx; ++cx) {
                    bool down = false;
                    for (const auto& p : row) if (p.x + p.w / 2 == cx) down = true;
                    const bool lit = act_x >= 0 && cx >= std::min(parent_cx, act_x) && cx <= std::max(parent_cx, act_x);
                    cv.put(cx, y, junction(cx == parent_cx, down, cx > mn, cx < mx), lit ? walk : pend);
                }
                ++y;
            }
            int next_parent = parent_cx;
            for (const auto& p : row) {
                const bool is_sel = p.node == sel, path = on_path(p.node);
                const std::string edge = path ? th.path : pend;   // (the box on the path: its border in the header colour like its text; the others in the "remaining" colour)
                const std::string lab = label_of(t, th, p.node, false);
                const std::string key = key_of(t, th, p.node);
                const std::string kt = key.empty() || !pl.keys ? std::string() : " [" + key + "]";
                const int lw = width_of(lab) + width_of(kt);
                const int tx = p.x + (p.w - lw) / 2;
                auto text = [&](int row_y) {
                    put_text(cv, tx, row_y, lab, is_sel ? sel_sgr : path ? th.path : "");
                    put_text(cv, tx + width_of(lab), row_y, kt, th.key);
                };
                if (tall) {
                    for (int dx = 0; dx < p.w; ++dx) {
                        cv.put(p.x + dx, y, dx == 0 ? tl : dx == p.w - 1 ? tr : hz, edge);
                        cv.put(p.x + dx, y + 2, dx == 0 ? bl : dx == p.w - 1 ? br : hz, edge);
                    }
                    text(y + 1);
                    cv.put(p.x, y + 1, vt, edge);
                    cv.put(p.x + p.w - 1, y + 1, vt, edge);
                } else {
                    text(y);
                    cv.put(p.x, y, "(", edge); cv.put(p.x + p.w - 1, y, ")", edge);
                }
                if (p.node == lv.active || (lv.active < 0 && li + 1 == nl)) next_parent = p.x + p.w / 2;
            }
            y += bh;
            parent_cx = next_parent;
        }
    }

    // the path and the selected menu
    std::string crumb;
    for (size_t i = 0; i < chain.size(); ++i) crumb += (i ? "  \xe2\x80\xba  " : "") + std::string(t.n[chain[i]].title);
    put_text(cv, std::max(0, (W - width_of(clip(crumb, W - 2))) / 2), H - 2, clip(crumb, W - 2), pend);
    const std::string inf = clip(info, W - 2);
    put_text(cv, std::max(0, (W - width_of(inf)) / 2), H - 1, inf, th.path);
    return detail::to_rows(cv);
}


// The classic view as a panel of its own size: two columns of lines, the categories (and the groups inside them) in the header colour and not
// choosable, the keys at the right edge of their column, the selection in reverse video. `rows` are exactly `inner_w` columns wide.
struct ClassicPanel { std::vector<std::string> rows; int inner_w = 0; };
inline ClassicPanel render_classic(const Tree& t, int sel, const Theme& th, int max_rows, int max_w) {
    using namespace detail;
    sel = classic_sel(t, sel);
    const auto cols = classic_columns(t);
    auto upper = [](std::string v) { for (char& ch : v) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A'); return v; };
    auto key_text = [&](const CCell& c) { const std::string k = c.header ? std::string() : key_of(t, th, c.node); return k.empty() ? std::string() : "[" + k + "]"; };
    auto indent_of = [&](const CCell& c) { return c.header ? 0 : 2 + (t.n[c.node].depth - 2) * 2; };
    int cw[2] = {10, 10};
    for (int c = 0; c < 2; ++c)
        for (const CCell& cell : cols[static_cast<size_t>(c)]) {
            if (cell.node < 0) continue;
            const std::string lab = cell.header ? upper(label_of(t, th, cell.node, false)) : label_of(t, th, cell.node, false);
            const std::string kt = key_text(cell);
            cw[c] = std::max(cw[c], indent_of(cell) + width_of(lab) + (kt.empty() ? 0 : 2 + width_of(kt)) + 4);
        }
    max_w = std::max(24, max_w);
    while (cw[0] + 1 + cw[1] > max_w) (cw[0] >= cw[1] ? cw[0] : cw[1])--;
    const int W = cw[0] + 1 + cw[1];
    const size_t rows = std::max(cols[0].size(), cols[1].size());
    const size_t room = static_cast<size_t>(std::max(1, max_rows - 2));   // (a blank row above and below)
    const size_t shown = std::min(rows, room);
    size_t first = 0;
    if (rows > room) {
        size_t sel_row = 0;
        for (int c = 0; c < 2; ++c)
            for (size_t r = 0; r < cols[static_cast<size_t>(c)].size(); ++r) if (cols[static_cast<size_t>(c)][r].node == sel) sel_row = r;
        first = sel_row > room / 2 ? sel_row - room / 2 : 0;
        if (first + room > rows) first = rows - room;
    }
    Canvas cv(W, static_cast<int>(shown) + 2);
    for (size_t r = first; r < first + shown; ++r)
        for (int c = 0; c < 2; ++c) {
            const auto& col = cols[static_cast<size_t>(c)];
            if (r >= col.size() || col[r].node < 0) continue;
            const CCell& cell = col[r];
            const int x0 = c == 0 ? 0 : cw[0] + 1, tw = cw[c] - 4, y = 1 + static_cast<int>(r - first);
            const bool is_sel = !cell.header && cell.node == sel;
            const bool head = cell.header || !t.n[cell.node].action;   // categories and groups: the header colour
            const std::string kt = key_text(cell);
            const int indent = indent_of(cell);
            std::string lab = label_of(t, th, cell.node, false);
            if (cell.header) lab = upper(lab);
            if (is_sel) for (int dx = 0; dx < cw[c]; ++dx) cv.put(x0 + dx, y, " ", "\x1b[7m");
            const std::string text = clip((is_sel ? std::string("> ") : std::string(static_cast<size_t>(indent >= 2 ? 2 : 0), ' ')) + std::string(static_cast<size_t>(std::max(0, indent - 2)), ' ') + lab,
                                          std::max(1, tw - (kt.empty() ? 0 : width_of(kt) + 1)));
            put_text(cv, x0 + 2, y, text, is_sel ? "\x1b[7m" : head ? th.path : "");
            if (!kt.empty()) put_text(cv, x0 + 2 + tw - width_of(kt), y, kt, is_sel ? "\x1b[7m" : th.key);
        }
    for (int y = 0; y < cv.H; ++y) cv.put(cw[0], y, "\xe2\x94\x82", th.line);
    ClassicPanel pnl;
    pnl.rows = to_rows(cv);
    pnl.inner_w = W;
    return pnl;
}

}  // namespace org
}  // namespace muisc
