// The retro looks of the video picture that are written as functions of their own (included by video_player.cpp, inside its anonymous namespace,
// after the helpers they use: fx_rand, fx_clamp, par_rows, FxSmall ...). Every function reads the untouched frame (`src`) and writes `px`; the
// rows are shared between threads where the work is per pixel, and nothing depends on the order the rows are done in.
//   reworked: CRT (2), VHS (3), Pixel (5), Film (15), Interlace (17)
//   new:      ZX Spectrum (29), Amstrad CPC (30), Teletext (31), Amiga HAM (32), Silent Film (33), Technicolor (34), Vector (35),
//             ASCII (37), Thermal (38), Risograph (39), LaserDisc (40), Halftone CMYK (41), Halftone CMYK High-Res (42)

struct FxCtx {
    int w = 0, h = 0;
    size_t stride = 0;
    const uint8_t* src = nullptr;
    uint8_t* px = nullptr;
    double t = 0.0;
    uint32_t tick = 0;
    bool braille = false;
    int dither = 0;
    const VideoFrame* prev = nullptr;
    int period = 2;          // rows of one scanline
    bool own_lines = true;   // false: another Video Base Layer replaces the scanlines
    const uint8_t* at(int x, int y) const { return src + static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x); }
    uint8_t* out(int x, int y) const { return px + static_cast<size_t>(y) * stride + 3 * static_cast<size_t>(x); }
    float beam(int y, float depth) const {   // 1 on the bright row(s) of a line, 1 - depth in the gap (a raised cosine)
        if (!own_lines) return 1.0f;
        const float ph = static_cast<float>(y % period) / static_cast<float>(period);
        return 1.0f - depth * (0.5f - 0.5f * std::cos(ph * 6.2831853f));
    }
    int gp() const { return braille ? 1 : std::max(1, static_cast<int>(std::lround(w / 640.0))); }   // one "pixel" of a 640 wide picture
};

inline float fx_lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float fx_smoothstep(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }
// NTSC colour: Y (brightness) and I / Q (the colour carried at a much lower bandwidth on tape and in a composite signal)
inline void fx_to_yiq(float r, float g, float b, float& y, float& i, float& q) {
    y = 0.299f * r + 0.587f * g + 0.114f * b;
    i = 0.596f * r - 0.274f * g - 0.322f * b;
    q = 0.211f * r - 0.523f * g + 0.312f * b;
}
inline void fx_from_yiq(float y, float i, float q, uint8_t* o) {
    o[0] = fx_clamp(y + 0.956f * i + 0.621f * q);
    o[1] = fx_clamp(y - 0.272f * i - 0.647f * q);
    o[2] = fx_clamp(y - 1.106f * i + 1.703f * q);
}
// A box blur of radius r along a row (the edges count only the pixels that are there); `pre` is scratch space of n + 1 floats.
inline void fx_box_row(const float* in, float* out, float* pre, int n, int r) {
    pre[0] = 0.0f;
    for (int i = 0; i < n; ++i) pre[i + 1] = pre[i] + in[i];
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    for (int x = 0; x < n; ++x) {
        const int a = std::max(0, x - r), b = std::min(n, x + r + 1);
        out[x] = b - a == 2 * r + 1 ? (pre[b] - pre[a]) * inv : (pre[b] - pre[a]) / static_cast<float>(b - a);   // (the multiplication where the window is whole)
    }
}
// The colour at a (float) position of an 8 bit picture, bilinear, edges repeat.
inline void fx_bilinear(const uint8_t* img, int w, int h, float x, float y, float* o) {
    x = std::clamp(x, 0.0f, static_cast<float>(w - 1)); y = std::clamp(y, 0.0f, static_cast<float>(h - 1));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y), x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
    const float tx = x - static_cast<float>(x0), ty = y - static_cast<float>(y0);
    const size_t st = static_cast<size_t>(w) * 3;
    const uint8_t* a = img + static_cast<size_t>(y0) * st + 3 * static_cast<size_t>(x0);
    const uint8_t* b = img + static_cast<size_t>(y0) * st + 3 * static_cast<size_t>(x1);
    const uint8_t* c = img + static_cast<size_t>(y1) * st + 3 * static_cast<size_t>(x0);
    const uint8_t* d = img + static_cast<size_t>(y1) * st + 3 * static_cast<size_t>(x1);
    for (int k = 0; k < 3; ++k) o[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
}
// One channel (k) of the same, for the looks that move the colours separately.
inline float fx_bilinear1(const uint8_t* img, int w, int h, float x, float y, int k) {
    x = std::clamp(x, 0.0f, static_cast<float>(w - 1)); y = std::clamp(y, 0.0f, static_cast<float>(h - 1));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y), x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
    const float tx = x - static_cast<float>(x0), ty = y - static_cast<float>(y0);
    const size_t st = static_cast<size_t>(w) * 3;
    const uint8_t* r0 = img + static_cast<size_t>(y0) * st + k;
    const uint8_t* r1 = img + static_cast<size_t>(y1) * st + k;
    return (r0[3 * x0] * (1 - tx) + r0[3 * x1] * tx) * (1 - ty) + (r1[3 * x0] * (1 - tx) + r1[3 * x1] * tx) * ty;
}
// x^g for a byte (0 .. 255 in, 0 .. 255 out as float): a table instead of a pow() per pixel. g < 1 lifts the shadows.
struct FxGammaLut {
    float v[256];
    explicit FxGammaLut(float g) { for (int i = 0; i < 256; ++i) v[i] = 255.0f * std::pow(static_cast<float>(i) / 255.0f, g); }
    float at(float x) const { const int i = std::clamp(static_cast<int>(x + 0.5f), 0, 255); return v[i]; }
};
// A noise value of the triangular kind (-1 .. 1, mostly near 0): two uniform ones added, like the grain of a film.
inline float fx_tri(uint32_t a, uint32_t b) { return fx_rand(a, b) + fx_rand(a ^ 0x68E31DA4U, b + 977U) - 1.0f; }

