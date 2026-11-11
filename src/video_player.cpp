#include "video_player.h"
#include "video_timing.h"
#include <atomic>

#include "console_log.h"
#include "path_utf8.h"
#include "string_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace muisc {
namespace {

constexpr size_t kQueueMin = 6;   // decoded frames kept ahead at the least (a wide picture in a small buffer)

const uint8_t kBayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

std::string fmt_seconds(double t) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f", std::max(0.0, t));
    return b;
}

inline uint32_t fx_hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
inline float fx_rand(uint32_t a, uint32_t b = 0) { return static_cast<float>(fx_hash(a * 0x9E3779B1U ^ fx_hash(b + 0x85EBCA6BU)) & 0xFFFFFF) / 16777216.0f; }
inline uint8_t fx_clamp(float v) { return static_cast<uint8_t>(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v + 0.5f); }

// One row shifted sideways by `dx` pixels (edge pixels repeat), with the red and blue channels split by `split` pixels.
void fx_shift_row(const uint8_t* src, uint8_t* dst, int w, int dx, int split) {
    for (int x = 0; x < w; ++x) {
        const int xr = std::clamp(x - dx - split, 0, w - 1), xg = std::clamp(x - dx, 0, w - 1), xb = std::clamp(x - dx + split, 0, w - 1);
        dst[3 * x] = src[3 * xr];
        dst[3 * x + 1] = src[3 * xg + 1];
        dst[3 * x + 2] = src[3 * xb + 2];
    }
}


inline float fx_smooth(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
inline float fx_luma(const uint8_t* p) { return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]; }
inline float fx_wave_y(const float* c) { return std::sqrt(0.299f * c[0] * c[0] + 0.587f * c[1] * c[1] + 0.114f * c[2] * c[2]); }
// The wave's colour made equally bright along the whole wave (the dimmest of the three gradient colours sets the level), so a light
// colour such as the purple does not light the picture up into bars: the wave changes the hue only, the picture's brightness stays.
inline float fx_wave_lo(const VideoFxGradient& g) {   // (the dimmest of the three gradient colours: the same for every pixel of a frame)
    float lo = 255.0f;
    for (int i = 0; i < 3; ++i) { const float v[3] = {static_cast<float>(g.c[i][0]), static_cast<float>(g.c[i][1]), static_cast<float>(g.c[i][2])}; lo = std::min(lo, fx_wave_y(v)); }
    return lo;
}
inline void fx_even_wave_lo(float lo, float* c) {
    const float y = fx_wave_y(c);
    if (y > lo && y > 1.0f) { const float k = std::max(lo, 80.0f) / y; if (k < 1.0f) for (int i = 0; i < 3; ++i) c[i] *= k; }
}
inline void fx_even_wave(const VideoFxGradient& g, float* c) { fx_even_wave_lo(fx_wave_lo(g), c); }
// The colour between two stops of the gradient (stop i0 -> i0 + 1, fr 0 .. 1) as 0 .. 255 floats. With a saturation below 100 (Video Gradient Saturation)
// the blended colour is moved towards the grey of the same brightness (luma) -- 0 = grey, the hue and the brightness of the wave stay -- from a table made
// once per frame; at 100 it is the plain blend, as it always was.
struct GradLut {
    bool on = false;
    float v[2][129][3];
    explicit GradLut(const VideoFxGradient& g) {
        on = g.sat < 100;
        if (!on) return;
        const float s = static_cast<float>(std::max(0, g.sat)) / 100.0f;
        for (int sg = 0; sg < 2; ++sg)
            for (int i = 0; i <= 128; ++i) {
                const float fr = static_cast<float>(i) / 128.0f;
                float c[3];
                for (int k = 0; k < 3; ++k) c[k] = g.c[sg][k] * (1.0f - fr) + g.c[sg + 1][k] * fr;
                const float y = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
                for (int k = 0; k < 3; ++k) v[sg][i][k] = std::clamp(y + (c[k] - y) * s, 0.0f, 255.0f);
            }
    }
    void at(const VideoFxGradient& g, int i0, float fr, float* o) const {
        if (!on) { for (int k = 0; k < 3; ++k) o[k] = g.c[i0][k] * (1.0f - fr) + g.c[i0 + 1][k] * fr; return; }
        const float f = std::clamp(fr, 0.0f, 1.0f) * 128.0f;
        const int k0 = std::min(127, static_cast<int>(f));
        const float a = f - static_cast<float>(k0);
        for (int k = 0; k < 3; ++k) o[k] = v[i0][k0][k] * (1.0f - a) + v[i0][k0 + 1][k] * a;
    }
};

// A picture made of f x f blocks, c channels of floats (for glows: they are blurred small and sampled smoothly, which is cheap and soft).
struct FxSmall {
    int w = 0, h = 0, c = 1, f = 1;
    std::vector<float> v;
};
template <class F>
FxSmall fx_small(int w, int h, int f, int c, F&& fn) {
    FxSmall s;
    s.f = std::max(1, f); s.c = c;
    s.w = (w + s.f - 1) / s.f; s.h = (h + s.f - 1) / s.f;
    s.v.assign(static_cast<size_t>(s.w) * s.h * c, 0.0f);
    std::vector<float> cnt(static_cast<size_t>(s.w) * s.h, 0.0f);
    float o[3] = {0, 0, 0};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            fn(x, y, o);
            const size_t i = static_cast<size_t>(y / s.f) * s.w + static_cast<size_t>(x / s.f);
            for (int k = 0; k < c; ++k) s.v[i * c + k] += o[k];
            cnt[i] += 1.0f;
        }
    for (size_t i = 0; i < cnt.size(); ++i)
        for (int k = 0; k < c; ++k) s.v[i * c + k] /= std::max(1.0f, cnt[i]);
    return s;
}
// A box blur of radius r, `passes` times, horizontally and vertically (edge pixels repeat).
void fx_blur(FxSmall& s, int r, int passes) {
    if (r < 1 || s.w < 2 || s.h < 2) return;
    std::vector<float> line(static_cast<size_t>(std::max(s.w, s.h)));
    for (int p = 0; p < passes; ++p)
        for (int k = 0; k < s.c; ++k)
            for (int dir = 0; dir < 2; ++dir) {
                const int len = dir == 0 ? s.w : s.h, lines = dir == 0 ? s.h : s.w;
                const size_t step = dir == 0 ? static_cast<size_t>(s.c) : static_cast<size_t>(s.w) * s.c;
                const size_t lstep = dir == 0 ? static_cast<size_t>(s.w) * s.c : static_cast<size_t>(s.c);
                for (int l = 0; l < lines; ++l) {
                    float* base = s.v.data() + l * lstep + k;
                    auto at = [&](int i) { return base[static_cast<size_t>(std::clamp(i, 0, len - 1)) * step]; };
                    float sum = 0.0f;
                    for (int i = -r; i <= r; ++i) sum += at(i);
                    for (int i = 0; i < len; ++i) {
                        line[static_cast<size_t>(i)] = sum / static_cast<float>(2 * r + 1);
                        sum += at(i + r + 1) - at(i - r);
                    }
                    for (int i = 0; i < len; ++i) base[static_cast<size_t>(i) * step] = line[static_cast<size_t>(i)];
                }
            }
}
// The pixel loops of the video looks are independent row by row (each reads the untouched copy of the frame and writes its own rows), so
// the rows are shared between a few threads. A small picture (or a one-core machine) just runs in the calling thread.
std::atomic<int> g_fx_threads{0};   // VideoPlayer::set_fx_threads()

template <class F>
void par_rows(int h, F&& fn) {
    // threads for the looks: half of the cores, at most 4 (the decoder, the audio and the screen need some too); MOUSIKI_FX_THREADS=1 turns it off
    static const int automatic = [] {
        if (const char* e = std::getenv("MOUSIKI_FX_THREADS")) { const int v = muisc::atoi_safe(e); if (v >= 1) return std::min(v, 16); }
        return std::clamp(static_cast<int>(std::max(1u, std::thread::hardware_concurrency())) / 2, 1, 4);
    }();
    const int forced = g_fx_threads.load(std::memory_order_relaxed);
    const int threads = forced > 0 ? forced : automatic;
    const int n = std::min(threads, h / 32);
    video_timing().threads = std::max(1, n);
    if (n <= 1) { fn(0, h); return; }
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(n));
    int done = h;   // the rows from `done` on are done by the threads started; the first rows by this thread
    for (int i = n - 1; i >= 1; --i) {
        const int a = h * i / n, b = done;
        try { pool.emplace_back([&fn, a, b] { fn(a, b); }); done = a; }
        catch (...) { break; }   // no thread to be had: this thread takes those rows too
    }
    fn(0, done);
    for (auto& t : pool) t.join();
}

// The value of channel k at the full-resolution pixel (x, y), smoothly (bilinear between the blocks).
inline float fx_sample(const FxSmall& s, int k, int x, int y) {
    const float fx = (static_cast<float>(x) + 0.5f) / s.f - 0.5f, fy = (static_cast<float>(y) + 0.5f) / s.f - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, s.w - 1), y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, s.h - 1);
    const int x1 = std::min(s.w - 1, x0 + 1), y1 = std::min(s.h - 1, y0 + 1);
    const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f), ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
    auto v = [&](int xx, int yy) { return s.v[(static_cast<size_t>(yy) * s.w + xx) * s.c + k]; };
    return (v(x0, y0) * (1 - tx) + v(x1, y0) * tx) * (1 - ty) + (v(x0, y1) * (1 - tx) + v(x1, y1) * tx) * ty;
}
// The three channels at once (the position arithmetic is done once; the result is the same as three fx_sample() calls).
inline void fx_sample3(const FxSmall& s, int x, int y, float* o) {
    const float fx = (static_cast<float>(x) + 0.5f) / s.f - 0.5f, fy = (static_cast<float>(y) + 0.5f) / s.f - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, s.w - 1), y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, s.h - 1);
    const int x1 = std::min(s.w - 1, x0 + 1), y1 = std::min(s.h - 1, y0 + 1);
    const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f), ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
    const float* a = &s.v[(static_cast<size_t>(y0) * s.w + x0) * s.c];
    const float* b = &s.v[(static_cast<size_t>(y0) * s.w + x1) * s.c];
    const float* c = &s.v[(static_cast<size_t>(y1) * s.w + x0) * s.c];
    const float* d = &s.v[(static_cast<size_t>(y1) * s.w + x1) * s.c];
    for (int k = 0; k < 3; ++k) o[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
}
// x^e for x = luma / 255, as a table over the integer 299 r + 587 g + 114 b (what the luma is made of): a pow() per pixel costs more than the rest of the look.
struct LumaPow {
    std::vector<float> t;
    explicit LumaPow(float e) : t(255001) { for (size_t i = 0; i < t.size(); ++i) t[i] = std::pow(static_cast<float>(i) / 255000.0f, e); }
    float at(const uint8_t* p) const { return t[static_cast<size_t>(299 * p[0] + 587 * p[1] + 114 * p[2])]; }
};
// 8 x 8 Bayer threshold (0..63) built from the 4 x 4 one
inline int fx_bayer8(int x, int y) {
    static const int q[2][2] = {{0, 2}, {3, 1}};
    return 4 * kBayer4[y & 3][x & 3] + q[(y >> 2) & 1][(x >> 2) & 1];
}
// A blue-noise threshold (0..1023) over a 32 x 32 tile that wraps around: the ranks of a "void filling" -- every next rank goes to the cell
// that is farthest from all the earlier ones (least Gaussian energy). Used instead of the Bayer matrix where a regular dither showed as a
// lattice of little squares over flat areas: the average tone is the same, but there is no repeating pattern to see.
inline int fx_blue32(int x, int y) {
    static const std::array<uint16_t, 1024> rank = [] {
        constexpr int N = 32, R = 5;
        std::array<float, N * N> energy{};
        std::array<bool, N * N> used{};
        std::array<float, (2 * R + 1) * (2 * R + 1)> kern{};
        for (int dy = -R; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx) kern[static_cast<size_t>((dy + R) * (2 * R + 1) + dx + R)] = std::exp(-static_cast<float>(dx * dx + dy * dy) / (2.0f * 1.5f * 1.5f));
        uint32_t seed = 2463534242u;   // (a tiny jitter breaks the ties of the first ranks, so they do not fall on a lattice; the table is the same on every run)
        for (float& e : energy) { seed = seed * 1664525u + 1013904223u; e = static_cast<float>(seed >> 8) * (1.0f / 16777216.0f) * 1e-4f; }
        std::array<uint16_t, N * N> out{};
        for (int r = 0; r < N * N; ++r) {
            int best = -1;
            for (int i = 0; i < N * N; ++i)
                if (!used[static_cast<size_t>(i)] && (best < 0 || energy[static_cast<size_t>(i)] < energy[static_cast<size_t>(best)])) best = i;
            used[static_cast<size_t>(best)] = true;
            out[static_cast<size_t>(best)] = static_cast<uint16_t>(r);
            const int bx = best % N, by = best / N;
            for (int dy = -R; dy <= R; ++dy)
                for (int dx = -R; dx <= R; ++dx)
                    energy[static_cast<size_t>(((by + dy + N) % N) * N + (bx + dx + N) % N)] += kern[static_cast<size_t>((dy + R) * (2 * R + 1) + dx + R)];
        }
        return out;
    }();
    return rank[static_cast<size_t>((y & 31) * 32 + (x & 31))];
}
// The threshold of the dither pattern `pat` (1 .. 6, see dither_name() in video_fx.h) at a pixel: a rank 0 .. 1023, the same scale for all of them
// (so the callers compare against one table). The patterns depend on the position only: a still picture stays still.
inline int fx_dither_rank(int pat, int x, int y) {
    switch (pat) {
        case 2: { static const uint8_t k[4] = {0, 2, 3, 1}; return k[(y & 1) * 2 + (x & 1)] * 256 + 128; }   // bayer 2x2 (the coarsest raster: only five tones per 2x2)
        case 3: return static_cast<int>(kBayer4[y & 3][x & 3]) * 64 + 32;                                   // bayer 4x4 (the regular raster)
        case 4: return fx_bayer8(x, y) * 16 + 8;                                                              // bayer 8x8
        case 5: {   // interleaved gradient noise (Jimenez): an organic, slightly diagonal grain
            float a = 0.06711056f * static_cast<float>(x) + 0.00583715f * static_cast<float>(y);
            a -= std::floor(a);
            float v = 52.9829189f * a;
            v -= std::floor(v);
            return std::min(1023, static_cast<int>(v * 1024.0f));
        }
        case 6: {   // golden ratio (the R2 sequence, Roberts): a low-discrepancy scatter, even like blue noise but with a fine diagonal weave
            float v = 0.75487766f * static_cast<float>(x) + 0.56984029f * static_cast<float>(y);
            v -= std::floor(v);
            return std::min(1023, static_cast<int>(v * 1024.0f));
        }
        default: return fx_blue32(x, y);                                                                       // blue noise
    }
}
inline int fx_pat(int pat, int dflt) { return pat > 0 && pat < kDitherPatterns ? pat : dflt; }   // 0 = auto: the look's own choice
const uint8_t kEga[16][3] = {{0, 0, 0},     {0, 0, 170},   {0, 170, 0},   {0, 170, 170}, {170, 0, 0},   {170, 0, 170}, {170, 85, 0}, {170, 170, 170},
                             {85, 85, 85},  {85, 85, 255}, {85, 255, 85}, {85, 255, 255}, {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255}};
