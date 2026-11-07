// Pictures in the terminal: draws images with the Kitty graphics protocol (or falls back to text),
// detected at start-up.
#include "terminal_gfx.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#if !defined(_WIN32)
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#include "miniz.h"

namespace muisc {

namespace {

constexpr int kImageId = 7201;

std::string base64(const uint8_t* d, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(d[i]) << 16) | (static_cast<uint32_t>(d[i + 1]) << 8) | d[i + 2];
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
    }
    if (i + 1 == n) {
        const uint32_t v = static_cast<uint32_t>(d[i]) << 16;
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += "==";
    } else if (i + 2 == n) {
        const uint32_t v = (static_cast<uint32_t>(d[i]) << 16) | (static_cast<uint32_t>(d[i + 1]) << 8);
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '=';
    }
    return o;
}

std::string at_cell(int row, int col) { return "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) + "H"; }

// brightness curve shared by both protocols: faint stays faint, the core turns white-hot
float curve(float l) { return std::pow(std::clamp(l, 0.0f, 1.0f), 0.85f); }
void shade(const GfxFrame& f, uint8_t level, uint8_t hue, float& r, float& g, float& b, float& a) {
    const float l = static_cast<float>(level) / 255.0f;
    a = curve(l);
    const float hot = std::clamp((l - 0.80f) / 0.20f, 0.0f, 1.0f) * 0.40f;   // a white core on the brightest pixels
    r = static_cast<float>(f.pal[hue][0]) * (1.0f - hot) + 255.0f * hot;
    g = static_cast<float>(f.pal[hue][1]) * (1.0f - hot) + 255.0f * hot;
    b = static_cast<float>(f.pal[hue][2]) * (1.0f - hot) + 255.0f * hot;
}

// The grid layer: a grid pixel is always drawn and the beam shows only between the grid lines. (Letting the beam replace grid
// pixels, by a threshold, a hold or "the brighter wins", made grid lines blink wherever a signal ran along them or touched the
// borders of the picture: with no afterglow the pixel is the beam for one picture and the grid again for the next.)
inline bool grid_shows(const GfxFrame& f, size_t i) {
    return !f.grid.empty() && f.grid[i];
}

std::string kitty_emit(const GfxFrame& f) {
    // crop in picture pixels (the picture may be smaller than the cells: the terminal scales it to c x r cells)
    const int cropx = static_cast<int>(static_cast<long long>(std::clamp(f.crop, 0, f.cols)) * f.w / std::max(1, f.cols));
    const int vis_cols = f.cols - std::clamp(f.crop, 0, f.cols);
    if (vis_cols <= 0) return gfx_clear(GfxProto::Kitty, f.id);
    std::vector<uint8_t> rgba(static_cast<size_t>(f.w) * f.h * 4);
    const bool true_colour = f.plain && f.direct && f.truecolor && f.rgb.size() >= static_cast<size_t>(f.w) * f.h * 3 && f.level.size() >= static_cast<size_t>(f.w) * f.h;
    for (size_t i = 0; i < static_cast<size_t>(f.w) * f.h; ++i) {
        if (true_colour) {   // the video's own colours, all 8 bits
            if (f.level[i] < 3) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + 3] = 0; continue; }
            rgba[i * 4] = f.rgb[i * 3]; rgba[i * 4 + 1] = f.rgb[i * 3 + 1]; rgba[i * 4 + 2] = f.rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255;
            continue;
        }
        if (grid_shows(f, i)) {   // the grid shows where the beam is fainter than it
            rgba[i * 4] = f.grid_rgb[0] & 0xF8; rgba[i * 4 + 1] = f.grid_rgb[1] & 0xF8; rgba[i * 4 + 2] = f.grid_rgb[2] & 0xF8; rgba[i * 4 + 3] = 0xF8;
            continue;
        }
        if (f.level[i] < (f.plain ? 3 : 8)) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + 3] = 0; continue; }
        float r, g, b, a;
        shade(f, f.level[i], f.hue[i], r, g, b, a);
        if (f.plain) { a = static_cast<float>(f.level[i]) / 255.0f; r = f.pal[f.hue[i]][0]; g = f.pal[f.hue[i]][1]; b = f.pal[f.hue[i]][2]; }
        // 5 bits per channel: invisible on screen, and it halves what has to be sent (the bloom is full of near-equal values)
        rgba[i * 4] = static_cast<uint8_t>(r) & 0xF8; rgba[i * 4 + 1] = static_cast<uint8_t>(g) & 0xF8; rgba[i * 4 + 2] = static_cast<uint8_t>(b) & 0xF8;
        rgba[i * 4 + 3] = static_cast<uint8_t>(std::clamp(a * 255.0f, 0.0f, 255.0f)) & 0xF8;
    }
    std::string payload;
    std::string comp = "";
    {   // zlib stream (miniz, vendored in third_party/miniz): level 1 is fast and the picture is mostly empty
        mz_ulong n = mz_compressBound(static_cast<mz_ulong>(rgba.size()));
        std::vector<uint8_t> z(n);
        if (mz_compress2(z.data(), &n, rgba.data(), static_cast<mz_ulong>(rgba.size()), 1) == MZ_OK) { payload = base64(z.data(), n); comp = ",o=z"; }
    }
    if (comp.empty()) payload = base64(rgba.data(), rgba.size());
    std::string keys = "a=T,f=32,s=" + std::to_string(f.w) + ",v=" + std::to_string(f.h) + ",i=" + std::to_string(kImageId + f.id) + ",p=1,z=-1,C=1,q=2"
                     + comp + ",c=" + std::to_string(vis_cols) + ",r=" + std::to_string(f.rows);
    if (cropx > 0) keys += ",x=" + std::to_string(cropx) + ",y=0,w=" + std::to_string(f.w - cropx) + ",h=" + std::to_string(f.h);
    std::string o = at_cell(f.row, f.col + (f.cols - vis_cols));
    constexpr size_t kChunk = 4096;
    for (size_t off = 0; off < payload.size() || off == 0; off += kChunk) {
        const bool more = off + kChunk < payload.size();
        o += "\x1b_G";
        if (off == 0) o += keys + ",";
        o += std::string("m=") + (more ? "1" : "0") + ";" + payload.substr(off, kChunk) + "\x1b\\";
        if (!more) break;
    }
    return o;
}