// ------------------------------------------------------------------ CRT
// A tube: the beam's scanlines and phosphor stripes, a little horizontal softness, a tube gamma, bloom around bright parts, a gentle curve of
// the glass (the picture is a touch bulged, the corners stay where they are), red and blue that miss each other a little towards the edges
// (convergence), a vignette and the faint flicker of the supply.
void fx_crt(const FxCtx& c) {
    const int w = c.w, h = c.h;
    const bool braille = c.braille;
    const float depth = braille ? 0.30f : 0.40f;
    const bool mask = !braille && w >= 160 && c.own_lines;   // the RGB stripes need pixels to be seen
    // the tube's gamma as a table over v * 4 (v = 0 .. 255): a pow() per colour would cost more than the rest
    static const std::vector<float> kTube = [] { std::vector<float> t(1024); for (size_t i = 0; i < t.size(); ++i) t[i] = 255.0f * std::pow(std::min(1.0f, static_cast<float>(i) / 1020.0f), 1.08f); return t; }();
    const float flick = 1.0f + 0.012f * std::sin(static_cast<float>(c.t) * 47.0f) + 0.008f * (fx_rand(c.tick, 3) - 0.5f);
    const float kc = braille ? 0.0f : 0.035f, zoom = 1.0f / (1.0f + 2.0f * kc);   // the curve: edges are pulled in, the corners keep their place
    const float cvm = braille ? 0.0f : 0.0016f * static_cast<float>(w);           // the convergence error at the edge, in pixels
    std::vector<float> dxv(static_cast<size_t>(w)), dx2(static_cast<size_t>(w)), vx(static_cast<size_t>(w)), vy(static_cast<size_t>(h));
    for (int x = 0; x < w; ++x) {
        const float dx = (static_cast<float>(x) + 0.5f) / w * 2.0f - 1.0f;
        dxv[static_cast<size_t>(x)] = dx; dx2[static_cast<size_t>(x)] = dx * dx;
        vx[static_cast<size_t>(x)] = std::pow(std::fabs(dx), 2.6f);
    }
    for (int y = 0; y < h; ++y) vy[static_cast<size_t>(y)] = std::pow(std::fabs((static_cast<float>(y) + 0.5f) / h * 2.0f - 1.0f), 2.6f);
    // the beam is not razor sharp: a 1-6-1 softening along the line; red and blue are taken from a little to the side (the convergence error,
    // growing towards the edges, in sixteenths of a pixel), so the three guns miss each other
    std::vector<uint8_t> soft(c.stride * static_cast<size_t>(h));
    std::vector<int> pr(static_cast<size_t>(w)), pb(static_cast<size_t>(w));   // where red / blue come from: x * 16 + shift
    for (int x = 0; x < w; ++x) {
        const int sh = static_cast<int>(std::lround(dxv[static_cast<size_t>(x)] * cvm * 16.0f));
        pr[static_cast<size_t>(x)] = std::clamp(x * 16 - sh, 0, (w - 1) * 16);
        pb[static_cast<size_t>(x)] = std::clamp(x * 16 + sh, 0, (w - 1) * 16);
    }
    par_rows(h, [&](int y0, int y1) {
        std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
        for (int y = y0; y < y1; ++y) {
            const uint8_t* r = c.at(0, y);
            uint8_t* o = &soft[static_cast<size_t>(y) * c.stride];
            for (int x = 0; x < w; ++x) {
                const int xl = std::max(0, x - 1), xr = std::min(w - 1, x + 1);
                for (int k = 0; k < 3; ++k) row[3 * static_cast<size_t>(x) + k] = static_cast<uint8_t>((r[3 * xl + k] + 6 * r[3 * x + k] + r[3 * xr + k] + 4) >> 3);
            }
            if (cvm <= 0.0f) { std::copy(row.begin(), row.end(), o); continue; }
            for (int x = 0; x < w; ++x) {
                o[3 * x + 1] = row[3 * static_cast<size_t>(x) + 1];
                const int a = pr[static_cast<size_t>(x)], b = pb[static_cast<size_t>(x)];
                const int a0 = a >> 4, a1 = std::min(w - 1, a0 + 1), ta = a & 15, b0 = b >> 4, b1 = std::min(w - 1, b0 + 1), tb = b & 15;
                o[3 * x] = static_cast<uint8_t>((row[3 * static_cast<size_t>(a0)] * (16 - ta) + row[3 * static_cast<size_t>(a1)] * ta + 8) >> 4);
                o[3 * x + 2] = static_cast<uint8_t>((row[3 * static_cast<size_t>(b0) + 2] * (16 - tb) + row[3 * static_cast<size_t>(b1) + 2] * tb + 8) >> 4);
            }
        }
    });
    const int bf = braille ? 1 : std::max(1, w / 160);   // bloom: the bright parts scatter light over their surroundings
    FxSmall bloom = fx_small(w, h, bf, 3, [&](int x, int y, float* o) {
        const uint8_t* s = c.at(x, y);
        for (int k = 0; k < 3; ++k) o[k] = std::max(0.0f, static_cast<float>(s[k]) - 150.0f) / 105.0f;
    });
    fx_blur(bloom, braille ? 1 : 2, 2);
    const FxUp up(bloom, w, h);
    static const float kMask[3][3] = {{1.08f, 0.86f, 0.86f}, {0.86f, 1.08f, 0.86f}, {0.86f, 0.86f, 1.08f}};   // [x % 3][colour]: the stripe of this colour is lit
    par_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float dy = (static_cast<float>(y) + 0.5f) / h * 2.0f - 1.0f;
            const float dy2 = dy * dy;
            const float line = c.beam(y, depth) * (1.0f + depth * 0.30f);
            const float vyy = 0.30f * vy[static_cast<size_t>(y)];
            uint8_t* orow = c.out(0, y);
            int xm = 0;
            for (int x = 0; x < w; ++x, xm = xm == 2 ? 0 : xm + 1) {
                float ch[3];
                if (kc > 0.0f) {
                    const float dx = dxv[static_cast<size_t>(x)];
                    const float sc = (1.0f + kc * (dx2[static_cast<size_t>(x)] + dy2)) * zoom;
                    const float sx = std::clamp((dx * sc + 1.0f) * 0.5f * w - 0.5f, 0.0f, static_cast<float>(w - 1));
                    const float sy = std::clamp((dy * sc + 1.0f) * 0.5f * h - 0.5f, 0.0f, static_cast<float>(h - 1));
                    const int x0 = static_cast<int>(sx), y0i = static_cast<int>(sy), x1 = std::min(w - 1, x0 + 1), y1i = std::min(h - 1, y0i + 1);
                    const float tx = sx - static_cast<float>(x0), ty = sy - static_cast<float>(y0i);
                    const uint8_t* a = &soft[static_cast<size_t>(y0i) * c.stride + 3 * static_cast<size_t>(x0)];
                    const uint8_t* b = &soft[static_cast<size_t>(y0i) * c.stride + 3 * static_cast<size_t>(x1)];
                    const uint8_t* cc = &soft[static_cast<size_t>(y1i) * c.stride + 3 * static_cast<size_t>(x0)];
                    const uint8_t* d = &soft[static_cast<size_t>(y1i) * c.stride + 3 * static_cast<size_t>(x1)];
                    const float w00 = (1 - tx) * (1 - ty), w01 = tx * (1 - ty), w10 = (1 - tx) * ty, w11 = tx * ty;
                    for (int k = 0; k < 3; ++k) ch[k] = a[k] * w00 + b[k] * w01 + cc[k] * w10 + d[k] * w11;
                } else {
                    const uint8_t* s = &soft[static_cast<size_t>(y) * c.stride + 3 * static_cast<size_t>(x)];
                    for (int k = 0; k < 3; ++k) ch[k] = s[k];
                }
                const float vig = std::max(0.35f, 1.0f - 0.34f * vx[static_cast<size_t>(x)] - vyy - 0.10f * dx2[static_cast<size_t>(x)] * dy2) * flick;
                float bl[3];
                up.sample3(x, y, bl);
                uint8_t* o = orow + 3 * static_cast<size_t>(x);
                for (int k = 0; k < 3; ++k) {
                    float v = ch[k];
                    if (mask) v *= kMask[xm][k];
                    v = kTube[static_cast<size_t>(std::min(v, 255.0f) * 4.0f)];
                    o[k] = fx_clamp((v * line + bl[k] * 255.0f * 0.30f) * vig);
                }
            }
        }
    });
}