constexpr int kCgaSteps = 8;   // CGA: mixing ratios 1/8 .. 7/8 between two palette colours
const uint8_t kCga[4][3] = {{0, 0, 0}, {85, 255, 255}, {255, 85, 255}, {255, 255, 255}};   // CGA palette 1, high intensity: black, cyan, magenta, white
// The C64's 16 colours (the "Pepto" measurement of a PAL VIC-II)
const uint8_t kC64[16][3] = {{0, 0, 0}, {255, 255, 255}, {104, 55, 43}, {112, 164, 178}, {111, 61, 134}, {88, 141, 67}, {53, 40, 121}, {184, 199, 111},
                             {111, 79, 37}, {67, 57, 0}, {154, 103, 89}, {68, 68, 68}, {108, 108, 108}, {154, 210, 132}, {108, 94, 181}, {149, 149, 149}};

// The base layer of the tube looks: a small tile of per-channel multipliers (the screen's structure) repeated over the picture. The tile's
// average is brought to about 0.9 so the picture keeps its brightness. kind: 1 aperture grille, 2 shadow mask, 3 slot mask, 4 lcd grid.
// `strength` 0..1 (VHS and the glitch use a lighter structure than the tube looks). Only for pictures that have the pixels to show it.
struct BaseTile {
    int tw = 1, th = 1;
    std::vector<float> m;   // th * tw * 3
};
BaseTile fx_base_tile(int kind, int mx, float strength) {
    BaseTile t;
    const float dim = 1.0f - 0.58f * strength;   // what an unlit stripe / the gap lets through
    auto at = [&](int x, int y, int c) -> float& { return t.m[(static_cast<size_t>(y) * t.tw + static_cast<size_t>(x)) * 3 + static_cast<size_t>(c)]; };
    if (kind == 1) {   // aperture grille: vertical stripes, red green blue
        t.tw = 3 * mx; t.th = 1;
        t.m.assign(static_cast<size_t>(t.tw) * 3, dim);
        for (int x = 0; x < t.tw; ++x) at(x, 0, (x / mx) % 3) = 1.0f;
    } else if (kind == 2) {   // shadow mask: round dots in a delta pattern (every second row is shifted by half a triad)
        const int cw = 3 * mx, rh = std::max(2, 2 * mx);
        t.tw = cw; t.th = 2 * rh;
        t.m.assign(static_cast<size_t>(t.tw) * t.th * 3, 0.0f);
        const float rad = 0.5f * static_cast<float>(mx) + 0.55f;
        for (int y = 0; y < t.th; ++y)
            for (int x = 0; x < t.tw; ++x)
                for (int c = 0; c < 3; ++c) {
                    const bool odd = (y / rh) & 1;
                    float cx = (static_cast<float>(c) + 0.5f) * static_cast<float>(mx) + (odd ? 1.5f * static_cast<float>(mx) : 0.0f);
                    float dx = std::fabs(static_cast<float>(x) + 0.5f - cx);
                    dx = std::min(dx, static_cast<float>(cw) - dx);
                    const float dy = (static_cast<float>(y % rh) + 0.5f - static_cast<float>(rh) * 0.5f) * 0.8f;
                    const float lit = std::clamp(1.35f - std::sqrt(dx * dx + dy * dy) / rad, 0.0f, 1.0f);
                    at(x, y, c) = dim * 0.55f + (1.0f - dim * 0.55f) * lit;
                }
    } else if (kind == 3) {   // slot mask: stripes with slots, the slots of neighbouring triads are staggered
        const int sh = 3 * mx, gap = mx;
        t.tw = 6 * mx; t.th = sh + gap;
        t.m.assign(static_cast<size_t>(t.tw) * t.th * 3, dim);
        for (int y = 0; y < t.th; ++y)
            for (int x = 0; x < t.tw; ++x) {
                const int tri = x / (3 * mx);
                const int yy = (y + (tri & 1 ? (sh + gap) / 2 : 0)) % (sh + gap);
                const bool slot_gap = yy >= sh;
                for (int c = 0; c < 3; ++c) at(x, y, c) = slot_gap ? dim * 0.45f : ((x / mx) % 3 == c ? 1.0f : dim);
            }
    } else {   // lcd grid: every pixel is three vertical sub-pixels with a dark line to its right and below
        t.tw = 4 * mx; t.th = 4 * mx;
        t.m.assign(static_cast<size_t>(t.tw) * t.th * 3, 1.0f);
        const float gapv = 1.0f - 0.78f * strength;
        for (int y = 0; y < t.th; ++y)
            for (int x = 0; x < t.tw; ++x) {
                const bool gx = x >= 3 * mx, gy = y >= 3 * mx;
                for (int c = 0; c < 3; ++c) at(x, y, c) = (gx || gy) ? gapv : ((x / mx) % 3 == c ? 1.0f : 1.0f - 0.45f * strength);
            }
    }
    double sum = 0.0;
    for (float v : t.m) sum += v;
    const float avg = static_cast<float>(sum / static_cast<double>(t.m.size()));
    const float gain = std::clamp(0.92f / std::max(0.05f, avg), 1.0f, 2.4f);
    for (float& v : t.m) v *= gain;
    return t;
}
void fx_apply_base(uint8_t* px, int w, int h, int kind, float strength) {
    if (kind < 1 || kind > 4 || w < 160) return;   // (a small picture has no pixels for a mask)
    const int mx = std::clamp((w + 240) / 480, 1, 4);
    const BaseTile tile = fx_base_tile(kind, mx, strength);
    par_rows(h, [&](int ry0, int ry1) {
        for (int y = ry0; y < ry1; ++y) {
            const float* row = &tile.m[static_cast<size_t>(y % tile.th) * tile.tw * 3];
            uint8_t* q = px + static_cast<size_t>(y) * static_cast<size_t>(w) * 3;
            int tx = 0;
            for (int x = 0; x < w; ++x, q += 3) {
                const float* m = row + static_cast<size_t>(tx) * 3;
                q[0] = fx_clamp(q[0] * m[0]);
                q[1] = fx_clamp(q[1] * m[1]);
                q[2] = fx_clamp(q[2] * m[2]);
                if (++tx == tile.tw) tx = 0;
            }
        }
    });
}

const uint8_t kGameBoy[8][3] = {{8, 32, 12}, {48, 98, 48}, {139, 172, 15}, {155, 188, 15}, {0, 0, 0}, {85, 85, 85}, {170, 170, 170}, {255, 255, 255}};   // 0..3 the greens, 4..7 the black and white version   // (the darkest green is darker than the handheld's: dark areas stay dark)

}  // namespace

