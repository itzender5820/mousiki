#include "video_player.h"

#include "console_log.h"
#include "path_utf8.h"

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
inline void fx_even_wave(const VideoFxGradient& g, float* c) {
    float lo = 255.0f;
    for (int i = 0; i < 3; ++i) { const float v[3] = {static_cast<float>(g.c[i][0]), static_cast<float>(g.c[i][1]), static_cast<float>(g.c[i][2])}; lo = std::min(lo, fx_wave_y(v)); }
    const float y = fx_wave_y(c);
    if (y > lo && y > 1.0f) { const float k = std::max(lo, 80.0f) / y; if (k < 1.0f) for (int i = 0; i < 3; ++i) c[i] *= k; }
}

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
// The value of channel k at the full-resolution pixel (x, y), smoothly (bilinear between the blocks).
inline float fx_sample(const FxSmall& s, int k, int x, int y) {
    const float fx = (static_cast<float>(x) + 0.5f) / s.f - 0.5f, fy = (static_cast<float>(y) + 0.5f) / s.f - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, s.w - 1), y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, s.h - 1);
    const int x1 = std::min(s.w - 1, x0 + 1), y1 = std::min(s.h - 1, y0 + 1);
    const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f), ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
    auto v = [&](int xx, int yy) { return s.v[(static_cast<size_t>(yy) * s.w + xx) * s.c + k]; };
    return (v(x0, y0) * (1 - tx) + v(x1, y0) * tx) * (1 - ty) + (v(x0, y1) * (1 - tx) + v(x1, y1) * tx) * ty;
}
// 8 x 8 Bayer threshold (0..63) built from the 4 x 4 one
inline int fx_bayer8(int x, int y) {
    static const int q[2][2] = {{0, 2}, {3, 1}};
    return 4 * kBayer4[y & 3][x & 3] + q[(y >> 2) & 1][(x >> 2) & 1];
}
const uint8_t kEga[16][3] = {{0, 0, 0},     {0, 0, 170},   {0, 170, 0},   {0, 170, 170}, {170, 0, 0},   {170, 0, 170}, {170, 85, 0}, {170, 170, 170},
                             {85, 85, 85},  {85, 85, 255}, {85, 255, 85}, {85, 255, 255}, {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255}};
const uint8_t kGameBoy[8][3] = {{8, 32, 12}, {48, 98, 48}, {139, 172, 15}, {155, 188, 15}, {0, 0, 0}, {85, 85, 85}, {170, 170, 170}, {255, 255, 255}};   // 0..3 the greens, 4..7 the black and white version   // (the darkest green is darker than the handheld's: dark areas stay dark)

}  // namespace