// ------------------------------------------------------------------ VHS
// A tape: the brightness has a low bandwidth (soft, with a halo of ringing at edges), the colour a much lower one (it is smeared to the right and
// arrives late), the last lines tear (head switching), the noise comes in short horizontal streaks, now and then a white drop-out, and the colours
// are a little paler. The lines sway only a little (well under a pixel of a 640 wide picture, slowly): a tape that tracks well is steady. Now and
// then the tracking slips (vhs_tracking: in about one stretch of 8 seconds in three, for 1 to 3 seconds): then the lines wobble strongly and a band
// of noise rolls up the picture, as a worn tape does.
float vhs_tracking(float t) {
    constexpr float kSlot = 8.0f;
    const float k = std::floor(std::max(0.0f, t) / kSlot);
    const uint32_t ks = static_cast<uint32_t>(k);
    if (fx_rand(ks, 4242U) > 0.35f) return 0.0f;
    const float start = k * kSlot + fx_rand(ks, 4243U) * 4.5f, len = 1.2f + fx_rand(ks, 4244U) * 1.8f;
    const float p = (t - start) / len;
    if (p <= 0.0f || p >= 1.0f) return 0.0f;
    return fx_smoothstep(p * 4.0f) * fx_smoothstep((1.0f - p) * 4.0f);   // (it comes and goes within a quarter of its time)
}