void VideoPlayer::apply_fx(VideoFrame& f, int fx, double t, bool braille, const VideoFxGradient* grad, const VideoFrame* prev, int dither, int base) {
    const int w = f.w, h = f.h;
    if (fx <= 0 || w < 4 || h < 4 || f.rgb.size() < static_cast<size_t>(w) * h * 3) return;
    const size_t stride = static_cast<size_t>(w) * 3;
    uint8_t* px = f.rgb.data();
    const std::vector<uint8_t> src(f.rgb.begin(), f.rgb.begin() + static_cast<long>(stride * h));   // the untouched frame
    const uint32_t tick = static_cast<uint32_t>(std::max(0.0, t) * 25.0);   // one step per video frame
    // A scanline gap every `period` rows. A tall picture gets 3 rows per line, a small one 2 (braille: 4 dots per cell row -> 2). A very tall one
    // (a full screen picture on a big monitor: 900 pixels and more) 4 or 5, so the lines stay about the same number (about 250) and do not turn
    // into a fine hatching.
    const int period = braille || h < 300 ? 2 : h < 900 ? 3 : h < 1260 ? 4 : 5;
    // The Video Base Layer replaces the scanlines (and the CRT's RGB stripes) of the tube looks: with another layer than 0 they draw no lines.
    const bool own_lines = base == 0 || !video_fx_uses_base(fx);
    auto beam = [&](int y, float depth) {   // 1 on the bright row(s) of a line, 1 - depth in the gap, a soft ramp between (a raised cosine)
        if (!own_lines) return 1.0f;
        const float ph = static_cast<float>(y % period) / static_cast<float>(period);
        return 1.0f - depth * (0.5f - 0.5f * std::cos(ph * 6.2831853f));
    };
    auto soft_sample = [&](int x, int y, int c) {   // a little horizontal softness (the beam is not razor sharp)
        const uint8_t* r = &src[static_cast<size_t>(y) * stride];
        const int xl = std::max(0, x - 1), xr = std::min(w - 1, x + 1);
        return 0.25f * r[3 * xl + c] + 0.5f * r[3 * x + c] + 0.25f * r[3 * xr + c];
    };
    if (fx == 1) {   // scanlines: a beam that is narrower on bright lines, a glow around bright spots, a slow refresh bar, a faint flicker
        const float depth = braille ? 0.34f : 0.50f;
        // the bright pass of the picture, softened sideways: the glow (bloom) that bleeds into the dark gaps
        std::vector<float> glow(static_cast<size_t>(w) * h), lumv(static_cast<size_t>(w) * h);
        par_rows(h, [&](int ry0, int ry1) {
            for (int y = ry0; y < ry1; ++y) {
                const uint8_t* r = &src[static_cast<size_t>(y) * stride];
                float* lv = &lumv[static_cast<size_t>(y) * w];
                for (int x = 0; x < w; ++x) lv[x] = 0.299f * r[3 * x] + 0.587f * r[3 * x + 1] + 0.114f * r[3 * x + 2];   // (the luma of every pixel once, not five times)
                float* gl = &glow[static_cast<size_t>(y) * w];
                for (int x = 0; x < w; ++x) {
                    float acc = 0.0f;
                    for (int k = -2; k <= 2; ++k) {
                        const float lum = lv[std::clamp(x + k, 0, w - 1)];
                        acc += std::max(0.0f, lum - 150.0f) * (k == 0 ? 0.36f : std::abs(k) == 1 ? 0.24f : 0.08f);
                    }
                    gl[x] = acc;
                }
            }
        });
        const float bar_y = (1.0f - std::fmod(static_cast<float>(t) * 0.07f, 1.0f)) * h;   // the refresh bar climbs the picture
        const float bar_h = std::max(4.0f, h * 0.22f);
        const float flick = 1.0f + 0.010f * std::sin(static_cast<float>(t) * 31.0f);
        par_rows(h, [&](int ry0, int ry1) {
            for (int y = ry0; y < ry1; ++y) {
                const float ph = static_cast<float>(y % period) / static_cast<float>(period);
                const float gap = 0.5f - 0.5f * std::cos(ph * 6.2831853f);   // 0 on the beam row, 1 in the middle of the gap
                const float bd = std::fabs(static_cast<float>(y) - bar_y);
                const float bar = bd < bar_h ? 1.0f - 0.16f * (0.5f + 0.5f * std::cos(bd / bar_h * 3.14159265f)) : 1.0f;
                for (int x = 0; x < w; ++x) {
                    const uint8_t* q = &src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                    const float lum = (0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2]) / 255.0f;
                    const float d = depth * gap * (1.0f - 0.55f * lum);   // a bright line has a wider beam: its gap is less dark
                    const float g = glow[static_cast<size_t>(y) * w + x] * (0.55f + 0.45f * gap);   // the glow shows most in the gaps
                    for (int c = 0; c < 3; ++c) {
                        const float v = (q[c] * (1.0f - d) * (1.0f + depth * 0.22f * (1.0f - gap)) + g * (c == 2 ? 0.9f : 1.0f)) * bar * flick;
                        px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x) + static_cast<size_t>(c)] = fx_clamp(v);
                    }
                }
            }
        });
    } else if (fx == 2) {   // crt
        const float depth = braille ? 0.30f : 0.40f;
        const bool mask = !braille && w >= 160 && own_lines;   // the RGB stripes need pixels to be seen
        static const std::vector<float> kTube = [] { std::vector<float> t(4097); for (size_t i = 0; i < t.size(); ++i) t[i] = 255.0f * std::pow(static_cast<float>(i) / 4096.0f, 1.08f); return t; }();   // (the tube's gamma, a table with interpolation)
        const float flick = 1.0f + 0.012f * std::sin(static_cast<float>(t) * 47.0f) + 0.008f * (fx_rand(tick, 3) - 0.5f);
        // the vignette's two powers depend on x alone and on y alone: computed once per column / row instead of once per pixel
        std::vector<float> vx(static_cast<size_t>(w)), vy(static_cast<size_t>(h));
        for (int x = 0; x < w; ++x) vx[static_cast<size_t>(x)] = std::pow(std::fabs((static_cast<float>(x) + 0.5f) / w * 2.0f - 1.0f), 2.6f);
        for (int y = 0; y < h; ++y) vy[static_cast<size_t>(y)] = std::pow(std::fabs((static_cast<float>(y) + 0.5f) / h * 2.0f - 1.0f), 2.6f);
        par_rows(h, [&](int ry0, int ry1) {
            for (int y = ry0; y < ry1; ++y) {
                const float dy = (static_cast<float>(y) + 0.5f) / h * 2.0f - 1.0f;
                const float line = beam(y, depth) * (1.0f + depth * 0.30f);
                for (int x = 0; x < w; ++x) {
                    const float dx = (static_cast<float>(x) + 0.5f) / w * 2.0f - 1.0f;
                    const float vig = 1.0f - 0.34f * vx[static_cast<size_t>(x)] - 0.30f * vy[static_cast<size_t>(y)] - 0.10f * dx * dx * dy * dy;
                    for (int c = 0; c < 3; ++c) {
                        float v = braille ? static_cast<float>(src[static_cast<size_t>(y) * stride + 3 * x + c]) : soft_sample(x, y, c);
                        if (mask) v *= (x % 3 == c) ? 1.08f : 0.86f;   // the phosphor stripe of this colour is lit, the other two are dim
                        // a touch more contrast, like a tube's gamma
                        {
                            const float pos = std::clamp(v / 255.0f, 0.0f, 1.0f) * 4096.0f;
                            const int i = std::min(4095, static_cast<int>(pos));
                            v = kTube[static_cast<size_t>(i)] + (kTube[static_cast<size_t>(i) + 1] - kTube[static_cast<size_t>(i)]) * (pos - static_cast<float>(i));
                        }
                        px[static_cast<size_t>(y) * stride + 3 * x + c] = fx_clamp(v * line * std::max(0.35f, vig) * flick);
                    }
                }
            }
        });
    } else if (fx == 3) {   // vhs
        const int split = std::max(1, w / 180);
        const float band_y = std::fmod(static_cast<float>(t) * 0.11f, 1.0f) * h;   // the tracking band rolls up the picture
        const float band_h = std::max(3.0f, h * 0.07f);
        std::vector<uint8_t> row(stride);
        for (int y = 0; y < h; ++y) {
            const uint8_t* s = &src[static_cast<size_t>(y) * stride];
            float wob = 0.7f * std::sin(static_cast<float>(y) * 0.19f + static_cast<float>(t) * 2.3f) + 0.5f * std::sin(static_cast<float>(y) * 0.043f - static_cast<float>(t) * 1.1f);
            wob *= std::max(0.6f, w / 320.0f);
            float noise_amp = 5.0f;
            float dist = std::fabs(static_cast<float>(y) - band_y);
            if (dist < band_h) {   // inside the band: a bigger slip and more noise
                const float k = 1.0f - dist / band_h;
                wob += (fx_rand(tick, static_cast<uint32_t>(y)) - 0.35f) * 0.035f * w * k;
                noise_amp += 26.0f * k;
            }
            if (y > h - std::max(2, h / 24)) {   // head-switching noise: the last lines tear to the side
                wob += (fx_rand(static_cast<uint32_t>(y), 11) - 0.2f) * 0.045f * w;
                noise_amp += 14.0f;
            }
            fx_shift_row(s, row.data(), w, static_cast<int>(std::lround(wob)), split);
            const float line = beam(y, braille ? 0.18f : 0.22f);
            for (int x = 0; x < w; ++x) {
                const float n = (fx_rand(tick * 7919U + static_cast<uint32_t>(x), static_cast<uint32_t>(y)) - 0.5f) * 2.0f * noise_amp;
                const float r = row[3 * static_cast<size_t>(x)], g = row[3 * static_cast<size_t>(x) + 1], b = row[3 * static_cast<size_t>(x) + 2];
                const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
                const float sat = 0.82f;   // paler colours
                px[static_cast<size_t>(y) * stride + 3 * x] = fx_clamp((lum + (r - lum) * sat + n) * line);
                px[static_cast<size_t>(y) * stride + 3 * x + 1] = fx_clamp((lum + (g - lum) * sat + n) * line);
                px[static_cast<size_t>(y) * stride + 3 * x + 2] = fx_clamp((lum + (b - lum) * sat + n) * line);
            }
        }
    } else if (fx == 5) {   // pixel: a coarse mosaic with a reduced palette (a small console's screen)
        const int bs = braille ? 2 : std::clamp(h / 100, 2, 6);   // block edge in pixels (about 100 blocks over the height)
        for (int by = 0; by < h; by += bs)
            for (int bx = 0; bx < w; bx += bs) {
                const int x1 = std::min(w, bx + bs), y1 = std::min(h, by + bs);
                float sum[3] = {0, 0, 0};
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x)
                        for (int c = 0; c < 3; ++c) sum[c] += src[static_cast<size_t>(y) * stride + 3 * x + c];
                const float n = static_cast<float>((x1 - bx) * (y1 - by));
                const float d = (kBayer4[(by / bs) & 3][(bx / bs) & 3] / 15.0f - 0.5f) * (braille ? 0.0f : 14.0f);   // a faint ordered dither per block
                for (int c = 0; c < 3; ++c) {
                    const float v = std::floor((sum[c] / n + d) / 255.0f * 15.0f + 0.5f) / 15.0f * 255.0f;   // 16 levels per colour
                    const uint8_t o = fx_clamp(v);
                    for (int y = by; y < y1; ++y)
                        for (int x = bx; x < x1; ++x) px[static_cast<size_t>(y) * stride + 3 * x + c] = o;
                }
            }
    } else if (fx == 6) {   // gradient: a slow diagonal wave in the gradient colours, from the upper left to the lower right
        const VideoFxGradient g = grad ? *grad : VideoFxGradient();
        const GradLut gl(g);
        const float shift = static_cast<float>(t) * 0.10f;
        const float wave_lo = fx_wave_lo(g);   // (the same for every pixel of the frame)
        static const LumaPow kShade(1.15f);
        par_rows(h, [&](int ry0, int ry1) {
            for (int y = ry0; y < ry1; ++y)
                for (int x = 0; x < w; ++x) {
                    const float p = (static_cast<float>(x) / w + static_cast<float>(y) / h) * 0.5f;   // 0 upper left .. 1 lower right
                    float u = p * 1.25f - shift; u -= std::trunc(u);   // (the remainder of a division by 1: exact, and much cheaper than fmod)   // the wave travels towards the lower right
                    if (u < 0.0f) u += 1.0f;
                    const float k = 1.0f - std::fabs(2.0f * u - 1.0f);   // there and back: left colour - centre - right colour - centre ...
                    const float pos = k * 2.0f;
                    const int i0 = std::min(1, static_cast<int>(pos));
                    const float fr = pos - static_cast<float>(i0);
                    const uint8_t* s = &src[static_cast<size_t>(y) * stride + 3 * x];
                    const float lum = (0.299f * s[0] + 0.587f * s[1] + 0.114f * s[2]) / 255.0f;
                    // The wave's colour only goes through the light parts: the shade follows the brightness, a dark area stays dark (no grey
                    // wash of the original is mixed in), so the dark parts of the picture do not turn into big coloured bands.
                    const float shade = braille ? 1.10f * lum : 1.12f * kShade.at(s);
                    float wcol[3];
                    gl.at(g, i0, fr, wcol);
                    fx_even_wave_lo(wave_lo, wcol);
                    for (int c = 0; c < 3; ++c) px[static_cast<size_t>(y) * stride + 3 * x + c] = fx_clamp(wcol[c] * shade);
                }
        });
    } else if (fx == 8 || fx == 9 || fx == 26) {   // phosphor: a monochrome tube (green or amber); 26 adds the glow of a long-persistence tube (halation, a longer tail)
        const bool amber = fx == 9, glowy = fx == 26;
        // the colour of the phosphor at full drive: P1 / P31 green, P3 amber (the blue of the glow is the tube's own, not white)
        const float tr = amber ? 255.0f : 51.0f, tg = amber ? 176.0f : 255.0f, tb = amber ? 0.0f : 51.0f;
        const int f = braille ? 1 : std::max(1, w / 200);
        const int sc = std::max(1, w / 320);   // one "pixel" of a 320 wide picture, in pixels
        FxSmall glow = fx_small(w, h, f, 1, [&](int x, int y, float* o) { o[0] = std::max(0.0f, fx_luma(&src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)]) / 255.0f - 0.35f) / 0.65f; });
        fx_blur(glow, braille ? 1 : 2, 2);
        FxSmall halo;   // the glass and the phosphor layer scatter light far around bright parts (halation)
        if (glowy && !braille) {
            halo = fx_small(w, h, std::max(2, w / 80), 1, [&](int x, int y, float* o) { o[0] = std::max(0.0f, fx_luma(&src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)]) / 255.0f - 0.22f) / 0.78f; });
            fx_blur(halo, 2, 3);
        }
        const float flick = 1.0f + 0.010f * std::sin(static_cast<float>(t) * 53.0f) + 0.006f * (fx_rand(tick, 5) - 0.5f);
        const float vpow = glowy ? 2.2f : 2.4f, vgx = glowy ? 0.24f : 0.26f, vgy = glowy ? 0.20f : 0.22f;
        std::vector<float> vx(static_cast<size_t>(w)), vy(static_cast<size_t>(h));   // the vignette's powers, per column / row
        for (int x = 0; x < w; ++x) vx[static_cast<size_t>(x)] = std::pow(std::fabs((static_cast<float>(x) + 0.5f) / w * 2.0f - 1.0f), vpow);
        for (int y = 0; y < h; ++y) vy[static_cast<size_t>(y)] = std::pow(std::fabs((static_cast<float>(y) + 0.5f) / h * 2.0f - 1.0f), vpow);
        static const LumaPow kPhos(1.12f);
        // The beam draws left to right and the phosphor fades slowly: every bright pixel leaves a short tail to its right (an exponential
        // decay along the line; the glowy tube has the longer one). Computed per row, so it stays deterministic.
        const float tail = braille ? 0.0f : glowy ? std::pow(0.62f, 1.0f / static_cast<float>(sc)) : std::pow(0.30f, 1.0f / static_cast<float>(sc));
        const float tail_k = glowy ? 0.62f : 0.40f;
        const float depth = braille ? 0.22f : 0.36f;
        par_rows(h, [&](int ry0, int ry1) {
            std::vector<float> lum(static_cast<size_t>(w));
            for (int y = ry0; y < ry1; ++y) {
                const float ph = static_cast<float>(y % period) / static_cast<float>(period);
                const float gap = own_lines ? 0.5f - 0.5f * std::cos(ph * 6.2831853f) : 0.0f;   // 0 on the beam row, 1 in the middle of the gap
                float run = 0.0f;
                for (int x = 0; x < w; ++x) {
                    const float l0 = kPhos.at(&src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)]);
                    run = std::max(l0, run * tail);
                    lum[static_cast<size_t>(x)] = tail > 0.0f ? l0 + (run - l0) * tail_k : l0;
                }
                for (int x = 0; x < w; ++x) {
                    const float vig = 1.0f - vgx * vx[static_cast<size_t>(x)] - vgy * vy[static_cast<size_t>(y)];
                    float v = lum[static_cast<size_t>(x)] * 0.92f + fx_sample(glow, 0, x, y) * (glowy ? 0.62f : 0.55f);
                    if (glowy && !braille) v += fx_sample(halo, 0, x, y) * 0.42f;
                    // the beam of a bright line is wider: its gap is less dark (as on a real tube), a dark line has a thin sharp beam
                    v *= (1.0f - depth * gap * (1.0f - 0.6f * std::min(1.0f, v))) * std::max(0.4f, vig) * flick;
                    if (glowy && !braille) v += (fx_rand(tick * 7919U + static_cast<uint32_t>(x / sc), static_cast<uint32_t>(y / sc)) - 0.5f) * 0.035f;   // the raster's faint noise
                    const float hot = std::min(1.0f, std::max(0.0f, v - 0.85f) * 1.6f);   // the hottest spots burn towards white
                    uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                    const float k = std::clamp(v, 0.0f, 1.0f);
                    // the black of a tube is not black: the raster glows very faintly in the phosphor's own colour
                    q[0] = fx_clamp(tr * (0.045f + k) + 255.0f * hot * 0.45f);
                    q[1] = fx_clamp(tg * (0.045f + k) + 255.0f * hot * 0.45f);
                    q[2] = fx_clamp(tb * (0.045f + k) + 255.0f * hot * 0.45f);
                }
            }
        });
    } else if (fx == 10) {   // halftone: newsprint, ink dots on a 45 degree grid whose size follows the darkness
        const float cs = braille ? 3.0f : std::max(3.5f, static_cast<float>(h) / 64.0f);
        const float ca = 0.70710678f, sa = 0.70710678f, aa = 0.9f / cs;
        auto sample = [&](int x, int y, float* o) {   // 3 x 3 average at a pixel
            float a[3] = {0, 0, 0};
            int n = 0;
            for (int yy = std::max(0, y - 1); yy <= std::min(h - 1, y + 1); ++yy)
                for (int xx = std::max(0, x - 1); xx <= std::min(w - 1, x + 1); ++xx) {
                    const uint8_t* s = &src[static_cast<size_t>(yy) * stride + 3 * static_cast<size_t>(xx)];
                    a[0] += s[0]; a[1] += s[1]; a[2] += s[2]; ++n;
                }
            for (int k = 0; k < 3; ++k) o[k] = a[k] / static_cast<float>(n);
        };
        par_rows(h, [&](int ry0, int ry1) {
            for (int y = ry0; y < ry1; ++y) {
                // everything but the distance to the dot's centre belongs to the dot (its cell): worked out once per dot, not per pixel
                float cu_last = 1e30f, cv_last = 1e30f, c[3] = {0, 0, 0}, r = 0.0f;
                for (int x = 0; x < w; ++x) {
                    const float u = (static_cast<float>(x) * ca + static_cast<float>(y) * sa) / cs, v = (-static_cast<float>(x) * sa + static_cast<float>(y) * ca) / cs;
                    const float cu = std::floor(u) + 0.5f, cv = std::floor(v) + 0.5f;
                    const float du = u - cu, dv = v - cv, d = std::sqrt(du * du + dv * dv);
                    if (cu != cu_last || cv != cv_last) {
                        cu_last = cu; cv_last = cv;
                        const int xc = std::clamp(static_cast<int>((cu * ca - cv * sa) * cs), 0, w - 1), yc = std::clamp(static_cast<int>((cu * sa + cv * ca) * cs), 0, h - 1);
                        sample(xc, yc, c);
                        const float lum = (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]) / 255.0f;
                        const float cov = std::clamp(1.0f - std::pow(lum, 0.95f), 0.0f, 1.0f);
                        r = std::sqrt(cov) * 0.72f;
                    }
                    const float ink = 1.0f - fx_smooth(r - aa, r + aa, d);
                    uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                    for (int k = 0; k < 3; ++k) {
                        const float paper = (k == 0 ? 243.0f : k == 1 ? 238.0f : 224.0f) * 0.84f + c[k] * 0.16f;   // warm paper with a hint of the picture
                        q[k] = fx_clamp(paper * (1.0f - ink) + c[k] * 0.50f * ink);
                    }
                }
            }
        });
    } else if (fx == 21 || fx == 22) {   // dither: a one-bit picture, ordered (Bayer 8x8 unless another pattern is chosen) dots in two inks (21: plain black and white, 22: black and the gradient wave); 11 and 14 (Bloom) are no longer offered and do nothing

        const VideoFxGradient dg = grad ? *grad : VideoFxGradient();
        const GradLut dgl(dg);
        const int bs = (!braille && h >= 240) ? 2 : 1;
        // Local contrast: a detailed picture came out as a few big black and white areas, because one global curve decides every dot. The
        // difference to the brightness around the dot (a box of about 1/40 of the width) is pushed up, so fine detail survives in the
        // dark and in the light parts; where it is nearly black around, nothing is pushed (no noise in the black, dark areas stay dark).
        std::vector<float> lm(static_cast<size_t>(w) * h);   // the mean brightness around every pixel (an integral image)
        {
            std::vector<double> ii(static_cast<size_t>(w + 1) * (h + 1), 0.0);
            for (int y = 0; y < h; ++y) {
                double row = 0.0;
                for (int x = 0; x < w; ++x) {
                    row += fx_luma(&src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)]) / 255.0;
                    ii[static_cast<size_t>(y + 1) * (w + 1) + x + 1] = ii[static_cast<size_t>(y) * (w + 1) + x + 1] + row;
                }
            }
            const int rad = std::max(3, w / 40);
            par_rows(h, [&](int ry0, int ry1) {
                for (int y = ry0; y < ry1; ++y)
                    for (int x = 0; x < w; ++x) {
                        const int x0 = std::max(0, x - rad), x1 = std::min(w, x + rad + 1), y0 = std::max(0, y - rad), y1 = std::min(h, y + rad + 1);
                        const double sum = ii[static_cast<size_t>(y1) * (w + 1) + x1] - ii[static_cast<size_t>(y0) * (w + 1) + x1] - ii[static_cast<size_t>(y1) * (w + 1) + x0] + ii[static_cast<size_t>(y0) * (w + 1) + x0];
                        lm[static_cast<size_t>(y) * w + x] = static_cast<float>(sum / (static_cast<double>(x1 - x0) * (y1 - y0)));
                    }
            });
        }
        static const std::array<float, 1024> thr_on = [] {   // pow(v, 1.2) > thr  <=>  v > thr^(1/1.2): the power is taken of the 1024 thresholds, not of every pixel
            std::array<float, 1024> t{};
            for (int i = 0; i < 1024; ++i) t[static_cast<size_t>(i)] = std::pow((static_cast<float>(i) + 0.5f) / 1024.0f, 1.0f / 1.2f);
            return t;
        }();
        const int pat = fx_pat(dither, 4);   // (auto: the ordered Bayer 8x8 dots this look always had)
        const float dwave_lo = fx_wave_lo(dg);
        par_rows(h, [&](int ry0, int ry1) {
            for (int y = ry0; y < ry1; ++y)
                for (int x = 0; x < w; ++x) {
                    const int bx = x / bs, by = y / bs;
                    const int sx = std::min(w - 1, bx * bs + bs / 2), sy = std::min(h - 1, by * bs + bs / 2);
                    float l = fx_luma(&src[static_cast<size_t>(sy) * stride + 3 * static_cast<size_t>(sx)]) / 255.0f;
                    const float mean = lm[static_cast<size_t>(sy) * w + sx];
                    l += 1.1f * (l - mean) * std::clamp(mean / 0.18f, 0.0f, 1.0f);
                    const bool on = std::clamp((l - 0.5f) * 1.12f + 0.5f, 0.0f, 1.0f) > thr_on[static_cast<size_t>(fx_dither_rank(pat, bx, by))];
                    uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                    if (fx == 22) {   // the light dots take the colour of the wave there, the dark ones stay black
                        float wc[3] = {0, 0, 0};
                        if (on) {
                            const float p = (static_cast<float>(sx) / w + static_cast<float>(sy) / h) * 0.5f;
                            float u = p * 1.25f - static_cast<float>(t) * 0.10f; u -= std::trunc(u);   // (the remainder of a division by 1: exact, and much cheaper than fmod)
                            if (u < 0.0f) u += 1.0f;
                            const float pos = (1.0f - std::fabs(2.0f * u - 1.0f)) * 2.0f;
                            const int i0 = std::min(1, static_cast<int>(pos));
                            const float fr = pos - static_cast<float>(i0);
                            dgl.at(dg, i0, fr, wc);
                            fx_even_wave_lo(dwave_lo, wc);
                        }
                        for (int k = 0; k < 3; ++k) q[k] = fx_clamp(wc[k]);
                    } else q[0] = q[1] = q[2] = on ? 255 : 0;
                }
        });
    } else if (fx == 12 || fx == 18 || fx == 20 || fx == 23 || fx == 24 || fx == 25) {   // game boy (20, 25: the four shades are the gradient wave's colour in four brightnesses): four greens, in a coarse grid with a light dither (18, 24, 25: low-res, big blocks)
        const bool low = fx == 18 || fx == 24 || fx == 25;
        const bool waved = fx == 20 || fx == 25;
        const int pal0 = (fx == 23 || fx == 24) ? 4 : 0;   // (the black and white version: the shades 4 .. 7 of kGameBoy)
        const VideoFxGradient gg = grad ? *grad : VideoFxGradient();
        const GradLut ggl(gg);
        const float wave_lo = fx_wave_lo(gg);
        static const float kShadeK[4] = {0.0f, 0.42f, 0.72f, 1.0f};   // (the darkest shade is black: the wave only runs through the light shades)
        auto wave_at = [&](int x, int y, float* o) {
            const float p = (static_cast<float>(x) / w + static_cast<float>(y) / h) * 0.5f;
            float u = p * 1.25f - static_cast<float>(t) * 0.10f; u -= std::trunc(u);   // (the remainder of a division by 1: exact, and much cheaper than fmod)
            if (u < 0.0f) u += 1.0f;
            const float pos = (1.0f - std::fabs(2.0f * u - 1.0f)) * 2.0f;
            const int i0 = std::min(1, static_cast<int>(pos));
            const float fr = pos - static_cast<float>(i0);
            ggl.at(gg, i0, fr, o);
            fx_even_wave_lo(wave_lo, o);
        };
        const int bs = low ? (braille ? 2 : std::max(3, static_cast<int>(std::lround(w / 160.0))))
                           : (braille ? 1 : std::max(2, static_cast<int>(std::lround(w / 320.0))));
        const int block_rows = (h + bs - 1) / bs;
        const int gpat = fx_pat(dither, 1);
        par_rows(block_rows, [&](int br0, int br1) {   // (the blocks are independent: each reads the untouched copy and writes its own pixels)
            for (int br = br0; br < br1; ++br) {
                const int by = br * bs;
                for (int bx = 0; bx < w; bx += bs) {
                    const int x1 = std::min(w, bx + bs), y1 = std::min(h, by + bs);
                    float sum = 0.0f;
                    for (int y = by; y < y1; ++y)
                        for (int x = bx; x < x1; ++x) sum += fx_luma(&src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)]);
                    float l = sum / (static_cast<float>((x1 - bx) * (y1 - by)) * 255.0f);
                    l = std::clamp((l - 0.5f) * 1.18f + 0.5f, 0.0f, 1.0f);   // a little more contrast: four shades are few
                    l = std::pow(l, 1.35f);   // and the dark half goes to the dark shades: shadows are not made lighter
                    const float d = ((static_cast<float>(fx_dither_rank(gpat, bx / bs, by / bs)) + 0.5f) / 1024.0f - 0.5f) * 0.22f;   // (auto = blue noise, not Bayer: no lattice of squares in flat areas)
                    const int idx = std::clamp(static_cast<int>(std::floor((l + d) * 4.0f)), 0, 3);
                    float wc[3] = {0, 0, 0};
                    if (waved) wave_at(std::min(w - 1, bx + bs / 2), std::min(h - 1, by + bs / 2), wc);
                    uint8_t col[3];   // (the colour of the block: worked out once, not for every pixel of it)
                    for (int k = 0; k < 3; ++k) col[k] = fx_clamp(waved ? wc[k] * kShadeK[idx] : kGameBoy[pal0 + idx][k]);
                    for (int y = by; y < y1; ++y) {
                        uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(bx)];
                        for (int x = bx; x < x1; ++x, q += 3) { q[0] = col[0]; q[1] = col[1]; q[2] = col[2]; }
                    }
                }
            }
        });
    } else if (fx == 27) {   // cga: four colours (cyan, magenta, white, black) -- every colour is MIXED from two of them in a pattern, as the games did
        // (the nearest of the four alone makes nearly everything magenta or cyan: a mix of two palette colours in a dither pattern gives every
        // brightness and most hues, and the picture stays a picture)
        struct Mix { float c[3]; uint8_t a, b, k; };
        static const std::vector<Mix> mixes = [] {
            std::vector<Mix> m;
            for (int i = 0; i < 4; ++i) { Mix x; for (int c = 0; c < 3; ++c) x.c[c] = kCga[i][c]; x.a = x.b = static_cast<uint8_t>(i); x.k = 0; m.push_back(x); }
            for (int i = 0; i < 4; ++i)
                for (int j = i + 1; j < 4; ++j)
                    for (int k = 1; k < kCgaSteps; ++k) {
                        Mix x;
                        for (int c = 0; c < 3; ++c) x.c[c] = (kCga[i][c] * static_cast<float>(kCgaSteps - k) + kCga[j][c] * static_cast<float>(k)) / static_cast<float>(kCgaSteps);
                        x.a = static_cast<uint8_t>(i); x.b = static_cast<uint8_t>(j); x.k = static_cast<uint8_t>(k);
                        m.push_back(x);
                    }
            return m;
        }();
        const int bs = braille ? 1 : std::clamp(h / 200, 1, 4);   // (a 320 x 200 screen)
        const int epat = fx_pat(dither, 4);
        const int bw = (w + bs - 1) / bs, bh = (h + bs - 1) / bs;
        par_rows(bh, [&](int r0, int r1) {
            for (int byb = r0; byb < r1; ++byb) {
                const int by = byb * bs;
                for (int bxb = 0; bxb < bw; ++bxb) {
                    const int bx = bxb * bs;
                    const int x1 = std::min(w, bx + bs), y1 = std::min(h, by + bs);
                    float sum[3] = {0, 0, 0};
                    for (int y = by; y < y1; ++y)
                        for (int x = bx; x < x1; ++x)
                            for (int k = 0; k < 3; ++k) sum[k] += src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x) + k];
                    const float n = static_cast<float>((x1 - bx) * (y1 - by));
                    const float r = sum[0] / n, g = sum[1] / n, b = sum[2] / n;
                    size_t best = 0;
                    float bd = 1e30f;
                    for (size_t i = 0; i < mixes.size(); ++i) {
                        const float dr = r - mixes[i].c[0], dg = g - mixes[i].c[1], db = b - mixes[i].c[2];
                        const float dd = 0.30f * dr * dr + 0.59f * dg * dg + 0.11f * db * db;
                        if (dd < bd) { bd = dd; best = i; }
                    }
                    const Mix& m = mixes[best];
                    const float rank = (static_cast<float>(fx_dither_rank(epat, bxb, byb)) + 0.5f) / 1024.0f;   // 0..1: where this cell sits in the pattern
                    const uint8_t pick = m.k == 0 || rank >= static_cast<float>(m.k) / static_cast<float>(kCgaSteps) ? m.a : m.b;
                    for (int y = by; y < y1; ++y)
                        for (int x = bx; x < x1; ++x) {
                            uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                            q[0] = kCga[pick][0]; q[1] = kCga[pick][1]; q[2] = kCga[pick][2];
                        }
                }
            }
        });
    } else if (fx == 13 || fx == 28) {   // ega 16: the 16 colours of the old PC graphics, ordered dither (28: the C64's 16)
        const uint8_t (*pal)[3] = fx == 28 ? kC64 : kEga;
        const int pal_n = 16;
        const float pbias = 80.0f;
        const int bs = fx == 28 ? (braille ? 1 : std::clamp(h / 160, 2, 5)) : (!braille && h >= 300) ? 2 : 1;   // (the C64's wide pixels: about 160 x 100 blocks)
        const int epat = fx_pat(dither, 4);
        for (int by = 0; by < h; by += bs)
            for (int bx = 0; bx < w; bx += bs) {
                const int x1 = std::min(w, bx + bs), y1 = std::min(h, by + bs);
                float sum[3] = {0, 0, 0};
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x)
                        for (int k = 0; k < 3; ++k) sum[k] += src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x) + k];
                const float n = static_cast<float>((x1 - bx) * (y1 - by));
                const float bias = ((static_cast<float>(fx_dither_rank(epat, bx / bs, by / bs)) + 0.5f) / 1024.0f - 0.5f) * pbias;
                const float r = sum[0] / n + bias, g = sum[1] / n + bias, b = sum[2] / n + bias;
                int best = 0;
                float bd = 1e30f;
                for (int i = 0; i < pal_n; ++i) {
                    const float dr = r - pal[i][0], dg = g - pal[i][1], db = b - pal[i][2];
                    const float dd = 0.30f * dr * dr + 0.59f * dg * dg + 0.11f * db * db;   // (the eye is most sensitive to green)
                    if (dd < bd) { bd = dd; best = i; }
                }
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x) {
                        uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                        q[0] = pal[best][0]; q[1] = pal[best][1]; q[2] = pal[best][2];
                    }
            }
    } else if (fx == 15) {   // film: grain, a faint sepia tone, a flickering gate, a vignette, now and then a scratch and some dust
        const float flick = 1.0f + (fx_rand(tick, 31) - 0.5f) * 0.07f + 0.012f * std::sin(static_cast<float>(t) * 9.0f);
        const int grain = braille ? 1 : std::max(1, w / 320);
        const float amp = braille ? 7.0f : 15.0f;
        const int weave = (fx_rand(tick, 51) < 0.12f) ? ((fx_rand(tick, 52) < 0.5f) ? -1 : 1) * std::max(1, h / 360) : 0;   // the picture jumps a little in the gate now and then
        const bool scratch = fx_rand(tick / 3, 41) < 0.30f;
        const int sx = static_cast<int>(fx_rand(tick / 3, 42) * w);
        std::vector<float> vx(static_cast<size_t>(w)), vy(static_cast<size_t>(h));   // the vignette's powers, per column / row
        for (int x = 0; x < w; ++x) vx[static_cast<size_t>(x)] = std::pow(std::fabs((static_cast<float>(x) + 0.5f) / w * 2.0f - 1.0f), 2.4f);
        for (int y = 0; y < h; ++y) vy[static_cast<size_t>(y)] = std::pow(std::fabs((static_cast<float>(y) + 0.5f) / h * 2.0f - 1.0f), 2.4f);
        par_rows(h, [&](int ry0, int ry1) {
            for (int y = ry0; y < ry1; ++y) {
                const int ys = std::clamp(y + weave, 0, h - 1);
                for (int x = 0; x < w; ++x) {
                    const float vig = 1.0f - 0.30f * vx[static_cast<size_t>(x)] - 0.26f * vy[static_cast<size_t>(y)];
                    const uint8_t* s = &src[static_cast<size_t>(ys) * stride + 3 * static_cast<size_t>(x)];
                    const float l = fx_luma(s);
                    const float sep[3] = {std::min(255.0f, l * 1.08f + 10.0f), l * 0.95f + 4.0f, l * 0.76f};
                    const float n = (fx_rand(tick * 7919U + static_cast<uint32_t>(x / grain), static_cast<uint32_t>(y / grain)) - 0.5f) * 2.0f * amp * (0.55f + 0.9f * (1.0f - l / 255.0f));
                    uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                    for (int k = 0; k < 3; ++k) q[k] = fx_clamp((s[k] * 0.68f + sep[k] * 0.32f) * flick * std::max(0.45f, vig) + n);
                }
            }
        });
        if (scratch) {   // a thin, bright, slightly wavering vertical line
            for (int y = 0; y < h; ++y) {
                if (fx_rand(static_cast<uint32_t>(y / 6), 43 + tick / 3) < 0.25f) continue;
                const int x = std::clamp(sx + static_cast<int>(std::sin(y * 0.05f + static_cast<float>(tick)) * 1.5f), 0, w - 1);
                uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                for (int k = 0; k < 3; ++k) q[k] = fx_clamp(q[k] * 0.6f + 255.0f * 0.4f);
            }
        }
        const int dust = 2 + static_cast<int>(fx_rand(tick, 44) * 6.0f);   // specks
        for (int k = 0; k < dust; ++k) {
            const uint32_t sd = tick * 241U + static_cast<uint32_t>(k);
            const int x = static_cast<int>(fx_rand(sd, 45) * w), y = static_cast<int>(fx_rand(sd, 46) * h), r = 1 + static_cast<int>(fx_rand(sd, 47) * std::max(1, w / 320));
            const bool dark = fx_rand(sd, 48) < 0.6f;
            for (int yy = std::max(0, y - r + 1); yy < std::min(h, y + r); ++yy)
                for (int xx = std::max(0, x - r + 1); xx < std::min(w, x + r); ++xx) {
                    uint8_t* q = &px[static_cast<size_t>(yy) * stride + 3 * static_cast<size_t>(xx)];
                    for (int c = 0; c < 3; ++c) q[c] = dark ? fx_clamp(q[c] * 0.35f) : fx_clamp(q[c] * 0.5f + 128.0f);
                }
        }
    } else if (fx == 16 || fx == 19) {   // neon edge: only the outlines, in the picture's own colours, glowing on black (19: the grey picture, outlines in the gradient wave)
        const bool wavy = fx == 19;
        const VideoFxGradient g = grad ? *grad : VideoFxGradient();
        const GradLut gl(g);
        auto wave = [&](int x, int y, float* o) {   // the wave of the gradient look (6) at this point
            const float p = (static_cast<float>(x) / w + static_cast<float>(y) / h) * 0.5f;
            float u = p * 1.25f - static_cast<float>(t) * 0.10f; u -= std::trunc(u);   // (the remainder of a division by 1: exact, and much cheaper than fmod)
            if (u < 0.0f) u += 1.0f;
            const float pos = (1.0f - std::fabs(2.0f * u - 1.0f)) * 2.0f;
            const int i0 = std::min(1, static_cast<int>(pos));
            const float fr = pos - static_cast<float>(i0);
            gl.at(g, i0, fr, o);
            for (int k = 0; k < 3; ++k) o[k] /= 255.0f;
        };
        std::vector<float> lum(static_cast<size_t>(w) * h);
        par_rows(h, [&](int ry0, int ry1) {
            for (int y = ry0; y < ry1; ++y)
                for (int x = 0; x < w; ++x) lum[static_cast<size_t>(y) * w + x] = fx_luma(&src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)]);
        });
        auto L = [&](int x, int y) { return lum[static_cast<size_t>(std::clamp(y, 0, h - 1)) * w + static_cast<size_t>(std::clamp(x, 0, w - 1))]; };
        const int rr = (!braille && w >= 480) ? 2 : 1;   // the Sobel reach grows with the picture, so a wide picture gets lines of a similar look
        std::vector<float> edge(static_cast<size_t>(w) * h);
        par_rows(h, [&](int ry0, int ry1) {
            for (int y = ry0; y < ry1; ++y)
                for (int x = 0; x < w; ++x) {
                    const float gx = (L(x + rr, y - rr) + 2 * L(x + rr, y) + L(x + rr, y + rr)) - (L(x - rr, y - rr) + 2 * L(x - rr, y) + L(x - rr, y + rr));
                    const float gy = (L(x - rr, y + rr) + 2 * L(x, y + rr) + L(x + rr, y + rr)) - (L(x - rr, y - rr) + 2 * L(x, y - rr) + L(x + rr, y - rr));
                    {   // a floor under the noise and a steeper rise: clean lines on a deeper black (more contrast), not blown out
                        const float m = std::sqrt(gx * gx + gy * gy) / (4.0f * 255.0f) * 3.2f;
                        edge[static_cast<size_t>(y) * w + x] = fx_smooth(0.10f, 0.85f, m);
                    }
                }
        });
        const int f = braille ? 1 : std::max(1, w / 200);
        auto edge_col = [&](int x, int y, float* o) {   // the colour of the outline: the picture's colour here, made vivid (19: the wave)
            if (wavy) { wave(x, y, o); return; }
            const uint8_t* s = &src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
            const float mx = std::max({static_cast<float>(s[0]), static_cast<float>(s[1]), static_cast<float>(s[2]), 40.0f});
            const float mean = (s[0] + s[1] + s[2]) / 3.0f / mx;
            for (int k = 0; k < 3; ++k) o[k] = std::clamp((s[k] / mx - mean) * 1.3f + mean * 0.9f + 0.08f, 0.0f, 1.0f);
        };
        std::vector<float> colv(static_cast<size_t>(w) * h * 3);   // the outline colour of every pixel, worked out once (it is used twice)
        par_rows(h, [&](int ry0, int ry1) {
            for (int y = ry0; y < ry1; ++y)
                for (int x = 0; x < w; ++x) edge_col(x, y, &colv[(static_cast<size_t>(y) * w + x) * 3]);
        });
        FxSmall glow = fx_small(w, h, f, 3, [&](int x, int y, float* o) {
            const float* c = &colv[(static_cast<size_t>(y) * w + x) * 3];
            const float e = edge[static_cast<size_t>(y) * w + x];
            for (int k = 0; k < 3; ++k) o[k] = c[k] * e;
        });
        fx_blur(glow, braille ? 1 : 2, 2);
        par_rows(h, [&](int ry0, int ry1) {
            for (int y = ry0; y < ry1; ++y)
                for (int x = 0; x < w; ++x) {
                    const float* c = &colv[(static_cast<size_t>(y) * w + x) * 3];
                    const float e = edge[static_cast<size_t>(y) * w + x];
                    const uint8_t* s = &src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                    uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                    float gs[3];
                    fx_sample3(glow, x, y, gs);
                    if (wavy) {   // the picture in grey, tinted a little by the wave, under outlines that glow in the wave's colours
                        const float lum = fx_luma(s) / 255.0f;
                        for (int k = 0; k < 3; ++k) q[k] = fx_clamp(255.0f * (lum * (0.03f + 0.05f * c[k]) + c[k] * e * 1.0f + gs[k] * 0.75f));   // the grey areas take the wave's colour too
                    } else {
                        for (int k = 0; k < 3; ++k) q[k] = fx_clamp(s[k] * 0.03f + 255.0f * (c[k] * e * 1.0f + gs[k] * 0.7f) + (e > 0.85f ? 20.0f * (e - 0.85f) * 6.0f : 0.0f));
                    }
                }
        });
    } else if (fx == 17) {   // interlace: the odd lines come from the frame before -- where something moved the lines comb
        const bool pv = prev && prev->w == w && prev->h == h && prev->rgb.size() >= src.size();
        for (int y = 0; y < h; ++y) {
            const uint8_t* s = (y & 1) && pv ? &prev->rgb[static_cast<size_t>(y) * stride] : &src[static_cast<size_t>(y) * stride];
            const float line = (y & 1) ? 0.97f + 0.03f * fx_rand(tick, static_cast<uint32_t>(y >> 1)) : 1.0f;   // the lines twitter a little
            for (int x = 0; x < w * 3; ++x) px[static_cast<size_t>(y) * stride + x] = fx_clamp(s[x] * line * beam(y, braille ? 0.10f : 0.16f));
        }
    } else if (fx == 4 || fx == 7) {   // glitch (7: quiet -- few big hits, many small particles)
        const bool quiet = fx == 7;
        const uint32_t slot = static_cast<uint32_t>(std::max(0.0, t) * 8.0);   // eighth of a second
        const bool burst = fx_rand(slot / 3, 17) < (quiet ? 0.07f : 0.30f);   // groups of three slots (0.375 s) in about a third of the cases
        for (int y = 0; y < h; ++y) {
            const float line = beam(y, braille ? 0.14f : 0.18f);
            for (int x = 0; x < w * 3; ++x) px[static_cast<size_t>(y) * stride + x] = fx_clamp(src[static_cast<size_t>(y) * stride + x] * line);
        }
        if (burst) {
            std::vector<uint8_t> row(stride);
            const int slices = quiet ? 2 + static_cast<int>(fx_rand(slot, 5) * 2.0f) : 4 + static_cast<int>(fx_rand(slot, 5) * 4.0f);
            for (int k = 0; k < slices; ++k) {
                const int y0 = static_cast<int>(fx_rand(slot, 100 + k) * h);
                const int sh = std::max(2, static_cast<int>((quiet ? 0.02f + 0.04f * fx_rand(slot, 200 + k) : 0.03f + 0.10f * fx_rand(slot, 200 + k)) * h));
                const int dx = static_cast<int>((fx_rand(slot, 300 + k) - 0.5f) * (quiet ? 0.09f : 0.22f) * w);
                const int split = std::max(1, w / 120) + static_cast<int>(fx_rand(slot, 400 + k) * (w / 90));
                const bool inv = fx_rand(slot, 500 + k) < 0.12f;
                for (int y = y0; y < std::min(h, y0 + sh); ++y) {
                    fx_shift_row(&src[static_cast<size_t>(y) * stride], row.data(), w, dx, split);
                    const float line = beam(y, 0.18f);
                    for (int x = 0; x < w * 3; ++x) {
                        float v = row[static_cast<size_t>(x)] * line;
                        if (inv) v = 255.0f - v;
                        px[static_cast<size_t>(y) * stride + x] = fx_clamp(v);
                    }
                }
            }
        }
        // Particles: small blocks of the picture that jump sideways with split colours, thin hairline hits, and coloured specks. A few in the
        // calm phases (on some frames), many in a burst. They change with every video frame (`tick`).
        {
            const bool small_hit = burst || fx_rand(tick / 2, 61) < (quiet ? 0.85f : 0.45f);
            if (small_hit) {
                const int blocks = burst ? 14 + static_cast<int>(fx_rand(tick, 62) * 14.0f) : (quiet ? 4 + static_cast<int>(fx_rand(tick, 63) * 7.0f) : 1 + static_cast<int>(fx_rand(tick, 63) * 3.0f));
                const int unit = std::max(2, w / (quiet ? 96 : 64));   // the size of a particle follows the picture
                for (int k = 0; k < blocks; ++k) {
                    const uint32_t sd = tick * 131U + static_cast<uint32_t>(k);
                    const int bw = unit * (1 + static_cast<int>(fx_rand(sd, 1) * 5.0f));
                    const int bh = std::max(1, unit / 2) * (1 + static_cast<int>(fx_rand(sd, 2) * 2.0f));
                    const int x0 = static_cast<int>(fx_rand(sd, 3) * std::max(1, w - bw));
                    const int y0 = static_cast<int>(fx_rand(sd, 4) * std::max(1, h - bh));
                    const int dx = static_cast<int>((fx_rand(sd, 5) - 0.5f) * unit * 10.0f);
                    const int split = 1 + static_cast<int>(fx_rand(sd, 6) * unit * 0.6f);
                    const float tint = fx_rand(sd, 7);   // 0..0.2: a colour pop on the block, 0.2..: plain
                    for (int y = y0; y < std::min(h, y0 + bh); ++y)
                        for (int x = x0; x < std::min(w, x0 + bw); ++x) {
                            const int xr = std::clamp(x - dx - split, 0, w - 1), xg = std::clamp(x - dx, 0, w - 1), xb = std::clamp(x - dx + split, 0, w - 1);
                            float r = src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(xr)];
                            float g = src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(xg) + 1];
                            float b = src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(xb) + 2];
                            if (tint < 0.10f) { r = 255.0f - r * 0.4f; g *= 0.6f; }          // magenta pop
                            else if (tint < 0.20f) { g = 255.0f - g * 0.4f; b = 255.0f - b * 0.4f; r *= 0.5f; }   // cyan pop
                            uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                            q[0] = fx_clamp(r); q[1] = fx_clamp(g); q[2] = fx_clamp(b);
                        }
                }
                // hairline hits: single rows that slip a little
                const int hairs = burst ? 5 : (quiet ? 2 : 0) + static_cast<int>(fx_rand(tick, 64) * (quiet ? 4.0f : 3.0f));
                for (int k = 0; k < hairs; ++k) {
                    const uint32_t sd = tick * 977U + static_cast<uint32_t>(k);
                    const int y = static_cast<int>(fx_rand(sd, 11) * h);
                    const int dx = static_cast<int>((fx_rand(sd, 12) - 0.5f) * 0.12f * w);
                    std::vector<uint8_t> row(stride);
                    fx_shift_row(&src[static_cast<size_t>(y) * stride], row.data(), w, dx, 1);
                    std::copy(row.begin(), row.end(), px + static_cast<size_t>(y) * stride);
                }
                // specks: coloured dots
                const int specks = burst ? 40 + static_cast<int>(fx_rand(tick, 65) * 40.0f) : (quiet ? 12 + static_cast<int>(fx_rand(tick, 66) * 16.0f) : 3 + static_cast<int>(fx_rand(tick, 66) * 6.0f));
                for (int k = 0; k < specks; ++k) {
                    const uint32_t sd = tick * 313U + static_cast<uint32_t>(k);
                    const int x = static_cast<int>(fx_rand(sd, 21) * w), y = static_cast<int>(fx_rand(sd, 22) * h);
                    const int sw = 1 + static_cast<int>(fx_rand(sd, 23) * std::max(1, unit));
                    const float hue = fx_rand(sd, 24);
                    const uint8_t cr = hue < 0.33f ? 255 : hue < 0.66f ? 40 : 255, cg = hue < 0.33f ? 40 : hue < 0.66f ? 255 : 255, cb = hue < 0.33f ? 220 : hue < 0.66f ? 255 : 255;
                    for (int xx = x; xx < std::min(w, x + sw); ++xx) {
                        uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(xx)];
                        q[0] = cr; q[1] = cg; q[2] = cb;
                    }
                }
            }
        }
    }
    // the base layer: the structure of the screen under the tube looks (the scanlines of base 0 are drawn by the looks themselves)
    if (base >= 1 && !braille && video_fx_uses_base(fx)) {
        const float strength = fx == 3 ? 0.55f : (fx == 4 || fx == 7) ? 0.5f : 1.0f;
        fx_apply_base(px, w, h, base, strength);
    }
}

