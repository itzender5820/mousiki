// Colours for pictures that mix the disk's gradient with the cover's own colours (Settings -> Cover Colors): the music player's
// Cover behind Disk looks and the audio book cover with the gradient wave. A GfxFrame (terminal_gfx.h) draws `level` x pal[hue];
// these turn an RGB colour into that form.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace muisc {

// Cover Colors > 0: the colours are no longer one gradient colour per row, so the palette holds hue x saturation at full brightness
// (entry 0 white, then 6 saturations x 36 hues) and `level` carries the brightness of every pixel.
inline std::array<std::array<uint8_t, 3>, 256> hue_sat_palette() {
    std::array<std::array<uint8_t, 3>, 256> pal{};
    pal[0] = {255, 255, 255};
    for (int si = 1; si <= 6; ++si)
        for (int hi = 0; hi < 36; ++hi) {
            const double hh = hi * 10.0 / 60.0, sat = si / 6.0;
            const double c = sat, xx = c * (1.0 - std::fabs(std::fmod(hh, 2.0) - 1.0)), m = 1.0 - c;
            double r = 0, g = 0, b = 0;
            if (hh < 1) { r = c; g = xx; } else if (hh < 2) { r = xx; g = c; } else if (hh < 3) { g = c; b = xx; }
            else if (hh < 4) { g = xx; b = c; } else if (hh < 5) { r = xx; b = c; } else { r = c; b = xx; }
            pal[static_cast<size_t>(1 + (si - 1) * 36 + hi)] = {static_cast<uint8_t>(std::lround((r + m) * 255)), static_cast<uint8_t>(std::lround((g + m) * 255)),
                                                                 static_cast<uint8_t>(std::lround((b + m) * 255))};
        }
    return pal;
}
// A colour as (palette entry of hue_sat_palette(), brightness 0..255).
inline void hue_sat_encode(float r, float g, float b, uint8_t& idx, float& level) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    level = mx;
    idx = 0;
    if (mx < 1.f) return;
    const float sat = (mx - mn) / mx;
    const int si = static_cast<int>(std::lround(sat * 6.f));
    if (si == 0) return;
    float h;
    const float d = std::max(1e-3f, mx - mn);
    if (mx == r) h = 60.f * std::fmod((g - b) / d, 6.f);
    else if (mx == g) h = 60.f * ((b - r) / d + 2.f);
    else h = 60.f * ((r - g) / d + 4.f);
    if (h < 0) h += 360.f;
    const int hi = static_cast<int>(std::lround(h / 10.f)) % 36;
    idx = static_cast<uint8_t>(1 + (si - 1) * 36 + hi);
}
// The colour of a dot: the disk's gradient colour `base` at brightness `v`, mixed with the cover's own colour (cr, cg, cb) brought to
// the same brightness, by `t` (0 = gradient only, 1 = the cover's colour).
inline std::array<float, 3> mix_colour(const std::array<uint8_t, 3>& base, float v, float cr, float cg, float cb, float t) {
    const float lum = std::max(8.f, 0.299f * cr + 0.587f * cg + 0.114f * cb);
    const float k = v / lum;
    std::array<float, 3> out;
    const float c[3] = {cr * k, cg * k, cb * k};
    for (int i = 0; i < 3; ++i) out[static_cast<size_t>(i)] = std::clamp(base[static_cast<size_t>(i)] * (v / 255.f) * (1.f - t) + std::min(255.f, c[i]) * t, 0.f, 255.f);
    return out;
}

// The colour at place u (0..1) of the disk's gradient from (sr, sg, sb) to (er, eg, eb), for the cover looks. A plain blend of the two
// RGB colours loses colour on the way: halfway between a purple and a teal comes out a dull grey-blue. So a blended colour gets
// `strength` (0 = the plain blend, 0.6 = the default, 1 = all of it) of the saturation (HSV) it lacks compared with a saturation that goes from one end's to the other's in a straight line; the
// hue stays the blend's, and the brightness as the eye sees it (luma) stays the blend's as far as the colour can be brightened
// (the largest channel stops at 255). The two ends are exactly the colours that were chosen, and a colour without a hue (grey) is left
// as it is. The picture itself (the grey levels of the cover) is not touched, only the colour it is drawn in.
inline std::array<uint8_t, 3> gradient_step(int sr, int sg, int sb, int er, int eg, int eb, double u, double strength = 0.6) {
    const double c[3] = {sr + (er - sr) * u, sg + (eg - sg) * u, sb + (eb - sb) * u};
    const auto sat = [](double r, double g, double b) { const double mx = std::max({r, g, b}), mn = std::min({r, g, b}); return mx > 0 ? (mx - mn) / mx : 0.0; };
    const auto luma = [](const double* v) { return 0.299 * v[0] + 0.587 * v[1] + 0.114 * v[2]; };
    const double mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]});
    const double want = sat(sr, sg, sb) + (sat(er, eg, eb) - sat(sr, sg, sb)) * u, have = mx > 0 ? (mx - mn) / mx : 0.0;
    double out[3] = {c[0], c[1], c[2]};
    if (strength > 0.0 && mx - mn > 0.5 && want - have > 1e-4) {   // (strength 0: the plain blend, untouched)
        const double target = have + (want - have) * std::clamp(strength, 0.0, 1.0);
        const double k = (mx - mx * (1.0 - target)) / (mx - mn);   // stretch the channels away from the brightest one
        for (double& v : out) v = mx - (mx - v) * k;
        const double ly = luma(out);
        if (ly > 1.0) {
            const double scale = std::min(luma(c) / ly, 255.0 / std::max({out[0], out[1], out[2]}));
            if (scale > 1.0) for (double& v : out) v *= scale;
        }
    }
    const auto q = [](double v) { return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L)); };
    return {q(out[0]), q(out[1]), q(out[2])};
}

// Sixel draws a picture that is not `direct` with only 16 colours (blocks of 16 palette entries) x 15 brightness steps: right for
// a gradient, wrong for a cover's own colours. So with Sixel every pixel's colour, brightness included, comes from this fixed
// palette (6 x 8 x 5 steps of red, green, blue = 240 colours) and the picture is drawn `direct` (level = 255 for every lit pixel).
inline std::array<std::array<uint8_t, 3>, 256> cube_palette() {
    std::array<std::array<uint8_t, 3>, 256> pal{};
    for (int r = 0; r < 6; ++r)
        for (int g = 0; g < 8; ++g)
            for (int b = 0; b < 5; ++b)
                pal[static_cast<size_t>(r * 40 + g * 5 + b)] = {static_cast<uint8_t>(r * 255 / 5), static_cast<uint8_t>(g * 255 / 7), static_cast<uint8_t>(b * 255 / 4)};
    return pal;
}
// The cube_palette() entry for a colour at pixel (x, y), with ordered dithering (no colour bands). 0 = black.
inline int cube_index(float r, float g, float b, int x, int y) {
    static const float kBayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const float t = (kBayer4[y & 3][x & 3] + 0.5f) / 16.f - 0.5f;
    auto q = [&](float v, int n) { return std::clamp(static_cast<int>(std::lround(v / 255.f * (n - 1) + t)), 0, n - 1); };
    return q(r, 6) * 40 + q(g, 8) * 5 + q(b, 5);
}

} // namespace muisc