// ---- Sixel: 16 colours (from the hue) x 15 brightness steps; pixels below the threshold stay transparent (P2 = 1) ----
// Written for speed, because it runs every frame: one pass per 6-row band collects the bit columns of only the colours
// that occur in that band, each colour line stops at its last pixel, and runs (also the empty start) are length-coded.
// A picture smaller than the screen area (scale > 1) is enlarged here: columns by repeating them (cheap, the run-length
// code absorbs it), rows through the pixel aspect ratio where the terminal honours it (Windows Terminal), else by
// repeating them as well.
bool sixel_aspect_ok() {
    static const bool ok = std::getenv("WT_SESSION") != nullptr || std::getenv("MOUSIKI_SIXEL_ASPECT") != nullptr;
    return ok;
}

inline void put_int(std::string& o, int v) {
    char b[12];
    int n = 0;
    if (v == 0) { o += '0'; return; }
    while (v > 0) { b[n++] = static_cast<char>('0' + v % 10); v /= 10; }
    while (n > 0) o += b[--n];
}
inline void put_run(std::string& o, int run, char ch) {
    if (run <= 0) return;
    if (run > 3) { o += '!'; put_int(o, run); o += ch; }
    else o.append(static_cast<size_t>(run), ch);
}

std::string sixel_emit(const GfxFrame& f) {
    static std::string pal_text;
    static std::array<std::array<uint8_t, 3>, 256> pal_seen{};
    static bool pal_valid = false;
    static bool plain_seen = false;
    static std::array<uint8_t, 3> grid_seen{};
    static std::vector<uint8_t> bits;           // per colour: one 6-bit mask per output column (kept zeroed between bands)
    static std::vector<int> minx(256), maxx(256);
    const int cols = std::max(1, f.cols);
    const int crop = std::clamp(f.crop, 0, f.cols);
    const int vis_cols = f.cols - crop;
    if (vis_cols <= 0 || f.w <= 0 || f.h <= 0) return "";
    // No new picture: nothing to send. The text frame skips the picture's cells (gfx_fill_holes), so the last
    // picture stays on screen untouched -- sending it again every frame only flooded the terminal.
    if (!f.fresh) return "";
    const int ocw = f.out_cw > 0 ? f.out_cw : std::max(1, f.w / cols);
    const int och = f.out_ch > 0 ? f.out_ch : std::max(1, f.h / std::max(1, f.rows));
    const int scale = std::max(1, f.scale);
    const int full_w = cols * ocw, full_h = std::max(1, f.rows) * och;
    const int W = vis_cols * ocw;                                    // output columns (screen pixels)
    const int pan = (scale > 1 && sixel_aspect_ok()) ? scale : 1;    // screen rows per sixel row
    const int H = (full_h + pan - 1) / pan;                          // sixel rows
    // output column -> picture column, sixel row -> picture row (nearest neighbour)
    std::vector<int> xs(static_cast<size_t>(W)), ys(static_cast<size_t>(H));
    for (int x = 0; x < W; ++x) xs[static_cast<size_t>(x)] = std::min(f.w - 1, static_cast<int>(static_cast<long long>(crop * ocw + x) * f.w / full_w));
    for (int y = 0; y < H; ++y) ys[static_cast<size_t>(y)] = std::min(f.h - 1, static_cast<int>(static_cast<long long>(y) * pan * f.h / full_h));
    // brightness -> step once per level value, then colour index per picture pixel (0 = transparent)
    uint8_t step_of[256];
    for (int l = 0; l < 256; ++l) step_of[l] = static_cast<uint8_t>(std::lround(curve(static_cast<float>(l) / 255.0f) * 15.0f));
    std::vector<uint8_t> idx(static_cast<size_t>(f.w) * f.h, 0);
    for (size_t i = 0; i < idx.size(); ++i) {
        const int st = step_of[f.level[i]];
        if (grid_shows(f, i)) idx[i] = 242;   // the grid colour, under the beam
        else if (f.direct) { if (f.level[i] >= 3) idx[i] = f.hue[i] >= 240 ? static_cast<uint8_t>(244 + std::min<int>(f.hue[i] - 240, 7)) : static_cast<uint8_t>(1 + f.hue[i]); }   // 240 .. 243: extra colours, registers 244 ..
        else if (st >= 1) idx[i] = static_cast<uint8_t>(1 + (f.hue[i] >> 4) * 15 + (st - 1));
    }
    // The picture is opaque: every pixel that is not beam or grid is drawn black (colour 243), so a new picture replaces the old
    // one in place. (It used to erase the cells first and draw on transparent pixels -- when the terminal painted between the
    // erase and the picture, which happens with the big pictures of loud music, the whole picture blinked away for a frame.)
    // The cells a floating panel covers stay transparent.
    {
        int xa = -1, xb = -1, ya = -1, yb = -1, xa2 = -1, xb2 = -1, ya2 = -1, yb2 = -1;
        if (f.cut_w > 0) {
            xa = f.cut_c * f.w / cols - 1; xb = (f.cut_c + f.cut_w) * f.w / cols + 1;
            ya = f.cut_r * f.h / std::max(1, f.rows) - 1; yb = (f.cut_r + f.cut_h) * f.h / std::max(1, f.rows) + 1;
        }
        if (f.cut2_w > 0) {
            xa2 = f.cut2_c * f.w / cols - 1; xb2 = (f.cut2_c + f.cut2_w) * f.w / cols + 1;
            ya2 = f.cut2_r * f.h / std::max(1, f.rows) - 1; yb2 = (f.cut2_r + f.cut2_h) * f.h / std::max(1, f.rows) + 1;
        }
        for (int y = 0; y < f.h; ++y) {
            const bool yin = y >= ya && y < yb, yin2 = y >= ya2 && y < yb2;
            for (int x = 0; x < f.w; ++x) {
                uint8_t& c = idx[static_cast<size_t>(y) * f.w + x];
                if (c == 0 && (!f.plain || f.opaque) && !(yin && x >= xa && x < xb) && !(yin2 && x >= xa2 && x < xb2)) c = 243;
            }
        }
    }
    // the palette only changes with the colour settings: build its text once
    static bool direct_seen = false;
    if (!pal_valid || pal_seen != f.pal || grid_seen != f.grid_rgb || plain_seen != f.plain || direct_seen != f.direct) {
        pal_text.clear();
        if (f.direct) {   // the picture's own colours: colour 1 + i is pal[i]
            for (int i = 0; i < 240; ++i) {
                pal_text += '#'; put_int(pal_text, 1 + i);
                pal_text += ";2;"; put_int(pal_text, f.pal[static_cast<size_t>(i)][0] * 100 / 255); pal_text += ';';
                put_int(pal_text, f.pal[static_cast<size_t>(i)][1] * 100 / 255); pal_text += ';'; put_int(pal_text, f.pal[static_cast<size_t>(i)][2] * 100 / 255);
            }
            for (int i = 240; i < 248; ++i) {   // the extra colours (the Game Boy greens and greys): registers 244 ..
                pal_text += '#'; put_int(pal_text, 4 + i);
                pal_text += ";2;"; put_int(pal_text, f.pal[static_cast<size_t>(i)][0] * 100 / 255); pal_text += ';';
                put_int(pal_text, f.pal[static_cast<size_t>(i)][1] * 100 / 255); pal_text += ';'; put_int(pal_text, f.pal[static_cast<size_t>(i)][2] * 100 / 255);
            }
        } else
        for (int hb = 0; hb < 16; ++hb)
            for (int st = 1; st <= 15; ++st) {
                const float l = static_cast<float>(st) / 15.0f;
                const float hot = f.plain ? 0.0f : std::clamp((l - 0.80f) / 0.20f, 0.0f, 1.0f) * 0.40f;
                const auto& p = f.pal[static_cast<size_t>(hb * 16 + 8)];
                auto ch = [&](int c) { return static_cast<int>(std::lround(((static_cast<float>(p[static_cast<size_t>(c)]) * (1.0f - hot) + 255.0f * hot) * l) / 255.0f * 100.0f)); };
                pal_text += '#'; put_int(pal_text, 1 + hb * 15 + st - 1);
                pal_text += ";2;"; put_int(pal_text, ch(0)); pal_text += ';'; put_int(pal_text, ch(1)); pal_text += ';'; put_int(pal_text, ch(2));
            }
        pal_text += "#243;2;0;0;0";   // the opaque background of the picture
        pal_text += "#242;2;";   // the grid colour (colour 242 is the only one above the beam's 1 + 16 * 15 colours that is used)
        put_int(pal_text, f.grid_rgb[0] * 100 / 255); pal_text += ';'; put_int(pal_text, f.grid_rgb[1] * 100 / 255); pal_text += ';'; put_int(pal_text, f.grid_rgb[2] * 100 / 255);
        pal_seen = f.pal;
        grid_seen = f.grid_rgb;
        pal_valid = true;
        plain_seen = f.plain;
        direct_seen = f.direct;
    }
    if (bits.size() != static_cast<size_t>(256) * W) bits.assign(static_cast<size_t>(256) * W, 0);
    std::string o;
    o.reserve(65536);
    o += "\x1b[0m";
    if (f.erase && f.plain) {   // blank the picture's cells (not the ones something else covers), then draw on them
        auto blank = [&](int r, int from, int to) { if (to > from) { o += at_cell(f.row + r, f.col + from); o.append(static_cast<size_t>(to - from), ' '); } };
        for (int r = 0; r < f.rows; ++r) {
            // the cells of the cut rectangles (a floating panel, the lyric box) are left out
            int from = crop;
            for (int c = crop; c <= f.cols; ++c) {
                const bool in_cut = c < f.cols && ((f.cut_w > 0 && r >= f.cut_r && r < f.cut_r + f.cut_h && c >= f.cut_c && c < f.cut_c + f.cut_w) ||
                                                   (f.cut2_w > 0 && r >= f.cut2_r && r < f.cut2_r + f.cut2_h && c >= f.cut2_c && c < f.cut2_c + f.cut2_w));
                if (in_cut || c == f.cols) { blank(r, from, c); from = c + 1; }
            }
        }
    }
    o += at_cell(f.row, f.col + crop);
    o += "\x1bP0;1;0q\""; put_int(o, pan); o += ";1;"; put_int(o, W); o += ';'; put_int(o, H);
    o += pal_text;
    std::vector<int> used;
    used.reserve(256);
    std::vector<char> seen(256, 0);
    for (int by = 0; by < H; by += 6) {
        const int bh = std::min(6, H - by);
        used.clear();
        for (int y = 0; y < bh; ++y) {
            const uint8_t* row = idx.data() + static_cast<size_t>(ys[static_cast<size_t>(by + y)]) * f.w;
            const uint8_t bit = static_cast<uint8_t>(1u << y);
            for (int x = 0; x < W; ++x) {
                const int c = row[xs[static_cast<size_t>(x)]];
                if (c == 0) continue;
                if (!seen[static_cast<size_t>(c)]) { seen[static_cast<size_t>(c)] = 1; used.push_back(c); minx[static_cast<size_t>(c)] = x; maxx[static_cast<size_t>(c)] = x; }
                else { minx[static_cast<size_t>(c)] = std::min(minx[static_cast<size_t>(c)], x); maxx[static_cast<size_t>(c)] = std::max(maxx[static_cast<size_t>(c)], x); }
                bits[static_cast<size_t>(c) * W + x] |= bit;
            }
        }
        for (size_t u = 0; u < used.size(); ++u) {
            const int c = used[u];
            uint8_t* b = bits.data() + static_cast<size_t>(c) * W;
            const int x0 = minx[static_cast<size_t>(c)], x1 = maxx[static_cast<size_t>(c)];
            o += '#'; put_int(o, c);
            put_run(o, x0, '?');
            int run = 0; char prev = 0;
            for (int x = x0; x <= x1; ++x) {
                const char ch = static_cast<char>(63 + b[x]);
                b[x] = 0;
                if (run > 0 && ch == prev) ++run;
                else { put_run(o, run, prev); prev = ch; run = 1; }
            }
            put_run(o, run, prev);
            seen[static_cast<size_t>(c)] = 0;
            if (u + 1 < used.size()) o += '$';
        }
        o += '-';
    }
    o += "\x1b\\";
    return o;
}

} // namespace