void VideoPlayer::apply_grade(VideoFrame& f, int contrast, int colors, int chroma, int brightness) {
    const int w = f.w, h = f.h;
    if (w < 2 || h < 2 || f.rgb.size() < static_cast<size_t>(w) * h * 3) return;
    contrast = std::clamp(contrast, 0, 100);
    colors = std::clamp(colors, 0, 100);
    brightness = std::clamp(brightness, 0, 100);
    const size_t n = static_cast<size_t>(w) * h;
    if (contrast > 0 || colors < 100 || brightness != 50) {
        uint8_t lut[256];
        const double factor = 1.0 + contrast / 100.0 * 1.5;   // the same as the cover's contrast
        const double gamma = std::pow(2.0, -(brightness - 50) / 50.0);   // brightness 100 -> 0.5 (brighter), 0 -> 2 (darker); 0 and 255 stay
        for (int i = 0; i < 256; ++i) {
            const double b = 255.0 * std::pow(i / 255.0, gamma);
            lut[i] = fx_clamp(static_cast<float>((b - 127.5) * factor + 127.5));
        }
        const float sat = static_cast<float>(colors) / 100.0f;
        if (colors >= 100) {   // only the curve: a table lookup per byte
            par_rows(h, [&](int ry0, int ry1) {
                uint8_t* p = &f.rgb[static_cast<size_t>(ry0) * w * 3];
                uint8_t* e = &f.rgb[static_cast<size_t>(ry1) * w * 3];
                for (; p != e; ++p) *p = lut[*p];
            });
        } else {
            par_rows(h, [&](int ry0, int ry1) {
                for (size_t i = static_cast<size_t>(ry0) * w; i < static_cast<size_t>(ry1) * w; ++i) {
                    uint8_t* p = &f.rgb[i * 3];
                    float v[3] = {static_cast<float>(lut[p[0]]), static_cast<float>(lut[p[1]]), static_cast<float>(lut[p[2]])};
                    const float l = 0.299f * v[0] + 0.587f * v[1] + 0.114f * v[2];
                    for (float& c : v) c = l + (c - l) * sat;
                    p[0] = fx_clamp(v[0]); p[1] = fx_clamp(v[1]); p[2] = fx_clamp(v[2]);
                }
            });
        }
    }
    if (chroma > 0) {   // red is pushed outwards, blue inwards, more and more towards the corners (a lens that does not focus every colour alike)
        const float k = chroma == 1 ? 0.0045f : chroma == 2 ? 0.009f : 0.016f;
        const std::vector<uint8_t> src(f.rgb.begin(), f.rgb.begin() + static_cast<long>(n * 3));
        const float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
        auto bil = [&](float x, float y, int c) {
            x = std::clamp(x, 0.0f, static_cast<float>(w - 1)); y = std::clamp(y, 0.0f, static_cast<float>(h - 1));
            const int x0 = static_cast<int>(x), y0 = static_cast<int>(y), x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
            const float tx = x - static_cast<float>(x0), ty = y - static_cast<float>(y0);
            auto v = [&](int xx, int yy) { return static_cast<float>(src[(static_cast<size_t>(yy) * w + xx) * 3 + static_cast<size_t>(c)]); };
            return (v(x0, y0) * (1 - tx) + v(x1, y0) * tx) * (1 - ty) + (v(x0, y1) * (1 - tx) + v(x1, y1) * tx) * ty;
        };
        par_rows(h, [&](int ry0, int ry1) {
            for (int y = ry0; y < ry1; ++y)
                for (int x = 0; x < w; ++x) {
                    const float dx = (x - cx) / std::max(1.0f, cx), dy = (y - cy) / std::max(1.0f, cy);
                    const float r2 = (dx * dx + dy * dy) * 0.5f;
                    const float g = 1.0f + k * (0.4f + 1.2f * r2);
                    uint8_t* q = &f.rgb[(static_cast<size_t>(y) * w + x) * 3];
                    q[0] = fx_clamp(bil(cx + (x - cx) * g, cy + (y - cy) * g, 0));
                    q[2] = fx_clamp(bil(cx + (x - cx) / g, cy + (y - cy) / g, 2));
                }
        });
    }
}