void fx_vhs(const FxCtx& c) {
    const int w = c.w, h = c.h;
    const bool braille = c.braille;
    const float t = static_cast<float>(c.t);
    const float trk = vhs_tracking(t);                       // 0 = the tape tracks well, up to 1 = it slips
    const float band_y = std::fmod(t * 0.11f, 1.0f) * h;   // the tracking band rolls up the picture
    const float band_h = std::max(3.0f, h * 0.07f);
    const int rc = braille ? 1 : std::max(2, w / 56);       // the colour's smear
    const int cd = braille ? 1 : std::max(1, w / 120);      // the colour is late
    const int rr = braille ? 1 : std::max(1, w / 170);      // the width of the ringing
    const int sc = braille ? 1 : std::max(1, w / 320);
    const int hs = std::max(2, h / 24);                      // the head-switching lines at the bottom
    const float scale = std::max(0.6f, w / 320.0f);
    par_rows(h, [&](int y0, int y1) {
        std::vector<float> Y(static_cast<size_t>(w)), I(static_cast<size_t>(w)), Q(static_cast<size_t>(w)), Yb(static_cast<size_t>(w)), Ib(static_cast<size_t>(w)), Qb(static_cast<size_t>(w)), pre(static_cast<size_t>(w) + 1);
        std::vector<float> nY(static_cast<size_t>(w) + 1), nI(static_cast<size_t>(w) / 3 + 2), nQ(static_cast<size_t>(w) / 3 + 2);
        for (int y = y0; y < y1; ++y) {
            // the steady sway (a slow bend of a fraction of a pixel), and while the tracking slips the old strong wobble on top
            float wob = (0.16f * std::sin(static_cast<float>(y) * 0.031f + t * 0.9f) + 0.10f * std::sin(static_cast<float>(y) * 0.0071f - t * 0.37f)) * scale;
            if (trk > 0.0f) wob += trk * (0.7f * std::sin(static_cast<float>(y) * 0.19f + t * 2.3f) + 0.5f * std::sin(static_cast<float>(y) * 0.043f - t * 1.1f)) * scale;
            float noise = 4.0f, bandk = 0.0f;
            const float dist = std::fabs(static_cast<float>(y) - band_y);
            if (trk > 0.0f && dist < band_h) {   // inside the band (only while the tracking slips): a bigger slip and more noise
                bandk = (1.0f - dist / band_h) * trk;
                wob += (fx_rand(c.tick, static_cast<uint32_t>(y)) - 0.35f) * 0.035f * w * bandk;
                noise += 22.0f * bandk;
            }
            float tear = 0.0f;
            if (y > h - hs) {   // head-switching noise: the last lines tear to the side
                tear = static_cast<float>(y - (h - hs)) / static_cast<float>(hs);
                wob += (fx_rand(static_cast<uint32_t>(y), 11) - 0.2f) * 0.05f * w * (0.4f + 0.6f * tear);
                noise += 14.0f;
            }
            // the line taken from `wob` pixels to the left, between two pixels where that is a fraction (so the slow sway is smooth, not steps of a pixel)
            const float wf = std::floor(wob), fr = wob - wf;
            const int dx = static_cast<int>(wf);
            const uint8_t* s = c.at(0, y);
            for (int x = 0; x < w; ++x) {
                const int xa = std::clamp(x - dx, 0, w - 1), xb = std::clamp(x - dx - 1, 0, w - 1);   // (x - wob lies between x - dx - 1 and x - dx)
                const float r = s[3 * xa] + (s[3 * xb] - s[3 * xa]) * fr, g = s[3 * xa + 1] + (s[3 * xb + 1] - s[3 * xa + 1]) * fr, b = s[3 * xa + 2] + (s[3 * xb + 2] - s[3 * xa + 2]) * fr;
                fx_to_yiq(r, g, b, Y[static_cast<size_t>(x)], I[static_cast<size_t>(x)], Q[static_cast<size_t>(x)]);
            }
            fx_box_row(Y.data(), Yb.data(), pre.data(), w, 1);                       // the luma's own softness
            for (int x = 0; x < w; ++x) Y[static_cast<size_t>(x)] = Yb[static_cast<size_t>(x)];
            fx_box_row(Y.data(), Yb.data(), pre.data(), w, rr + 1);                  // the ringing: the brightness pushed away from its surroundings
            fx_box_row(I.data(), Ib.data(), pre.data(), w, rc + static_cast<int>(tear * static_cast<float>(rc)));
            fx_box_row(Q.data(), Qb.data(), pre.data(), w, rc + static_cast<int>(tear * static_cast<float>(rc)));
            const float line = c.beam(y, braille ? 0.10f : 0.14f);
            const bool drop = !braille && fx_rand(c.tick, static_cast<uint32_t>(y) + 7001U) < 0.0010f;   // a drop-out: a short white dash
            const int dx0 = static_cast<int>(fx_rand(c.tick, static_cast<uint32_t>(y) + 7002U) * w), dlen = (6 + static_cast<int>(fx_rand(c.tick, static_cast<uint32_t>(y) + 7003U) * 30.0f)) * sc;
            uint8_t* o = c.out(0, y);
            // the noise is a short streak along the line (2 * sc pixels for the brightness, three times that for the colour): drawn once per streak
            const int cw = 2 * sc, ncell = (w + cw - 1) / cw;
            for (int g = 0; g < ncell; ++g) {
                nY[static_cast<size_t>(g)] = fx_rand(c.tick * 7919U + static_cast<uint32_t>(g), static_cast<uint32_t>(y)) - 0.5f;
                if (g % 3 == 0) {
                    nI[static_cast<size_t>(g / 3)] = fx_rand(c.tick * 131U + static_cast<uint32_t>(g / 3), static_cast<uint32_t>(y) + 90000U) - 0.5f;
                    nQ[static_cast<size_t>(g / 3)] = fx_rand(c.tick * 137U + static_cast<uint32_t>(g / 3), static_cast<uint32_t>(y) + 91000U) - 0.5f;
                }
            }
            int nx = 0, run = 0;
            for (int x = 0; x < w; ++x) {
                const size_t xi = static_cast<size_t>(x);
                float yy = Y[xi] + 0.55f * (Y[xi] - Yb[xi]);
                const size_t xc = static_cast<size_t>(std::max(0, x - cd));
                float ii = Ib[xc] * 0.86f, qq = Qb[xc] * 0.86f;   // paler, smeared, late
                yy += nY[static_cast<size_t>(nx)] * 2.0f * noise;
                ii += nI[static_cast<size_t>(nx / 3)] * 14.0f * (0.4f + bandk);
                qq += nQ[static_cast<size_t>(nx / 3)] * 14.0f * (0.4f + bandk);
                if (++run == cw) { run = 0; ++nx; }
                if (bandk > 0.0f) { yy += 26.0f * bandk * fx_rand(c.tick + static_cast<uint32_t>(x / sc), static_cast<uint32_t>(y)); ii *= 1.0f - 0.5f * bandk; qq *= 1.0f - 0.5f * bandk; }
                if (drop && x >= dx0 && x < dx0 + dlen) { yy = std::min(255.0f, yy + 120.0f); ii *= 0.2f; qq *= 0.2f; }
                fx_from_yiq(yy * line, ii * line, qq * line, o + 3 * x);
            }
        }
    });
}