void VideoPlayer::apply_fx(VideoFrame& f, int fx, double t, bool braille, const VideoFxGradient* grad, const VideoFrame* prev) {
    const int w = f.w, h = f.h;
    if (fx <= 0 || w < 4 || h < 4 || f.rgb.size() < static_cast<size_t>(w) * h * 3) return;
    const size_t stride = static_cast<size_t>(w) * 3;
    uint8_t* px = f.rgb.data();
    const std::vector<uint8_t> src(f.rgb.begin(), f.rgb.begin() + static_cast<long>(stride * h));   // the untouched frame
    const uint32_t tick = static_cast<uint32_t>(std::max(0.0, t) * 25.0);   // one step per video frame
    // A scanline gap every `period` rows. A tall picture gets 3 rows per line, a small one 2 (braille: 4 dots per cell row -> 2).
    const int period = (!braille && h >= 300) ? 3 : 2;
    auto beam = [&](int y, float depth) {   // 1 on the bright row(s) of a line, 1 - depth in the gap, a soft ramp between (a raised cosine)
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
        std::vector<float> glow(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float acc = 0.0f;
                for (int k = -2; k <= 2; ++k) {
                    const int xx = std::clamp(x + k, 0, w - 1);
                    const uint8_t* q = &src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(xx)];
                    const float lum = 0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2];
                    acc += std::max(0.0f, lum - 150.0f) * (k == 0 ? 0.36f : std::abs(k) == 1 ? 0.24f : 0.08f);
                }
                glow[static_cast<size_t>(y) * w + x] = acc;
            }
        const float bar_y = (1.0f - std::fmod(static_cast<float>(t) * 0.07f, 1.0f)) * h;   // the refresh bar climbs the picture
        const float bar_h = std::max(4.0f, h * 0.22f);
        const float flick = 1.0f + 0.010f * std::sin(static_cast<float>(t) * 31.0f);
        for (int y = 0; y < h; ++y) {
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
    } else if (fx == 2) {   // crt
        const float depth = braille ? 0.30f : 0.40f;
        const bool mask = !braille && w >= 160;   // the RGB stripes need pixels to be seen
        const float flick = 1.0f + 0.012f * std::sin(static_cast<float>(t) * 47.0f) + 0.008f * (fx_rand(tick, 3) - 0.5f);
        for (int y = 0; y < h; ++y) {
            const float dy = (static_cast<float>(y) + 0.5f) / h * 2.0f - 1.0f;
            const float line = beam(y, depth) * (1.0f + depth * 0.30f);
            for (int x = 0; x < w; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) / w * 2.0f - 1.0f;
                const float vig = 1.0f - 0.34f * std::pow(std::fabs(dx), 2.6f) - 0.30f * std::pow(std::fabs(dy), 2.6f) - 0.10f * dx * dx * dy * dy;
                for (int c = 0; c < 3; ++c) {
                    float v = braille ? static_cast<float>(src[static_cast<size_t>(y) * stride + 3 * x + c]) : soft_sample(x, y, c);
                    if (mask) v *= (x % 3 == c) ? 1.08f : 0.86f;   // the phosphor stripe of this colour is lit, the other two are dim
                    // a touch more contrast, like a tube's gamma
                    v = 255.0f * std::pow(std::clamp(v / 255.0f, 0.0f, 1.0f), 1.08f);
                    px[static_cast<size_t>(y) * stride + 3 * x + c] = fx_clamp(v * line * std::max(0.35f, vig) * flick);
                }
            }
        }
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
        const float shift = static_cast<float>(t) * 0.10f;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float p = (static_cast<float>(x) / w + static_cast<float>(y) / h) * 0.5f;   // 0 upper left .. 1 lower right
                float u = std::fmod(p * 1.25f - shift, 1.0f);   // the wave travels towards the lower right
                if (u < 0.0f) u += 1.0f;
                const float k = 1.0f - std::fabs(2.0f * u - 1.0f);   // there and back: left colour - centre - right colour - centre ...
                const float pos = k * 2.0f;
                const int i0 = std::min(1, static_cast<int>(pos));
                const float fr = pos - static_cast<float>(i0);
                const uint8_t* s = &src[static_cast<size_t>(y) * stride + 3 * x];
                const float lum = (0.299f * s[0] + 0.587f * s[1] + 0.114f * s[2]) / 255.0f;
                // The wave's colour only goes through the light parts: the shade follows the brightness, a dark area stays dark (no grey
                // wash of the original is mixed in), so the dark parts of the picture do not turn into big coloured bands.
                const float shade = braille ? 1.10f * lum : 1.12f * std::pow(lum, 1.15f);
                float wcol[3];
                for (int c = 0; c < 3; ++c) {
                    wcol[c] = g.c[i0][c] * (1.0f - fr) + g.c[i0 + 1][c] * fr;
                }
                fx_even_wave(g, wcol);
                for (int c = 0; c < 3; ++c) px[static_cast<size_t>(y) * stride + 3 * x + c] = fx_clamp(wcol[c] * shade);
            }
    } else if (fx == 8 || fx == 9) {   // phosphor: a monochrome tube (green or amber) with a glow around the bright parts
        const float tr = fx == 8 ? 60.0f : 255.0f, tg = fx == 8 ? 255.0f : 170.0f, tb = fx == 8 ? 100.0f : 30.0f;
        const int f = braille ? 1 : std::max(1, w / 200);
        FxSmall glow = fx_small(w, h, f, 1, [&](int x, int y, float* o) { o[0] = std::max(0.0f, fx_luma(&src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)]) / 255.0f - 0.35f) / 0.65f; });
        fx_blur(glow, braille ? 1 : 2, 2);
        const float flick = 1.0f + 0.010f * std::sin(static_cast<float>(t) * 53.0f) + 0.006f * (fx_rand(tick, 5) - 0.5f);
        for (int y = 0; y < h; ++y) {
            const float dy = (static_cast<float>(y) + 0.5f) / h * 2.0f - 1.0f;
            const float line = beam(y, braille ? 0.22f : 0.36f);
            for (int x = 0; x < w; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) / w * 2.0f - 1.0f;
                const float vig = 1.0f - 0.26f * std::pow(std::fabs(dx), 2.4f) - 0.22f * std::pow(std::fabs(dy), 2.4f);
                const float l = fx_luma(&src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)]) / 255.0f;
                float v = std::pow(l, 1.12f) * 0.92f + fx_sample(glow, 0, x, y) * 0.55f;
                v *= line * std::max(0.4f, vig) * flick;
                const float hot = std::max(0.0f, v - 0.85f) * 1.6f;   // the hottest spots burn towards white
                uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                const float k = std::min(v, 1.0f);
                q[0] = fx_clamp(4.0f + tr * k + 255.0f * hot * 0.45f);
                q[1] = fx_clamp(8.0f + tg * k + 255.0f * hot * 0.45f);
                q[2] = fx_clamp(5.0f + tb * k + 255.0f * hot * 0.45f);
            }
        }
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
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float u = (static_cast<float>(x) * ca + static_cast<float>(y) * sa) / cs, v = (-static_cast<float>(x) * sa + static_cast<float>(y) * ca) / cs;
                const float cu = std::floor(u) + 0.5f, cv = std::floor(v) + 0.5f;
                const float du = u - cu, dv = v - cv, d = std::sqrt(du * du + dv * dv);
                const int xc = std::clamp(static_cast<int>((cu * ca - cv * sa) * cs), 0, w - 1), yc = std::clamp(static_cast<int>((cu * sa + cv * ca) * cs), 0, h - 1);
                float c[3];
                sample(xc, yc, c);
                const float lum = (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]) / 255.0f;
                const float cov = std::clamp(1.0f - std::pow(lum, 0.95f), 0.0f, 1.0f);
                const float r = std::sqrt(cov) * 0.72f;
                const float ink = 1.0f - fx_smooth(r - aa, r + aa, d);
                uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                for (int k = 0; k < 3; ++k) {
                    const float paper = (k == 0 ? 243.0f : k == 1 ? 238.0f : 224.0f) * 0.84f + c[k] * 0.16f;   // warm paper with a hint of the picture
                    q[k] = fx_clamp(paper * (1.0f - ink) + c[k] * 0.50f * ink);
                }
            }
    } else if (fx == 11 || fx == 21 || fx == 22) {   // dither: a one-bit picture, ordered (Bayer) dots in two inks (21: plain black and white, 22: black and the gradient wave)
        const VideoFxGradient dg = grad ? *grad : VideoFxGradient();
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
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int x0 = std::max(0, x - rad), x1 = std::min(w, x + rad + 1), y0 = std::max(0, y - rad), y1 = std::min(h, y + rad + 1);
                    const double sum = ii[static_cast<size_t>(y1) * (w + 1) + x1] - ii[static_cast<size_t>(y0) * (w + 1) + x1] - ii[static_cast<size_t>(y1) * (w + 1) + x0] + ii[static_cast<size_t>(y0) * (w + 1) + x0];
                    lm[static_cast<size_t>(y) * w + x] = static_cast<float>(sum / (static_cast<double>(x1 - x0) * (y1 - y0)));
                }
        }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int bx = x / bs, by = y / bs;
                const int sx = std::min(w - 1, bx * bs + bs / 2), sy = std::min(h - 1, by * bs + bs / 2);
                float l = fx_luma(&src[static_cast<size_t>(sy) * stride + 3 * static_cast<size_t>(sx)]) / 255.0f;
                const float mean = lm[static_cast<size_t>(sy) * w + sx];
                l += 1.1f * (l - mean) * std::clamp(mean / 0.18f, 0.0f, 1.0f);
                const float thr = (static_cast<float>(fx_bayer8(bx, by)) + 0.5f) / 64.0f;
                const bool on = std::pow(std::clamp((l - 0.5f) * 1.12f + 0.5f, 0.0f, 1.0f), 1.2f) > thr;
                uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                if (fx == 22) {   // the light dots take the colour of the wave there, the dark ones stay black
                    float wc[3] = {0, 0, 0};
                    if (on) {
                        const float p = (static_cast<float>(sx) / w + static_cast<float>(sy) / h) * 0.5f;
                        float u = std::fmod(p * 1.25f - static_cast<float>(t) * 0.10f, 1.0f);
                        if (u < 0.0f) u += 1.0f;
                        const float pos = (1.0f - std::fabs(2.0f * u - 1.0f)) * 2.0f;
                        const int i0 = std::min(1, static_cast<int>(pos));
                        const float fr = pos - static_cast<float>(i0);
                        for (int k = 0; k < 3; ++k) wc[k] = dg.c[i0][k] * (1.0f - fr) + dg.c[i0 + 1][k] * fr;
                        fx_even_wave(dg, wc);
                    }
                    for (int k = 0; k < 3; ++k) q[k] = fx_clamp(wc[k]);
                } else if (fx == 21) q[0] = q[1] = q[2] = on ? 255 : 0;
                else { q[0] = on ? 236 : 14; q[1] = on ? 238 : 16; q[2] = on ? 226 : 28; }
            }
    } else if (fx == 12 || fx == 18 || fx == 20 || fx == 23 || fx == 24) {   // game boy (20: the four shades are the gradient wave's colour in four brightnesses): four greens, in a coarse grid with a light dither (18: low-res, big blocks)
        const bool low = fx == 18 || fx == 24;
        const int pal0 = (fx == 23 || fx == 24) ? 4 : 0;   // (the black and white version: the shades 4 .. 7 of kGameBoy)
        const VideoFxGradient gg = grad ? *grad : VideoFxGradient();
        static const float kShadeK[4] = {0.0f, 0.42f, 0.72f, 1.0f};   // (the darkest shade is black: the wave only runs through the light shades)
        auto wave_at = [&](int x, int y, float* o) {
            const float p = (static_cast<float>(x) / w + static_cast<float>(y) / h) * 0.5f;
            float u = std::fmod(p * 1.25f - static_cast<float>(t) * 0.10f, 1.0f);
            if (u < 0.0f) u += 1.0f;
            const float pos = (1.0f - std::fabs(2.0f * u - 1.0f)) * 2.0f;
            const int i0 = std::min(1, static_cast<int>(pos));
            const float fr = pos - static_cast<float>(i0);
            for (int k = 0; k < 3; ++k) o[k] = gg.c[i0][k] * (1.0f - fr) + gg.c[i0 + 1][k] * fr;
            fx_even_wave(gg, o);
        };
        const int bs = low ? (braille ? 2 : std::max(3, static_cast<int>(std::lround(w / 160.0))))
                           : (braille ? 1 : std::max(2, static_cast<int>(std::lround(w / 320.0))));
        for (int by = 0; by < h; by += bs)
            for (int bx = 0; bx < w; bx += bs) {
                const int x1 = std::min(w, bx + bs), y1 = std::min(h, by + bs);
                float sum = 0.0f;
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x) sum += fx_luma(&src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)]);
                float l = sum / (static_cast<float>((x1 - bx) * (y1 - by)) * 255.0f);
                l = std::clamp((l - 0.5f) * 1.18f + 0.5f, 0.0f, 1.0f);   // a little more contrast: four shades are few
                l = std::pow(l, 1.35f);   // and the dark half goes to the dark shades: shadows are not made lighter
                const float d = (static_cast<float>(kBayer4[(by / bs) & 3][(bx / bs) & 3]) / 15.0f - 0.5f) * 0.22f;
                const int idx = std::clamp(static_cast<int>(std::floor((l + d) * 4.0f)), 0, 3);
                float wc[3] = {0, 0, 0};
                if (fx == 20) wave_at(std::min(w - 1, bx + bs / 2), std::min(h - 1, by + bs / 2), wc);
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x) {
                        const bool gap = false;   // (no LCD gap any more: its grid of lines came out as squares in the 240-colour palette)
                        uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                        for (int k = 0; k < 3; ++k) q[k] = fx_clamp((fx == 20 ? wc[k] * kShadeK[idx] : kGameBoy[pal0 + idx][k]) * (gap ? 0.86f : 1.0f));
                    }
            }
    } else if (fx == 13) {   // ega 16: the 16 colours of the old PC graphics, ordered dither
        const int bs = (!braille && h >= 300) ? 2 : 1;
        for (int by = 0; by < h; by += bs)
            for (int bx = 0; bx < w; bx += bs) {
                const int x1 = std::min(w, bx + bs), y1 = std::min(h, by + bs);
                float sum[3] = {0, 0, 0};
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x)
                        for (int k = 0; k < 3; ++k) sum[k] += src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x) + k];
                const float n = static_cast<float>((x1 - bx) * (y1 - by));
                const float bias = (static_cast<float>(fx_bayer8(bx / bs, by / bs)) / 63.0f - 0.5f) * 80.0f;
                const float r = sum[0] / n + bias, g = sum[1] / n + bias, b = sum[2] / n + bias;
                int best = 0;
                float bd = 1e30f;
                for (int i = 0; i < 16; ++i) {
                    const float dr = r - kEga[i][0], dg = g - kEga[i][1], db = b - kEga[i][2];
                    const float dd = 0.30f * dr * dr + 0.59f * dg * dg + 0.11f * db * db;   // (the eye is most sensitive to green)
                    if (dd < bd) { bd = dd; best = i; }
                }
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x) {
                        uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                        q[0] = kEga[best][0]; q[1] = kEga[best][1]; q[2] = kEga[best][2];
                    }
            }
    } else if (fx == 14) {   // bloom: a soft glow around the bright parts, a hint of halation
        const int f = braille ? 1 : std::max(1, w / 160);
        FxSmall glow = fx_small(w, h, f, 3, [&](int x, int y, float* o) {
            const uint8_t* s = &src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
            const float k = fx_smooth(0.50f, 0.95f, fx_luma(s) / 255.0f);
            o[0] = s[0] * k; o[1] = s[1] * k; o[2] = s[2] * k;
        });
        FxSmall wide = glow;
        fx_blur(glow, 2, 2);
        fx_blur(wide, braille ? 2 : 5, 3);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                for (int k = 0; k < 3; ++k) {
                    const float base = src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x) + k];
                    const float g = 0.55f * fx_sample(glow, k, x, y) + 0.60f * fx_sample(wide, k, x, y) * (k == 0 ? 1.08f : k == 2 ? 0.92f : 1.0f);   // warmer wide halo
                    q[k] = fx_clamp(255.0f - (255.0f - base) * (255.0f - std::min(255.0f, g)) / 255.0f);   // "screen": never clips harshly
                }
            }
    } else if (fx == 15) {   // film: grain, a faint sepia tone, a flickering gate, a vignette, now and then a scratch and some dust
        const float flick = 1.0f + (fx_rand(tick, 31) - 0.5f) * 0.07f + 0.012f * std::sin(static_cast<float>(t) * 9.0f);
        const int grain = braille ? 1 : std::max(1, w / 320);
        const float amp = braille ? 7.0f : 15.0f;
        const int weave = (fx_rand(tick, 51) < 0.12f) ? ((fx_rand(tick, 52) < 0.5f) ? -1 : 1) * std::max(1, h / 360) : 0;   // the picture jumps a little in the gate now and then
        const bool scratch = fx_rand(tick / 3, 41) < 0.30f;
        const int sx = static_cast<int>(fx_rand(tick / 3, 42) * w);
        for (int y = 0; y < h; ++y) {
            const float dy = (static_cast<float>(y) + 0.5f) / h * 2.0f - 1.0f;
            const int ys = std::clamp(y + weave, 0, h - 1);
            for (int x = 0; x < w; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) / w * 2.0f - 1.0f;
                const float vig = 1.0f - 0.30f * std::pow(std::fabs(dx), 2.4f) - 0.26f * std::pow(std::fabs(dy), 2.4f);
                const uint8_t* s = &src[static_cast<size_t>(ys) * stride + 3 * static_cast<size_t>(x)];
                const float l = fx_luma(s);
                const float sep[3] = {std::min(255.0f, l * 1.08f + 10.0f), l * 0.95f + 4.0f, l * 0.76f};
                const float n = (fx_rand(tick * 7919U + static_cast<uint32_t>(x / grain), static_cast<uint32_t>(y / grain)) - 0.5f) * 2.0f * amp * (0.55f + 0.9f * (1.0f - l / 255.0f));
                uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                for (int k = 0; k < 3; ++k) q[k] = fx_clamp((s[k] * 0.68f + sep[k] * 0.32f) * flick * std::max(0.45f, vig) + n);
            }
        }
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
        auto wave = [&](int x, int y, float* o) {   // the wave of the gradient look (6) at this point
            const float p = (static_cast<float>(x) / w + static_cast<float>(y) / h) * 0.5f;
            float u = std::fmod(p * 1.25f - static_cast<float>(t) * 0.10f, 1.0f);
            if (u < 0.0f) u += 1.0f;
            const float pos = (1.0f - std::fabs(2.0f * u - 1.0f)) * 2.0f;
            const int i0 = std::min(1, static_cast<int>(pos));
            const float fr = pos - static_cast<float>(i0);
            for (int k = 0; k < 3; ++k) o[k] = (g.c[i0][k] * (1.0f - fr) + g.c[i0 + 1][k] * fr) / 255.0f;
        };
        std::vector<float> lum(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) lum[static_cast<size_t>(y) * w + x] = fx_luma(&src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)]);
        auto L = [&](int x, int y) { return lum[static_cast<size_t>(std::clamp(y, 0, h - 1)) * w + static_cast<size_t>(std::clamp(x, 0, w - 1))]; };
        const int rr = (!braille && w >= 480) ? 2 : 1;   // the Sobel reach grows with the picture, so a wide picture gets lines of a similar look
        std::vector<float> edge(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float gx = (L(x + rr, y - rr) + 2 * L(x + rr, y) + L(x + rr, y + rr)) - (L(x - rr, y - rr) + 2 * L(x - rr, y) + L(x - rr, y + rr));
                const float gy = (L(x - rr, y + rr) + 2 * L(x, y + rr) + L(x + rr, y + rr)) - (L(x - rr, y - rr) + 2 * L(x, y - rr) + L(x + rr, y - rr));
                {   // a floor under the noise and a steeper rise: clean lines on a deeper black (more contrast), not blown out
                    const float m = std::sqrt(gx * gx + gy * gy) / (4.0f * 255.0f) * 3.2f;
                    edge[static_cast<size_t>(y) * w + x] = fx_smooth(0.10f, 0.85f, m);
                }
            }
        const int f = braille ? 1 : std::max(1, w / 200);
        auto edge_col = [&](int x, int y, float* o) {   // the colour of the outline: the picture's colour here, made vivid (19: the wave)
            if (wavy) { wave(x, y, o); return; }
            const uint8_t* s = &src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
            const float mx = std::max({static_cast<float>(s[0]), static_cast<float>(s[1]), static_cast<float>(s[2]), 40.0f});
            const float mean = (s[0] + s[1] + s[2]) / 3.0f / mx;
            for (int k = 0; k < 3; ++k) o[k] = std::clamp((s[k] / mx - mean) * 1.3f + mean * 0.9f + 0.08f, 0.0f, 1.0f);
        };
        FxSmall glow = fx_small(w, h, f, 3, [&](int x, int y, float* o) {
            float c[3]; edge_col(x, y, c);
            const float e = edge[static_cast<size_t>(y) * w + x];
            for (int k = 0; k < 3; ++k) o[k] = c[k] * e;
        });
        fx_blur(glow, braille ? 1 : 2, 2);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float c[3]; edge_col(x, y, c);
                const float e = edge[static_cast<size_t>(y) * w + x];
                const uint8_t* s = &src[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                uint8_t* q = &px[static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x)];
                if (wavy) {   // the picture in grey, tinted a little by the wave, under outlines that glow in the wave's colours
                    const float lum = fx_luma(s) / 255.0f;
                    for (int k = 0; k < 3; ++k) q[k] = fx_clamp(255.0f * (lum * (0.03f + 0.05f * c[k]) + c[k] * e * 1.0f + fx_sample(glow, k, x, y) * 0.75f));   // the grey areas take the wave's colour too
                } else {
                    for (int k = 0; k < 3; ++k) q[k] = fx_clamp(s[k] * 0.03f + 255.0f * (c[k] * e * 1.0f + fx_sample(glow, k, x, y) * 0.7f) + (e > 0.85f ? 20.0f * (e - 0.85f) * 6.0f : 0.0f));
                }
            }
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
}