void VideoPlayer::apply_look(VideoFrame& f, const VideoLook& look, double t, bool braille, const VideoFrame* prev) {
    const auto g0 = std::chrono::steady_clock::now();
    apply_grade(f, look.contrast, look.colors, look.chroma, look.brightness);
    const auto g1 = std::chrono::steady_clock::now();
    video_timing().grade_ms = std::chrono::duration<double, std::milli>(g1 - g0).count();
    video_timing().fx = look.fx;
    video_timing().fx_ms = 0.0;
    if (look.fx > 0) {
        apply_fx(f, look.fx, t, braille, &look.grad, prev, look.dither, look.base);
        video_timing().fx_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g1).count();
        // The tube looks came out a little too hard in the blacks: the shadows are lifted a touch (pixel and film less, they were only a bit too hard).
        const int fx = look.fx;
        const float lift = (fx == 1 || fx == 2 || fx == 3 || fx == 4 || fx == 7 || fx == 6) ? 16.0f : (fx == 5 || fx == 15) ? 9.0f : 0.0f;
        if (lift > 0.0f) {
            uint8_t lut[256];
            for (int v = 0; v < 256; ++v) lut[v] = static_cast<uint8_t>(std::lround(lift + v * (255.0f - lift) / 255.0f));
            par_rows(f.h, [&](int ry0, int ry1) {
                uint8_t* p = &f.rgb[static_cast<size_t>(ry0) * f.w * 3];
                uint8_t* e = &f.rgb[static_cast<size_t>(ry1) * f.w * 3];
                for (; p != e; ++p) *p = lut[*p];
            });
        }
    }
}