// ------------------------------------------------------------------ Film and Silent Film
// Film (a print of the 35 mm era): grain that is stronger in the shadows, the warm tone of the print, red-orange halation around bright parts
// (the light bounces back from the base), a gate that weaves and flickers, a vignette, scratches that last half a second, dust and now and then
// a hair. Silent film (33) is the older cousin: sepia, a hard contrast curve, an iris-like dark rim, soft focus, rough grain, strong flicker,
// a crank that jumps, many scratches.
void fx_film(const FxCtx& c, bool silent) {
    const int w = c.w, h = c.h;
    const bool braille = c.braille;
    const uint32_t tick = c.tick;
    const float t = static_cast<float>(c.t);
    const float flick = silent ? 1.0f + (fx_rand(tick, 31) - 0.5f) * 0.16f + 0.035f * std::sin(t * 7.0f) : 1.0f + (fx_rand(tick, 31) - 0.5f) * 0.07f + 0.012f * std::sin(t * 9.0f);
    const int grain = braille ? 1 : std::max(1, static_cast<int>(std::lround(w / (silent ? 260.0 : 330.0))));
    const float amp = braille ? 7.0f : silent ? 24.0f : 14.0f;
    const int wa = std::max(1, h / 360);
    const bool jump = fx_rand(tick, 51) < (silent ? 0.20f : 0.12f);
    const int wx = jump ? static_cast<int>(std::lround((fx_rand(tick, 53) - 0.5f) * 2.0f * static_cast<float>(wa))) : 0;
    int wy = jump ? ((fx_rand(tick, 52) < 0.5f) ? -1 : 1) * wa : 0;
    if (silent && fx_rand(tick, 54) < 0.025f) wy += static_cast<int>(h * 0.025f);   // the crank slips: the picture drops by a few lines
    std::vector<float> vx(static_cast<size_t>(w)), vy(static_cast<size_t>(h));
    for (int x = 0; x < w; ++x) vx[static_cast<size_t>(x)] = std::pow(std::fabs((static_cast<float>(x) + 0.5f) / w * 2.0f - 1.0f), 2.4f);
    for (int y = 0; y < h; ++y) vy[static_cast<size_t>(y)] = std::pow(std::fabs((static_cast<float>(y) + 0.5f) / h * 2.0f - 1.0f), 2.4f);
    // the tone curve of the print (a gentle S; the silent film's is harder), as a table
    float curve[256];
    for (int i = 0; i < 256; ++i) { const float x = static_cast<float>(i) / 255.0f; curve[i] = 255.0f * fx_lerp(x, fx_smoothstep(x), silent ? 0.65f : 0.32f); }
    static const FxGammaLut kSilentGamma(0.8f);
    FxSmall halo = fx_small(w, h, braille ? 1 : std::max(2, w / 90), 1, [&](int x, int y, float* o) { o[0] = std::max(0.0f, fx_luma(c.at(x, y)) / 255.0f - 0.62f) / 0.38f; });
    fx_blur(halo, braille ? 1 : 2, 3);
    FxSmall soft;   // the silent film's soft focus
    if (silent) { soft = fx_small(w, h, braille ? 1 : 2, 1, [&](int x, int y, float* o) { o[0] = fx_luma(c.at(x, y)); }); fx_blur(soft, braille ? 1 : 2, 2); }
    const FxUp upHalo(halo, w, h), upSoft(silent ? soft : halo, w, h);   // (upSoft is only read by the silent film)
    std::vector<int> gxv(static_cast<size_t>(w));    // the grain cell of every column
    std::vector<float> dxs(static_cast<size_t>(w));   // (the iris) x of every column, squared
    for (int x = 0; x < w; ++x) {
        gxv[static_cast<size_t>(x)] = x / grain;
        const float dx = (static_cast<float>(x) + 0.5f) / w * 2.0f - 1.0f;
        dxs[static_cast<size_t>(x)] = dx * dx * 0.8f;
    }
    const int cells = (w + grain - 1) / grain;
    par_rows(h, [&](int y0, int y1) {
        // the grain is the same over a cell of grain x grain pixels: its random numbers are drawn once per cell, not per pixel
        std::vector<float> nt(static_cast<size_t>(cells)), nc(static_cast<size_t>(cells) * 3);
        int cached = -1;
        for (int y = y0; y < y1; ++y) {
            const int ys = std::clamp(y + wy, 0, h - 1);
            const int gy = y / grain;
            if (gy != cached) {
                cached = gy;
                for (int g = 0; g < cells; ++g) {
                    nt[static_cast<size_t>(g)] = fx_tri(tick * 7919U + static_cast<uint32_t>(g), static_cast<uint32_t>(gy));
                    if (!silent)
                        for (int k = 0; k < 3; ++k)
                            nc[static_cast<size_t>(g) * 3 + static_cast<size_t>(k)] = (fx_rand(tick * 31U + static_cast<uint32_t>(g), static_cast<uint32_t>(gy) + 5000U + static_cast<uint32_t>(k)) - 0.5f) * amp * 0.30f;
                }
            }
            const float dy = (static_cast<float>(y) + 0.5f) / h * 2.0f - 1.0f, dy2 = dy * dy;
            const float vyy = 0.26f * vy[static_cast<size_t>(y)];
            uint8_t* q = c.out(0, y);
            for (int x = 0; x < w; ++x, q += 3) {
                const int xs = std::clamp(x + wx, 0, w - 1);
                const uint8_t* s = c.at(xs, ys);
                const float hl = upHalo.sample1(x, y);
                float rgb[3];
                float l;
                if (silent) {
                    l = fx_lerp(fx_luma(s), upSoft.sample1(xs, ys), 0.38f);
                    l = curve[std::clamp(static_cast<int>(kSilentGamma.at(l) + 0.5f), 0, 255)] / 255.0f;   // (the gamma lifts the shadows a little: it is dark in the iris)
                    l = std::clamp(l * 1.06f + hl * 0.20f, 0.0f, 1.0f);
                    // sepia: dark brown - warm mid - cream, through the picture's brightness
                    static const float sh[3] = {26.0f, 15.0f, 9.0f}, mid[3] = {158.0f, 118.0f, 74.0f}, hi[3] = {255.0f, 240.0f, 208.0f};
                    for (int k = 0; k < 3; ++k) rgb[k] = l < 0.5f ? fx_lerp(sh[k], mid[k], l * 2.0f) : fx_lerp(mid[k], hi[k], (l - 0.5f) * 2.0f);
                } else {
                    const float r = curve[s[0]], g = curve[s[1]], b = curve[s[2]];
                    l = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
                    const float sep[3] = {std::min(255.0f, l * 255.0f * 1.08f + 10.0f), l * 255.0f * 0.95f + 4.0f, l * 255.0f * 0.76f};
                    const float warm[3] = {r * 1.03f, g, b * 0.93f};   // the print's warm cast
                    for (int k = 0; k < 3; ++k) rgb[k] = warm[k] * 0.78f + sep[k] * 0.22f;
                    rgb[0] += hl * 255.0f * 0.40f; rgb[1] += hl * 255.0f * 0.13f; rgb[2] += hl * 255.0f * 0.03f;   // halation: red-orange
                }
                float vig;
                if (silent) vig = 1.0f - 0.50f * fx_smoothstep((std::sqrt(dxs[static_cast<size_t>(x)] + dy2) - 0.55f) / 0.62f);   // the iris: a round, soft-edged dark rim
                else vig = 1.0f - 0.30f * vx[static_cast<size_t>(x)] - vyy;
                const size_t g = static_cast<size_t>(gxv[static_cast<size_t>(x)]);
                const float n = nt[g] * amp * (0.5f + 0.95f * (1.0f - l));
                const float vf = flick * std::max(0.4f, vig);
                if (silent) { for (int k = 0; k < 3; ++k) q[k] = fx_clamp(rgb[k] * vf + n); }
                else for (int k = 0; k < 3; ++k) q[k] = fx_clamp(rgb[k] * vf + n + nc[g * 3 + static_cast<size_t>(k)]);
            }
        }
    });
    // scratches: thin, bright, vertical lines that last about half a second (a new one every 10 frames) and waver very little
    const uint32_t seed = tick / 10U;
    for (int k = 0; k < 2; ++k) {
        if (fx_rand(seed, 41U + static_cast<uint32_t>(k)) >= (silent ? 0.62f : 0.36f)) continue;
        const float x0 = fx_rand(seed, 60U + static_cast<uint32_t>(k)) * w, tilt = (fx_rand(seed, 70U + static_cast<uint32_t>(k)) - 0.5f) * 0.012f;
        const float bright = silent ? 0.30f : 0.22f;
        for (int y = 0; y < h; ++y) {
            if (fx_rand(static_cast<uint32_t>(y / 7) + seed * 13U, 80U + static_cast<uint32_t>(k)) < 0.34f) continue;   // it breaks up
            const int x = std::clamp(static_cast<int>(x0 + (static_cast<float>(y) - h * 0.5f) * tilt), 0, w - 1);
            for (int xx = x; xx < std::min(w, x + std::max(1, grain / 2)); ++xx) {
                uint8_t* q = c.out(xx, y);
                for (int j = 0; j < 3; ++j) q[j] = fx_clamp(q[j] * (1.0f - bright) + 255.0f * bright);
            }
        }
    }
    // dust: specks that stay for two frames
    const uint32_t dseed = tick / 2U;
    const int dust = (silent ? 6 : 2) + static_cast<int>(fx_rand(dseed, 44) * (silent ? 12.0f : 6.0f));
    for (int k = 0; k < dust; ++k) {
        const uint32_t sd = dseed * 241U + static_cast<uint32_t>(k);
        const int x = static_cast<int>(fx_rand(sd, 45) * w), y = static_cast<int>(fx_rand(sd, 46) * h), r = 1 + static_cast<int>(fx_rand(sd, 47) * static_cast<float>(std::max(1, w / 320)));
        const bool dark = fx_rand(sd, 48) < 0.6f;
        for (int yy = std::max(0, y - r + 1); yy < std::min(h, y + r); ++yy)
            for (int xx = std::max(0, x - r + 1); xx < std::min(w, x + r); ++xx) {
                uint8_t* q = c.out(xx, yy);
                for (int j = 0; j < 3; ++j) q[j] = dark ? fx_clamp(q[j] * 0.32f) : fx_clamp(q[j] * 0.45f + 140.0f);
            }
    }
    // now and then a hair in the gate: a short dark curve
    if (fx_rand(tick / 6U, 90) < (silent ? 0.16f : 0.08f)) {
        const uint32_t hs = tick / 6U;
        float px0 = fx_rand(hs, 91) * w, py0 = fx_rand(hs, 92) * h;
        const float ang = fx_rand(hs, 93) * 6.2831853f, curl = (fx_rand(hs, 94) - 0.5f) * 0.12f;
        float a = ang;
        const int len = std::max(8, w / 14);
        for (int i = 0; i < len; ++i) {
            const int x = static_cast<int>(px0), y = static_cast<int>(py0);
            if (x >= 0 && x < w && y >= 0 && y < h) { uint8_t* q = c.out(x, y); for (int j = 0; j < 3; ++j) q[j] = fx_clamp(q[j] * 0.3f); }
            a += curl; px0 += std::cos(a); py0 += std::sin(a);
        }
    }
}