std::string gfx_emit(GfxProto proto, const GfxFrame& f) {
    if (proto == GfxProto::None || !f.active || f.cols <= 0 || f.rows <= 0) return "";
    if (proto == GfxProto::Kitty) return f.fresh ? kitty_emit(f) : std::string();
    return sixel_emit(f);
}

std::string gfx_clear(GfxProto proto, int id) {
    if (proto == GfxProto::Kitty) return "\x1b_Ga=d,d=I,i=" + std::to_string(kImageId + id) + ",q=2\x1b\\";
    return "";   // Sixel pictures vanish when the cells are written over (every frame does that)
}

const char* gfx_name(GfxProto p) { return p == GfxProto::Kitty ? "kitty" : p == GfxProto::Sixel ? "sixel" : "none"; }

void gfx_cell_pixels(const std::string& override_wxh, int& w, int& h) {
    w = 0; h = 0;
    const char* e = std::getenv("MOUSIKI_CELLPX");
    if (!e || !*e) e = std::getenv("MOUSIKI_RADIO_CELLPX");   // older name, still accepted
    const std::string ov = e && *e ? e : override_wxh;
    int a = 0, b = 0;
    if (std::sscanf(ov.c_str(), "%dx%d", &a, &b) == 2 && a >= 4 && b >= 8) { w = a; h = b; return; }
#if !defined(_WIN32)
    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0 && ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
        w = ws.ws_xpixel / ws.ws_col; h = ws.ws_ypixel / ws.ws_row;
    }