void VideoPlayer::set_fx_threads(int n) { g_fx_threads.store(std::max(0, std::min(n, 16)), std::memory_order_relaxed); }

VideoPlayer::VideoPlayer() = default;

VideoPlayer::~VideoPlayer() { halt(); }

void VideoPlayer::halt() {
    {
        std::lock_guard<std::mutex> lk(m_);
        ++gen_;
    }
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(child_m_);
        if (child_) child_->kill();
    }
    if (th_.joinable()) th_.join();
    {
        std::lock_guard<std::mutex> lk(child_m_);
        child_.reset();
    }
    running_ = false;
}

std::string VideoPlayer::status() {
    std::lock_guard<std::mutex> lk(m_);
    if (cur_valid_ || !q_.empty()) return std::string();
    if (!note_.empty()) return note_;
    return "video: starting ...";
}

void VideoPlayer::stop() {
    halt();
    std::lock_guard<std::mutex> lk(m_);
    q_.clear();
    cur_valid_ = false;
    cur_stale_ = false;
}

bool VideoPlayer::set_source(const std::string& path) {
    if (path == path_ && (path.empty() || has_video())) return has_video();
    halt();
    {
        std::lock_guard<std::mutex> lk(m_);
        q_.clear();
        cur_valid_ = false;
        cur_stale_ = false;
        eof_ = false;
    }
    path_ = path;
    vw_ = vh_ = 0;
    duration_ = 0.0;
    if (path.empty()) return false;
    // "1280,720,0" (width, height, a picture glued into an audio file = 1) and the duration of the container
    const ProcResult r = run_capture({"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                      "stream=width,height:stream_disposition=attached_pic:format=duration", "-of", "csv=p=0", path});
    if (!r.ok()) return false;
    std::istringstream in(r.out);
    std::string line;
    int w = 0, h = 0, pic = 0;
    bool have = false;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (!have && line.find(',') != std::string::npos) {
            if (std::sscanf(line.c_str(), "%d,%d,%d", &w, &h, &pic) >= 2) have = true;
        } else {
            char* end = nullptr;
            const double d = std::strtod(line.c_str(), &end);
            if (end != line.c_str() && d > 0) duration_ = d;
        }
    }
    if (!have || w <= 0 || h <= 0 || pic == 1) return false;
    vw_ = w;
    vh_ = h;
    return true;
}

void VideoPlayer::fit(int box_w, int box_h, double unit_aspect, int& out_w, int& out_h) const {
    const double ar = aspect() > 0 ? aspect() : 16.0 / 9.0;
    const double ua = unit_aspect > 0 ? unit_aspect : 1.0;
    box_w = std::max(2, box_w);
    box_h = std::max(2, box_h);
    double h = box_h, w = ar * h / ua;
    if (w > box_w) { w = box_w; h = w * ua / ar; }
    out_w = std::max(2, static_cast<int>(w) & ~1);
    out_h = std::max(2, static_cast<int>(h) & ~1);
}

void VideoPlayer::fit_cover(int box_w, int box_h, double unit_aspect, int& out_w, int& out_h) const {
    const double ar = aspect() > 0 ? aspect() : 16.0 / 9.0;
    const double ua = unit_aspect > 0 ? unit_aspect : 1.0;
    box_w = std::max(2, box_w);
    box_h = std::max(2, box_h);
    double h = box_h, w = ar * h / ua;
    if (w < box_w) { w = box_w; h = w * ua / ar; }
    out_w = std::max(box_w, static_cast<int>(std::ceil(w)) + 1) & ~1;
    out_h = std::max(box_h, static_cast<int>(std::ceil(h)) + 1) & ~1;
}

VideoFrame VideoPlayer::crop(const VideoFrame& f, int x0, int y0, int w, int h) {
    VideoFrame o;
    x0 = std::clamp(x0, 0, std::max(0, f.w - 1));
    y0 = std::clamp(y0, 0, std::max(0, f.h - 1));
    w = std::clamp(w, 1, f.w - x0);
    h = std::clamp(h, 1, f.h - y0);
    o.w = w; o.h = h; o.pts = f.pts;
    o.rgb.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        std::copy(f.rgb.begin() + (static_cast<size_t>(y0 + y) * f.w + x0) * 3, f.rgb.begin() + (static_cast<size_t>(y0 + y) * f.w + x0 + w) * 3, o.rgb.begin() + static_cast<size_t>(y) * w * 3);
    return o;
}

void VideoPlayer::start(double from, bool keep_picture) {
    halt();
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(m_);
        q_.clear();
        prev_valid_ = false;
        // a restart (seek, resize, coming back to the video view) keeps the last picture on screen until the new one is there (the
        // caller scales it to the new size meanwhile); only a new file has nothing to show
        cur_stale_ = keep_picture && cur_valid_;
        if (!cur_stale_) cur_valid_ = false;
        eof_ = false;
        gen = gen_;
    }
    running_ = true;
    {
        std::lock_guard<std::mutex> lk(m_);
        note_.clear();
        frames_decoded_ = 0;
        started_at_ = std::chrono::steady_clock::now();
    }
    th_ = std::thread([this, gen, from]() { run(gen, from, run_w_, run_h_); });
}