void VideoPlayer::apply_grade(VideoFrame& f, int contrast, int colors, int chroma) {
    const int w = f.w, h = f.h;
    if (w < 2 || h < 2 || f.rgb.size() < static_cast<size_t>(w) * h * 3) return;
    contrast = std::clamp(contrast, 0, 100);
    colors = std::clamp(colors, 0, 100);
    const size_t n = static_cast<size_t>(w) * h;
    if (contrast > 0 || colors < 100) {
        uint8_t lut[256];
        const double factor = 1.0 + contrast / 100.0 * 1.5;   // the same as the cover's contrast
        for (int i = 0; i < 256; ++i) lut[i] = fx_clamp(static_cast<float>((i - 127.5) * factor + 127.5));
        const float sat = static_cast<float>(colors) / 100.0f;
        for (size_t i = 0; i < n; ++i) {
            uint8_t* p = &f.rgb[i * 3];
            float v[3] = {static_cast<float>(lut[p[0]]), static_cast<float>(lut[p[1]]), static_cast<float>(lut[p[2]])};
            if (colors < 100) {
                const float l = 0.299f * v[0] + 0.587f * v[1] + 0.114f * v[2];
                for (float& c : v) c = l + (c - l) * sat;
            }
            p[0] = fx_clamp(v[0]); p[1] = fx_clamp(v[1]); p[2] = fx_clamp(v[2]);
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
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float dx = (x - cx) / std::max(1.0f, cx), dy = (y - cy) / std::max(1.0f, cy);
                const float r2 = (dx * dx + dy * dy) * 0.5f;
                const float g = 1.0f + k * (0.4f + 1.2f * r2);
                uint8_t* q = &f.rgb[(static_cast<size_t>(y) * w + x) * 3];
                q[0] = fx_clamp(bil(cx + (x - cx) * g, cy + (y - cy) * g, 0));
                q[2] = fx_clamp(bil(cx + (x - cx) / g, cy + (y - cy) / g, 2));
            }
    }
}