#endif
    if (w < 4 || h < 8) { w = 10; h = 20; }
}

GfxProto gfx_probe(const std::string& pref_in) {
    std::string pref = pref_in;
    if (const char* e = std::getenv("MOUSIKI_GFX")) { if (*e) pref = e; }
    else if (const char* r = std::getenv("MOUSIKI_RADIO_GFX")) { if (*r) pref = r; }   // older name, still accepted
    for (auto& c : pref) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (pref == "off" || pref == "none" || pref == "0" || pref == "false") return GfxProto::None;
    if (pref == "kitty") return GfxProto::Kitty;
    if (pref == "sixel") return GfxProto::Sixel;
    const char* term = std::getenv("TERM");
    const char* prog = std::getenv("TERM_PROGRAM");
    const std::string t = term ? term : "", p = prog ? prog : "";
    const bool env_kitty = std::getenv("KITTY_WINDOW_ID") || t == "xterm-kitty" || t == "xterm-ghostty" || p == "ghostty" || p == "WezTerm";
    const bool env_sixel = std::getenv("WT_SESSION") || t.rfind("foot", 0) == 0 || t.find("sixel") != std::string::npos || t.rfind("mlterm", 0) == 0;
#if !defined(_WIN32)
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return GfxProto::None;
    // Ask: a 1x1 Kitty graphics query, then the primary device attributes (every terminal answers those, so the wait ends).
    const char q[] = "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\\x1b[c";
    if (::write(STDOUT_FILENO, q, sizeof q - 1) < 0) return env_kitty ? GfxProto::Kitty : env_sixel ? GfxProto::Sixel : GfxProto::None;
    std::string reply;
    for (int waited = 0; waited < 250;) {
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        if (::poll(&pfd, 1, 25) <= 0) { waited += 25; continue; }
        char buf[256];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n <= 0) break;
        reply.append(buf, static_cast<size_t>(n));
        const size_t da = reply.find("\x1b[?");
        if (da != std::string::npos && reply.find('c', da) != std::string::npos) break;   // the DA reply closes the exchange
    }
    if (reply.find("_Gi=31;OK") != std::string::npos) return GfxProto::Kitty;
    const size_t da = reply.find("\x1b[?");
    if (da != std::string::npos) {
        const size_t end = reply.find('c', da);
        const std::string attrs = ";" + reply.substr(da + 3, end == std::string::npos ? std::string::npos : end - da - 3) + ";";
        if (attrs.find(";4;") != std::string::npos) return GfxProto::Sixel;
    }
    return env_kitty ? GfxProto::Kitty : env_sixel ? GfxProto::Sixel : GfxProto::None;