// ------------------------------------------------------------------ Interlace
// Video of 50 fields: this frame brings one field (every second line), the other lines are still those of the frame before -- where something moved
// the lines comb. The field that is new changes with every frame. Without a frame before (a still picture, the first frame) the old lines are
// made from their neighbours. The lines are a little dimmer where they are old (the phosphor fades), and the beam leaves its gaps.
void fx_interlace(const FxCtx& c) {
    const int w = c.w, h = c.h;
    const bool pv = c.prev && c.prev->w == w && c.prev->h == h && c.prev->rgb.size() >= c.stride * static_cast<size_t>(h);
    const int field = static_cast<int>(c.tick & 1U);
    par_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const bool old = (y & 1) != field;
            const float fade = old ? 0.94f : 1.0f;
            const float line = (0.985f + 0.015f * fx_rand(c.tick, static_cast<uint32_t>(y >> 1))) * c.beam(y, c.braille ? 0.10f : 0.16f) * fade;   // the lines twitter a little
            uint8_t* o = c.out(0, y);
            if (old && pv) {
                const uint8_t* s = &c.prev->rgb[static_cast<size_t>(y) * c.stride];
                for (int x = 0; x < w * 3; ++x) o[x] = fx_clamp(s[x] * line);
            } else if (old) {
                const uint8_t* a = c.at(0, std::max(0, y - 1));
                const uint8_t* b = c.at(0, std::min(h - 1, y + 1));
                for (int x = 0; x < w * 3; ++x) o[x] = fx_clamp((a[x] + b[x]) * 0.5f * line);
            } else {
                const uint8_t* s = c.at(0, y);
                for (int x = 0; x < w * 3; ++x) o[x] = fx_clamp(s[x] * line);
            }
        }
    });
}