void VideoPlayer::set_buffer_mb(int mb) {
    std::lock_guard<std::mutex> lk(m_);
    buffer_bytes_ = static_cast<size_t>(std::max(1, mb)) << 20;
    cv_.notify_all();
}

void VideoPlayer::run(uint64_t gen, double from, int w, int h) {
    const bool sharp = run_sharp_;
    const std::vector<std::string> args = {"ffmpeg", "-v", "error", "-nostdin", "-ss", fmt_seconds(from), "-i", path_, "-an", "-sn", "-dn",
                                           "-vf", "fps=" + std::to_string(kFps) + ",scale=" + std::to_string(w) + ":" + std::to_string(h) + ":flags=" + (sharp ? "lanczos" : "bilinear"),
                                           "-pix_fmt", "rgb24", "-f", "rawvideo", "-"};
    ConsoleLog::instance().log_basic("video: " + command_text(args));
    std::unique_ptr<ChildProcess> child = spawn_capture(args, false);
    if (!child) {
        ConsoleLog::instance().log_basic("video: ffmpeg could not be started (is it installed and on the PATH?)");
        std::lock_guard<std::mutex> lk(m_);
        if (gen == gen_) { eof_ = true; note_ = "video: ffmpeg could not be started"; }
        return;
    }
    child->lower_priority();   // (after every start the decoder runs flat out to fill its buffer: the picture being drawn goes first)
    ChildProcess* raw = child.get();
    {
        std::lock_guard<std::mutex> lk(child_m_);
        child_ = std::move(child);
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        if (gen != gen_) { std::lock_guard<std::mutex> lk2(child_m_); if (child_) child_->kill(); return; }
    }
    const size_t frame_bytes = static_cast<size_t>(w) * h * 3;
    std::vector<uint8_t> buf(frame_bytes);
    uint64_t n = 0;
    for (;;) {
        size_t got = 0;
        while (got < frame_bytes) {
            const std::ptrdiff_t r = raw->read(reinterpret_cast<char*>(buf.data()) + got, frame_bytes - got);
            if (r <= 0) break;
            got += static_cast<size_t>(r);
        }
        if (got < frame_bytes) break;   // the end (or the process was killed)
        VideoFrame f;
        f.w = w; f.h = h;
        f.rgb.swap(buf);
        buf.resize(frame_bytes);
        f.pts = from + static_cast<double>(n) / kFps;
        ++n;
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return gen != gen_ || q_.size() < queue_max(frame_bytes); });
        if (gen != gen_) return;
        if (n == 1) ConsoleLog::instance().log_basic("video: first frame after " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at_).count()) + " ms (" + std::to_string(w) + "x" + std::to_string(h) + ")");
        q_.push_back(std::move(f));
    }
    bool none = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (gen == gen_) { eof_ = true; none = n == 0; }
    }
    if (none) {   // not a single frame: ask ffmpeg again for its complaint (it was not part of the pipe) and show it
        const ProcResult r = run_capture({"ffmpeg", "-v", "error", "-nostdin", "-ss", fmt_seconds(from), "-i", path_, "-an", "-frames:v", "1", "-f", "null", "-"}, true);
        std::string msg = r.out;
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
        const size_t nl = msg.find('\n');
        const std::string first = nl == std::string::npos ? msg : msg.substr(0, nl);
        ConsoleLog::instance().log_basic("video: no frame from ffmpeg (exit " + std::to_string(r.exit_code) + "): " + (msg.empty() ? std::string("no message") : msg.substr(0, 600)));
        std::lock_guard<std::mutex> lk(m_);
        if (gen == gen_) note_ = "video: ffmpeg gave no picture" + (first.empty() ? std::string() : ": " + first.substr(0, 80)) + " (see the console log)";
    }
}

size_t VideoPlayer::queue_max(size_t frame_bytes) const {   // (m_ is held by the caller)
    return std::max(kQueueMin, buffer_bytes_ / std::max<size_t>(1, frame_bytes));
}

const VideoFrame* VideoPlayer::previous_frame() const {
    if (!prev_valid_ || !cur_valid_ || prev_.w != cur_.w || prev_.h != cur_.h) return nullptr;
    return &prev_;
}

namespace {
// How different two pictures of one video are (mean absolute difference of the pixels), compared at the smaller of the two sizes.
double frame_diff(const VideoFrame& a, const VideoFrame& b) {
    if (a.w <= 0 || b.w <= 0 || a.rgb.empty() || b.rgb.empty()) return 1e18;
    const bool a_small = a.w <= b.w && a.h <= b.h;
    const VideoFrame& small = a_small ? a : b;
    const VideoFrame& big = a_small ? b : a;
    const VideoFrame scaled = (big.w == small.w && big.h == small.h) ? VideoFrame() : VideoPlayer::downscale(big, small.w, small.h);
    const VideoFrame& other = (big.w == small.w && big.h == small.h) ? big : scaled;
    if (other.rgb.size() != small.rgb.size()) return 1e18;
    long long d = 0;
    for (size_t i = 0; i < small.rgb.size(); ++i) d += std::abs(static_cast<int>(small.rgb[i]) - static_cast<int>(other.rgb[i]));
    return static_cast<double>(d) / static_cast<double>(small.rgb.size());
}
}  // namespace

const VideoFrame* VideoPlayer::frame_for(double t, int w, int h, bool playing, bool* fresh) {
    if (fresh) *fresh = false;
    if (!has_video() || w < 2 || h < 2) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    bool restart = !running_;
    bool same_place = false;   // the restart only changes the size / filter (not the time): the new decoder starts at the picture on screen
    if (running_ && sharp_ != run_sharp_) { restart = true; same_place = true; }   // another filter: the decoder starts again
    if (running_ && (w > run_w_ || h > run_h_)) {
        // a LARGER size (window, fullscreen, the big overlay, lyrics on / off): the decoder is restarted at the new size only once the
        // size has stayed the same for a moment -- while the window is being dragged the picture on screen is simply scaled
        if (w != pend_w_ || h != pend_h_) { pend_w_ = w; pend_h_ = h; pend_since_ = now; }
        if (now - pend_since_ >= std::chrono::milliseconds(250)) { restart = true; same_place = true; }
    } else {
        pend_w_ = pend_h_ = 0;
    }
    // A size that is not larger is served from the frames of the running decoder (averaged down in frame_for), so going from the big
    // overlay back to the main screen restarts nothing. Only when the decoder has been running at more than 2.5 times the pixels
    // that were asked for, for 30 s on end, it starts again at the size that is needed (less ffmpeg work, smaller frames).
    if (running_ && w <= run_w_ && h <= run_h_ && static_cast<int64_t>(w) * h * 10 < static_cast<int64_t>(run_w_) * run_h_ * 4) {
        if (oversize_since_ == std::chrono::steady_clock::time_point{}) oversize_since_ = now;
        else if (now - oversize_since_ >= std::chrono::seconds(30)) { restart = true; same_place = true; }
    } else {
        oversize_since_ = std::chrono::steady_clock::time_point{};
    }
    bool eof = false;
    double last = 0.0;
    {
        std::lock_guard<std::mutex> lk(m_);
        eof = eof_;
        // how far the decoder has come: the newest queued frame, else the shown one -- but a picture left over from before a restart
        // says nothing about the new decoder (it starts at run_from_)
        last = !q_.empty() ? q_.back().pts : (cur_valid_ && !cur_stale_ ? cur_.pts : run_from_);
        const bool decoded = !q_.empty() || (cur_valid_ && !cur_stale_);
        const bool settled = now - restarted_at_ >= std::chrono::milliseconds(400);   // (a seek burst makes one restart, not one per key repeat)
        if (!restart && cur_valid_ && !cur_stale_ && t < cur_.pts - 0.5) restart = true;     // sought back
        // sought ahead: the decoder waits with a full queue far behind the clock (a slow decoder is not restarted: it catches up by
        // showing the newest frame, a restart would throw its work away); or the clock is very far ahead of anything decoded
        if (!restart && !eof && decoded && settled && t > last + 2.0 && (q_.size() >= queue_max(static_cast<size_t>(run_w_) * run_h_ * 3) || t > last + 10.0)) restart = true;
        // the new decoder has not delivered anything yet and the clock has left its start behind (a second seek while it started)
        if (!restart && !decoded && !eof && settled && std::fabs(t - run_from_) > 3.0 && now - restarted_at_ >= std::chrono::milliseconds(1500)) restart = true;
        if (!restart && !decoded && !eof && t > run_from_ + 20.0) restart = true;   // nothing at all for a very long time
    }
    if (restart) {
        run_w_ = w; run_h_ = h; run_sharp_ = sharp_;
        run_from_ = std::max(0.0, t);
        if (same_place) {
            // Resized (main UI <-> big overlay, full screen, lyrics on / off) or another filter: the new decoder's frames lie on a grid that
            // starts at its own start time, so starting at `t` showed a picture up to one frame period away from the one that was on screen -- a
            // paused video moved by a frame when the size changed. Starting at the shown frame's own time gives that same picture again.
            std::lock_guard<std::mutex> lk(m_);
            if (cur_valid_ && !cur_stale_ && std::fabs(t - cur_.pts) < 1.0) {
                // The new decoder starts a little before the picture on screen; which of its first frames IS that picture is decided by
                // comparing them (the frame times of a decoder do not line up with the source's own frames, so the same time can be the
                // next source frame). Only while paused: when playing, the clock decides anyway.
                run_from_ = std::max(0.0, cur_.pts - (playing ? 0.0 : 2.0 / kFps));
                if (!playing) { match_active_ = true; match_pts_ = cur_.pts; match_ref_ = cur_; }
            }
        }
        oversize_since_ = std::chrono::steady_clock::time_point{};
        restarted_at_ = now;
        pend_w_ = pend_h_ = 0;
        start(run_from_, true);
    }
    std::lock_guard<std::mutex> lk(m_);
    bool changed = false;
    if (match_active_ && (!cur_stale_ || playing)) match_active_ = false;   // (nothing to match any more, or the clock decides)
    if ((!cur_valid_ || cur_stale_) && !q_.empty()) {
        if (match_active_ && q_.size() < 4 && !eof_ && now - restarted_at_ < std::chrono::milliseconds(1500)) {
            // wait for the first few frames of the new decoder (the old picture stays meanwhile)
        } else {
            if (match_active_) {   // the frame among the first few that looks most like the one on screen; the earlier ones are dropped
                size_t best = 0;
                double best_d = -1.0;
                const size_t n = std::min<size_t>(4, q_.size());
                for (size_t i = 0; i < n; ++i) {
                    const double d = frame_diff(q_[i], match_ref_);
                    if (best_d < 0.0 || d < best_d) { best_d = d; best = i; }
                }
                for (size_t i = 0; i < best; ++i) q_.pop_front();
                q_.front().pts = match_pts_;   // (the picture keeps its time: nothing about the clock changes)
                match_active_ = false;
                match_ref_ = VideoFrame();
            }
            prev_valid_ = false; cur_ = std::move(q_.front()); q_.pop_front(); cur_valid_ = true; cur_stale_ = false; changed = true;
        }
    }
    if (playing) {
        while (!q_.empty() && q_.front().pts <= t) { if (cur_valid_ && !cur_stale_) { prev_ = std::move(cur_); prev_valid_ = true; } cur_ = std::move(q_.front()); q_.pop_front(); cur_valid_ = true; changed = true; }
        if (changed) cv_.notify_all();
    }
    if (changed) { cur_returned_ = false; scaled_valid_ = false; }
    if (fresh) *fresh = changed || (cur_valid_ && !cur_returned_);
    if (cur_valid_) cur_returned_ = true;
    if (!cur_valid_) return nullptr;
    // the decoder runs at a larger size than asked for (see scaled_): the same frame, averaged down once per frame and size
    if (cur_.w >= w && cur_.h >= h && (cur_.w != w || cur_.h != h)) {
        if (!scaled_valid_ || scaled_.w != w || scaled_.h != h) { scaled_ = downscale(cur_, w, h); scaled_valid_ = true; }
        return &scaled_;
    }
    return &cur_;
}