void VideoPlayer::apply_look(VideoFrame& f, const VideoLook& look, double t, bool braille, const VideoFrame* prev) {
    apply_grade(f, look.contrast, look.colors, look.chroma);
    if (look.fx > 0) {
        apply_fx(f, look.fx, t, braille, &look.grad, prev);
        // The tube looks came out a little too hard in the blacks: the shadows are lifted a touch (pixel and film less, they were only a bit too hard).
        const int fx = look.fx;
        const float lift = (fx == 1 || fx == 2 || fx == 3 || fx == 4 || fx == 7 || fx == 6 || fx == 8 || fx == 9 || fx == 14) ? 16.0f : (fx == 5 || fx == 15) ? 9.0f : 0.0f;
        if (lift > 0.0f) {
            uint8_t lut[256];
            for (int v = 0; v < 256; ++v) lut[v] = static_cast<uint8_t>(std::lround(lift + v * (255.0f - lift) / 255.0f));
            for (auto& b : f.rgb) b = lut[b];
        }
    }
}

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

const VideoFrame* VideoPlayer::frame_for(double t, int w, int h, bool playing, bool* fresh) {
    if (fresh) *fresh = false;
    if (!has_video() || w < 2 || h < 2) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    bool restart = !running_;
    if (running_ && sharp_ != run_sharp_) restart = true;   // another filter: the decoder starts again
    if (running_ && (w != run_w_ || h != run_h_)) {
        // resized (window, fullscreen, lyrics on / off): the decoder is restarted at the new size only once the size has stayed the
        // same for a moment -- while the window is being dragged the picture on screen is simply scaled
        if (w != pend_w_ || h != pend_h_) { pend_w_ = w; pend_h_ = h; pend_since_ = now; }
        if (now - pend_since_ >= std::chrono::milliseconds(250)) restart = true;
    } else {
        pend_w_ = pend_h_ = 0;
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
        restarted_at_ = now;
        pend_w_ = pend_h_ = 0;
        start(run_from_, true);
    }
    std::lock_guard<std::mutex> lk(m_);
    bool changed = false;
    if ((!cur_valid_ || cur_stale_) && !q_.empty()) { prev_valid_ = false; cur_ = std::move(q_.front()); q_.pop_front(); cur_valid_ = true; cur_stale_ = false; changed = true; }
    if (playing) {
        while (!q_.empty() && q_.front().pts <= t) { if (cur_valid_ && !cur_stale_) { prev_ = std::move(cur_); prev_valid_ = true; } cur_ = std::move(q_.front()); q_.pop_front(); cur_valid_ = true; changed = true; }
        if (changed) cv_.notify_all();
    }
    if (changed) cur_returned_ = false;
    if (fresh) *fresh = changed || (cur_valid_ && !cur_returned_);
    if (cur_valid_) cur_returned_ = true;
    return cur_valid_ ? &cur_ : nullptr;
}