// ------------------------------------------------------------------ Pixel
// A coarse mosaic in a real palette: the 32 colours of the PICO-8 fantasy console (its 16 and the 16 of its extended palette), every block the
// nearest of them, with the chosen dither pattern (Bayer 4x4 unless another one is chosen) deciding between neighbours.
const uint8_t kPico[32][3] = {
    {0, 0, 0}, {29, 43, 83}, {126, 37, 83}, {0, 135, 81}, {171, 82, 54}, {95, 87, 79}, {194, 195, 199}, {255, 241, 232},
    {255, 0, 77}, {255, 163, 0}, {255, 236, 39}, {0, 228, 54}, {41, 173, 255}, {131, 118, 156}, {255, 119, 168}, {255, 204, 170},
    {41, 24, 20}, {17, 29, 53}, {66, 33, 54}, {18, 83, 89}, {116, 47, 41}, {73, 51, 59}, {162, 136, 121}, {243, 239, 125},
    {190, 18, 80}, {255, 108, 36}, {168, 231, 46}, {0, 181, 67}, {6, 90, 181}, {117, 70, 101}, {255, 110, 89}, {255, 157, 129}};

void fx_pixel(const FxCtx& c) {
    const int w = c.w, h = c.h;
    const int bs = c.braille ? 2 : std::clamp(h / 100, 2, 6);   // block edge in pixels (about 100 blocks over the height)
    const int pat = fx_pat(c.dither, 3);
    const int brows = (h + bs - 1) / bs;
    par_rows(brows, [&](int r0, int r1) {
        for (int br = r0; br < r1; ++br) {
            const int by = br * bs;
            for (int bx = 0, bc = 0; bx < w; bx += bs, ++bc) {
                const int x1 = std::min(w, bx + bs), y1 = std::min(h, by + bs);
                float sum[3] = {0, 0, 0};
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x) { const uint8_t* s = c.at(x, y); sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2]; }
                const float n = static_cast<float>((x1 - bx) * (y1 - by));
                const float bias = c.braille ? 0.0f : ((static_cast<float>(fx_dither_rank(pat, bc, br)) + 0.5f) / 1024.0f - 0.5f) * 24.0f;
                float v[3];
                for (int k = 0; k < 3; ++k) v[k] = std::clamp((sum[k] / n - 128.0f) * 1.08f + 128.0f + bias, -20.0f, 275.0f);
                int best = 0;
                float bd = 1e30f;
                for (int i = 0; i < 32; ++i) {
                    const float dr = v[0] - kPico[i][0], dg = v[1] - kPico[i][1], db = v[2] - kPico[i][2];
                    const float dd = 0.30f * dr * dr + 0.59f * dg * dg + 0.11f * db * db;
                    if (dd < bd) { bd = dd; best = i; }
                }
                for (int y = by; y < y1; ++y) {
                    uint8_t* q = c.out(bx, y);
                    for (int x = bx; x < x1; ++x, q += 3) { q[0] = kPico[best][0]; q[1] = kPico[best][1]; q[2] = kPico[best][2]; }
                }
            }
        }
    });
}