VideoFrame VideoPlayer::test_card(int w, int h) {
    VideoFrame o;
    w = std::max(2, w); h = std::max(2, h);
    o.w = w; o.h = h;
    o.rgb.assign(static_cast<size_t>(w) * h * 3, 0);
    static const uint8_t bars[7][3] = {{235, 235, 235}, {235, 235, 16}, {16, 235, 235}, {16, 235, 16}, {235, 16, 235}, {235, 16, 16}, {16, 16, 235}};
    auto put = [&](int x, int y, float r, float g, float b) {
        uint8_t* q = &o.rgb[(static_cast<size_t>(y) * w + static_cast<size_t>(x)) * 3];
        q[0] = fx_clamp(r); q[1] = fx_clamp(g); q[2] = fx_clamp(b);
    };
    const int y1 = h * 55 / 100, y2 = h * 70 / 100;   // bars | grey ramp | the three pictures below
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
            if (y < y1) {   // colour bars, a little shade towards the bottom
                const uint8_t* c = bars[std::min(6, static_cast<int>(u * 7.0f))];
                const float k = 1.0f - 0.18f * static_cast<float>(y) / static_cast<float>(std::max(1, y1));
                put(x, y, c[0] * k, c[1] * k, c[2] * k);
            } else if (y < y2) {   // grey ramp
                const float v = 255.0f * u;
                put(x, y, v, v, v);
            } else {
                const float v = (static_cast<float>(y - y2) + 0.5f) / static_cast<float>(std::max(1, h - y2));   // 0..1 down the lower part
                if (u < 1.0f / 3.0f) {   // a hue sweep, softer towards the bottom
                    const float hh = u * 3.0f * 6.0f;
                    const float r = std::clamp(std::fabs(hh - 3.0f) - 1.0f, 0.0f, 1.0f), g = std::clamp(2.0f - std::fabs(hh - 2.0f), 0.0f, 1.0f), b = std::clamp(2.0f - std::fabs(hh - 4.0f), 0.0f, 1.0f);
                    const float m = 70.0f * v;
                    put(x, y, 40.0f + m + 190.0f * r * (1.0f - 0.3f * v), 40.0f + m + 190.0f * g * (1.0f - 0.3f * v), 40.0f + m + 190.0f * b * (1.0f - 0.3f * v));
                } else if (u < 2.0f / 3.0f) {   // a lit disc on a dark blue ground: the bright spot a glow grows from
                    const float dx = (u - 0.5f) * static_cast<float>(w) / static_cast<float>(std::max(1, h - y2)), dy = v - 0.5f;
                    const float lit = 1.0f - std::clamp((std::sqrt(dx * dx + dy * dy) - 0.30f) / 0.12f, 0.0f, 1.0f);
                    put(x, y, 18.0f + 232.0f * lit, 26.0f + 220.0f * lit, 70.0f + 120.0f * lit);
                } else {   // a ramp from black to white, top to bottom, with a warm tint on the left
                    const float t = (u - 2.0f / 3.0f) * 3.0f;
                    const float g = 255.0f * v;
                    put(x, y, g * (0.85f + 0.15f * (1.0f - t)), g * (0.9f + 0.05f * (1.0f - t)), g * (1.0f - 0.2f * (1.0f - t)));
                }
            }
        }
    return o;
}

VideoFrame VideoPlayer::downscale(const VideoFrame& f, int w, int h) {
    if (w <= 0 || h <= 0 || f.w <= 0 || f.h <= 0 || f.rgb.size() < static_cast<size_t>(f.w) * f.h * 3) { VideoFrame o; o.w = w; o.h = h; o.pts = f.pts; return o; }
    if (w >= f.w || h >= f.h) return resample(f, w, h);
    VideoFrame o;
    o.w = w; o.h = h; o.pts = f.pts;
    o.rgb.resize(static_cast<size_t>(w) * h * 3);
    // the source columns / rows of every destination pixel: [a, b), at least one
    std::vector<int> xa(static_cast<size_t>(w)), xb(static_cast<size_t>(w));
    for (int x = 0; x < w; ++x) {
        xa[static_cast<size_t>(x)] = static_cast<int>(static_cast<int64_t>(x) * f.w / w);
        xb[static_cast<size_t>(x)] = std::max(xa[static_cast<size_t>(x)] + 1, std::min(f.w, static_cast<int>((static_cast<int64_t>(x + 1) * f.w + w - 1) / w)));
    }
    for (int y = 0; y < h; ++y) {
        const int ya = static_cast<int>(static_cast<int64_t>(y) * f.h / h);
        const int yb = std::max(ya + 1, std::min(f.h, static_cast<int>((static_cast<int64_t>(y + 1) * f.h + h - 1) / h)));
        for (int x = 0; x < w; ++x) {
            const int x0 = xa[static_cast<size_t>(x)], x1 = xb[static_cast<size_t>(x)];
            int r = 0, g = 0, b = 0;
            for (int sy = ya; sy < yb; ++sy) {
                const uint8_t* p = f.rgb.data() + (static_cast<size_t>(sy) * f.w + x0) * 3;
                for (int sx = x0; sx < x1; ++sx, p += 3) { r += p[0]; g += p[1]; b += p[2]; }
            }
            const int n = (x1 - x0) * (yb - ya);
            const size_t di = (static_cast<size_t>(y) * w + x) * 3;
            o.rgb[di] = static_cast<uint8_t>((r + n / 2) / n);
            o.rgb[di + 1] = static_cast<uint8_t>((g + n / 2) / n);
            o.rgb[di + 2] = static_cast<uint8_t>((b + n / 2) / n);
        }
    }
    return o;
}

VideoFrame VideoPlayer::resample(const VideoFrame& f, int w, int h) {
    VideoFrame o;
    o.w = w; o.h = h; o.pts = f.pts;
    if (w <= 0 || h <= 0 || f.w <= 0 || f.h <= 0 || f.rgb.size() < static_cast<size_t>(f.w) * f.h * 3) return o;
    if (f.w * 2 >= w * 3 && f.h * 2 >= h * 3) return downscale(f, w, h);   // (1.5 times as large or more in both directions: average)
    o.rgb.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        const int sy = std::min(f.h - 1, static_cast<int>((static_cast<int64_t>(y) * f.h) / h));
        for (int x = 0; x < w; ++x) {
            const int sx = std::min(f.w - 1, static_cast<int>((static_cast<int64_t>(x) * f.w) / w));
            const size_t si = (static_cast<size_t>(sy) * f.w + sx) * 3, di = (static_cast<size_t>(y) * w + x) * 3;
            o.rgb[di] = f.rgb[si]; o.rgb[di + 1] = f.rgb[si + 1]; o.rgb[di + 2] = f.rgb[si + 2];
        }
    }
    return o;
}

// ---- terminal output ---------------------------------------------------------------------------------------------------------

namespace {
// The six levels of every channel of the 6x6x6 colour cube. Not evenly spaced any more: more of them in the dark half, where a tinted
// shadow (the gradient look!) used to fall between two far apart levels and came out grey or black.
const uint8_t kCubeLevel[6] = {0, 40, 85, 135, 190, 255};
}  // namespace

const std::vector<std::array<uint8_t, 3>>& VideoPlayer::palette() {
    static const std::vector<std::array<uint8_t, 3>> pal = [] {
        std::vector<std::array<uint8_t, 3>> p;
        for (int r = 0; r < 6; ++r)
            for (int g = 0; g < 6; ++g)
                for (int b = 0; b < 6; ++b) p.push_back({kCubeLevel[r], kCubeLevel[g], kCubeLevel[b]});
        for (int i = 0; i < 24; ++i) { const uint8_t v = static_cast<uint8_t>(std::lround(i * 255.0 / 23.0)); p.push_back({v, v, v}); }
        for (int i = 0; i < 8; ++i) p.push_back({kGameBoy[i][0], kGameBoy[i][1], kGameBoy[i][2]});   // 240 .. 247: the Game Boy greens and greys, so they are drawn flat
        return p;
    }();
    return pal;
}

void VideoPlayer::quantize(const VideoFrame& f, std::vector<uint8_t>& hue, std::vector<uint8_t>& level, int dither) {
    const size_t n = static_cast<size_t>(f.w) * f.h;
    hue.assign(n, 0);
    level.assign(n, 255);
    if (f.rgb.size() < n * 3) return;
    // dither between the two nearest levels of a channel: the share of the way up to the next level against a blue-noise threshold
    auto cube_index = [](int v, float thr) {
        int i = 0;
        while (i < 4 && v >= kCubeLevel[i + 1]) ++i;                                   // kCubeLevel[i] <= v < kCubeLevel[i + 1]  (v == 255 -> i = 4)
        const float fr = static_cast<float>(v - kCubeLevel[i]) / static_cast<float>(kCubeLevel[i + 1] - kCubeLevel[i]);
        return fr > thr ? i + 1 : i;
    };
    const int qpat = fx_pat(dither, 1);   // (auto: blue noise)
    par_rows(f.h, [&](int ry0, int ry1) {
        for (int y = ry0; y < ry1; ++y) {
            for (int x = 0; x < f.w; ++x) {
                const size_t i = static_cast<size_t>(y) * f.w + x;
                const int r = f.rgb[i * 3], g = f.rgb[i * 3 + 1], b = f.rgb[i * 3 + 2];
                const int mx = std::max({r, g, b});
                // The dark floor: the effects (glow, flicker, gaps of the lines) leave the black parts of a picture a hair above black, and the
                // dither turned that into a visible raster of little squares over every dark area. What is almost black stays black.
                int flat = -1;   // exactly one of the Game Boy greens: flat, no dither
                for (int k = 0; k < 8; ++k) {
                    const int ex = r - kGameBoy[k][0], ey = g - kGameBoy[k][1], ez = b - kGameBoy[k][2];
                    if (ex * ex + ey * ey + ez * ez <= 12) flat = k;
                }
                if (flat >= 0) { hue[i] = static_cast<uint8_t>(240 + flat); continue; }
                if (mx <= 30) { hue[i] = 216; continue; }   // (216 = the black of the grey ramp)
                // (blue noise, not the 4 x 4 Bayer matrix: that one drew a regular lattice of little squares over every smooth area -- a gradient look in
                // the 256 colours of a terminal's picture protocol; the mean tone is the same, but there is no repeating pattern to see)
                const float thr = (static_cast<float>(fx_dither_rank(qpat, x, y)) + 0.5f) / 1024.0f;
                // A colour with some saturation is dithered over the colour cube (a dim tint keeps being a tint and does not collapse into the grey
                // ramp); only an almost grey one takes the finer ramp of 24 greys.
                const bool use_cube = mx - std::min({r, g, b}) >= std::max(10, mx * 12 / 100);   // (a pale off-white is still "almost grey": relative to its brightness)
                if (use_cube) {
                    hue[i] = static_cast<uint8_t>(36 * cube_index(r, thr) + 6 * cube_index(g, thr) + cube_index(b, thr));
                } else {   // the ramp of 24 greys, dithered the same way
                    const float gv = (r + g + b) / 3.0f * 23.0f / 255.0f;
                    const int gi0 = std::clamp(static_cast<int>(gv), 0, 22);
                    hue[i] = static_cast<uint8_t>(216 + ((gv - static_cast<float>(gi0)) > thr ? gi0 + 1 : gi0));
                }
            }
        }
    });
}

std::vector<std::vector<VideoPlayer::Cell>> VideoPlayer::braille(const VideoFrame& f, int cols, int rows) {
    std::vector<std::vector<Cell>> out(static_cast<size_t>(std::max(0, rows)), std::vector<Cell>(static_cast<size_t>(std::max(0, cols))));
    static const uint8_t bit[2][4] = {{1, 2, 4, 64}, {8, 16, 32, 128}};   // [dot column][dot row]
    auto luma = [&](int x, int y) -> int {
        if (x < 0 || y < 0 || x >= f.w || y >= f.h) return 0;
        const size_t i = (static_cast<size_t>(y) * f.w + x) * 3;
        return (f.rgb[i] * 77 + f.rgb[i + 1] * 150 + f.rgb[i + 2] * 29) >> 8;
    };
    for (int cy = 0; cy < rows; ++cy)
        for (int cx = 0; cx < cols; ++cx) {
            int mask = 0, sum = 0;
            for (int dx = 0; dx < 2; ++dx)
                for (int dy = 0; dy < 4; ++dy) {
                    const int x = cx * 2 + dx, y = cy * 4 + dy;
                    const int l = luma(x, y);
                    sum += l;
                    const int thr = (static_cast<int>(kBayer4[y & 3][x & 3]) * 16 + 8);   // 8 .. 248
                    if (l > thr) mask |= bit[dx][dy];
                }
            Cell& c = out[static_cast<size_t>(cy)][static_cast<size_t>(cx)];
            c.mask = static_cast<uint8_t>(mask);
            c.level = static_cast<uint8_t>(sum / 8);
        }
    return out;
}

}  // namespace muisc