#else
    return env_kitty ? GfxProto::Kitty : env_sixel ? GfxProto::Sixel : GfxProto::None;
#endif
}

bool gfx_compressed() { return true; }

// The picture's cells in a text frame: U+E000 (private use, one column wide everywhere) as a placeholder, so overlays and
// width calculations treat them like any other cell.
std::string gfx_hole(int n) {
    std::string o;
    for (int i = 0; i < n; ++i) o += "\xEE\x80\x80";
    return o;
}

void gfx_fill_holes(std::string& s, bool skip) {
    static const std::string kHole = "\xEE\x80\x80";
    size_t at = s.find(kHole);
    if (at == std::string::npos) return;
    std::string o;
    o.reserve(s.size());
    size_t from = 0;
    while (at != std::string::npos) {
        o.append(s, from, at - from);
        int n = 0;
        while (s.compare(at, kHole.size(), kHole) == 0) { ++n; at += kHole.size(); }
        if (skip) { o += "\x1b["; o += std::to_string(n); o += 'C'; }   // move over the cells: the picture stays
        else o.append(static_cast<size_t>(n), ' ');
        from = at;
        at = s.find(kHole, from);
    }
    o.append(s, from, std::string::npos);
    s.swap(o);
}

int gfx_picture_cap(GfxProto proto, int scale) {
    if (proto == GfxProto::Kitty) return gfx_compressed() ? 60 : 20;
    return scale > 1 ? 60 : 30;   // Sixel: the encoder is fast; at full resolution the data per picture is what limits
}

} // namespace muisc