VideoFrame VideoPlayer::resample(const VideoFrame& f, int w, int h) {
    VideoFrame o;
    o.w = w; o.h = h; o.pts = f.pts;
    if (w <= 0 || h <= 0 || f.w <= 0 || f.h <= 0 || f.rgb.size() < static_cast<size_t>(f.w) * f.h * 3) return o;
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

void VideoPlayer::quantize(const VideoFrame& f, std::vector<uint8_t>& hue, std::vector<uint8_t>& level) {
    const size_t n = static_cast<size_t>(f.w) * f.h;
    hue.assign(n, 0);
    level.assign(n, 255);
    if (f.rgb.size() < n * 3) return;
    // ordered dither between the two nearest levels of a channel: the share of the way up to the next level against a Bayer threshold
    auto cube_index = [](int v, float thr) {
        int i = 0;
        while (i < 4 && v >= kCubeLevel[i + 1]) ++i;                                   // kCubeLevel[i] <= v < kCubeLevel[i + 1]  (v == 255 -> i = 4)
        const float fr = static_cast<float>(v - kCubeLevel[i]) / static_cast<float>(kCubeLevel[i + 1] - kCubeLevel[i]);
        return fr > thr ? i + 1 : i;
    };
    for (int y = 0; y < f.h; ++y) {
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
            const float thr = (static_cast<float>(kBayer4[y & 3][x & 3]) + 0.5f) / 16.0f;
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
