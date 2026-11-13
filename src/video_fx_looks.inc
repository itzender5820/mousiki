// The grading, print and display looks (included by video_player.cpp after video_fx_machines.inc):
//   Technicolor (34), Vector (35), Thermal (38), Risograph (39), LaserDisc (40), Halftone CMYK (41), Halftone CMYK High-Res (42)

// The Sobel edge strength of the picture's brightness, 0 .. 1 (a floor under the noise and a steeper rise), and the gradient itself.
struct FxEdges {
    std::vector<float> lum, gx, gy, mag;
};
FxEdges fx_edges(const FxCtx& c, int reach, float gain) {
    const int w = c.w, h = c.h;
    FxEdges e;
    e.lum.resize(static_cast<size_t>(w) * h);
    e.gx.resize(e.lum.size()); e.gy.resize(e.lum.size()); e.mag.resize(e.lum.size());
    par_rows(h, [&](int y0, int y1) { for (int y = y0; y < y1; ++y) for (int x = 0; x < w; ++x) e.lum[static_cast<size_t>(y) * w + x] = fx_luma(c.at(x, y)); });
    auto L = [&](int x, int y) { return e.lum[static_cast<size_t>(std::clamp(y, 0, h - 1)) * w + static_cast<size_t>(std::clamp(x, 0, w - 1))]; };
    par_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const float gx = (L(x + reach, y - reach) + 2 * L(x + reach, y) + L(x + reach, y + reach)) - (L(x - reach, y - reach) + 2 * L(x - reach, y) + L(x - reach, y + reach));
                const float gy = (L(x - reach, y + reach) + 2 * L(x, y + reach) + L(x + reach, y + reach)) - (L(x - reach, y - reach) + 2 * L(x, y - reach) + L(x + reach, y - reach));
                const size_t i = static_cast<size_t>(y) * w + x;
                e.gx[i] = gx; e.gy[i] = gy;
                e.mag[i] = std::sqrt(gx * gx + gy * gy) / (4.0f * 255.0f) * gain;
            }
    });
    return e;
}

// ------------------------------------------------------------------ Technicolor
// The three-strip look: saturated, clean primaries (the cross-talk between the colours is taken out), rich contrast, warm highlights over cool
// shadows, a soft diffusion glow, a little grain and the faint colour fringe of three strips that do not register perfectly.
void fx_technicolor(const FxCtx& c) {
    const int w = c.w, h = c.h;
    const bool braille = c.braille;
    const float reg = braille ? 0.0f : std::max(0.45f, static_cast<float>(w) / 900.0f);   // the strips' misregistration, in pixels
    FxSmall glow = fx_small(w, h, braille ? 1 : std::max(2, w / 100), 3, [&](int x, int y, float* o) { const uint8_t* s = c.at(x, y); for (int k = 0; k < 3; ++k) o[k] = s[k]; });
    fx_blur(glow, braille ? 1 : 2, 2);
    static const std::array<float, 256> curve = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) { const float x = static_cast<float>(i) / 255.0f; t[static_cast<size_t>(i)] = 255.0f * fx_lerp(x, fx_smoothstep(x), 0.38f); }
        return t;
    }();
    std::vector<float> vx(static_cast<size_t>(w)), vy(static_cast<size_t>(h));
    for (int x = 0; x < w; ++x) vx[static_cast<size_t>(x)] = std::pow(std::fabs((static_cast<float>(x) + 0.5f) / w * 2.0f - 1.0f), 2.4f);
    for (int y = 0; y < h; ++y) vy[static_cast<size_t>(y)] = std::pow(std::fabs((static_cast<float>(y) + 0.5f) / h * 2.0f - 1.0f), 2.4f);
    const FxUp up(glow, w, h);
    const int gc = std::max(1, w / 400);   // the grain's cell
    std::vector<int> gxv(static_cast<size_t>(w));
    for (int x = 0; x < w; ++x) gxv[static_cast<size_t>(x)] = x / gc;
    const int cells = (w + gc - 1) / gc;
    // the strips do not register: red is taken from `reg` pixels to the left, blue to the right, green from `reg` * 0.4 above (a shift along one
    // axis only, so it is a blend of two neighbours: weights fs / 1 - fs)
    const int si = static_cast<int>(reg), sg = static_cast<int>(reg * 0.4f);
    const float fs = reg - static_cast<float>(si), fg = reg * 0.4f - static_cast<float>(sg);
    par_rows(h, [&](int y0, int y1) {
        std::vector<float> nt(static_cast<size_t>(cells));
        int cached = -1;
        for (int y = y0; y < y1; ++y) {
            if (y / gc != cached) {
                cached = y / gc;
                for (int g = 0; g < cells; ++g) nt[static_cast<size_t>(g)] = braille ? 0.0f : fx_tri(c.tick * 7919U + static_cast<uint32_t>(g), static_cast<uint32_t>(cached)) * 4.0f;
            }
            const uint8_t* rowC = c.src + static_cast<size_t>(y) * c.stride;
            const uint8_t* rowG0 = c.src + static_cast<size_t>(std::max(0, y - sg - 1)) * c.stride;
            const uint8_t* rowG1 = c.src + static_cast<size_t>(std::max(0, y - sg)) * c.stride;
            const float vyy = 0.18f * vy[static_cast<size_t>(y)];
            uint8_t* q = c.out(0, y);
            for (int x = 0; x < w; ++x, q += 3) {
                const size_t xl0 = static_cast<size_t>(std::max(0, x - si - 1)), xl1 = static_cast<size_t>(std::max(0, x - si));
                const size_t xb0 = static_cast<size_t>(std::min(w - 1, x + si)), xb1 = static_cast<size_t>(std::min(w - 1, x + si + 1));
                const size_t X = static_cast<size_t>(x);
                float v[3] = {rowC[3 * xl0] * fs + rowC[3 * xl1] * (1 - fs), rowG0[3 * X + 1] * fg + rowG1[3 * X + 1] * (1 - fg), rowC[3 * xb0 + 2] * (1 - fs) + rowC[3 * xb1 + 2] * fs};
                float gl[3];
                up.sample3(x, y, gl);
                for (int k = 0; k < 3; ++k) v[k] = fx_lerp(v[k], gl[k], 0.20f);   // the diffusion
                // un-mix the colours: every primary is made purer by taking some of the other two away
                const float r = v[0] * 1.14f - v[1] * 0.07f - v[2] * 0.07f;
                const float g = -v[0] * 0.05f + v[1] * 1.12f - v[2] * 0.07f;
                const float b = -v[0] * 0.06f - v[1] * 0.08f + v[2] * 1.14f;
                const float l0 = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
                float o[3] = {l0 * 255.0f + (r - l0 * 255.0f) * 1.30f, l0 * 255.0f + (g - l0 * 255.0f) * 1.30f, l0 * 255.0f + (b - l0 * 255.0f) * 1.30f};
                const float lc = std::clamp(l0, 0.0f, 1.0f);
                const float sh = (1.0f - lc) * (1.0f - lc), hi = lc * lc;
                o[0] += -7.0f * sh + 8.0f * hi; o[1] += 2.0f * sh + 3.0f * hi; o[2] += 8.0f * sh - 7.0f * hi;   // cool shadows, warm highlights
                const float vig = 1.0f - 0.20f * vx[X] - vyy;
                const float n = nt[static_cast<size_t>(gxv[X])];
                for (int k = 0; k < 3; ++k) {
                    const float cl = std::clamp(o[k], 0.0f, 255.0f);
                    q[k] = fx_clamp(curve[static_cast<size_t>(cl + 0.5f)] * vig + n);
                }
            }
        }
    });
}

// ------------------------------------------------------------------ Vector
// A vector monitor (Asteroids, Vectrex): there is no raster, a beam draws lines. The edges of the picture, thinned to one pixel, drawn as
// bright lines in the white-green of the phosphor with a soft glow, on black; the brighter the edge the brighter the line; the beam's
// intensity flickers a little.
void fx_vector(const FxCtx& c) {
    const int w = c.w, h = c.h;
    const bool braille = c.braille;
    const int gp = c.gp();
    FxEdges e = fx_edges(c, (!braille && w >= 480) ? 2 : 1, 3.4f);
    std::vector<float> ln(static_cast<size_t>(w) * h, 0.0f);   // the thinned lines (non-maximum suppression along the gradient)
    par_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                const float m = e.mag[i];
                if (m < 0.10f) continue;
                const float ax = std::fabs(e.gx[i]), ay = std::fabs(e.gy[i]);
                int dx = 1, dy = 0;
                if (ay > 2.414f * ax) { dx = 0; dy = 1; }
                else if (ax <= 2.414f * ay) { dx = e.gx[i] * e.gy[i] > 0.0f ? 1 : -1; dy = 1; }
                auto M = [&](int xx, int yy) { return e.mag[static_cast<size_t>(std::clamp(yy, 0, h - 1)) * w + static_cast<size_t>(std::clamp(xx, 0, w - 1))]; };
                if (m >= M(x + dx, y + dy) && m > M(x - dx, y - dy)) ln[i] = fx_smoothstep((m - 0.10f) / 0.55f);
            }
    });
    FxSmall beam = fx_small(w, h, 1, 1, [&](int x, int y, float* o) { o[0] = ln[static_cast<size_t>(y) * w + x]; });
    fx_blur(beam, gp, 1);                                      // the spot is wider than a pixel
    FxSmall glow = fx_small(w, h, braille ? 1 : std::max(2, w / 160), 1, [&](int x, int y, float* o) { o[0] = ln[static_cast<size_t>(y) * w + x]; });
    fx_blur(glow, braille ? 1 : 2, 2);
    const float flick = 0.94f + 0.06f * fx_rand(c.tick, 77);
    par_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const float core = std::min(1.0f, fx_sample(beam, 0, x, y) * 3.2f) * flick;
                const float g = std::min(1.0f, fx_sample(glow, 0, x, y) * 2.2f) * flick;
                uint8_t* q = c.out(x, y);
                // the core is almost white, the glow the phosphor's green-blue; the black is faintly lit
                q[0] = fx_clamp(3.0f + 215.0f * core + 70.0f * g);
                q[1] = fx_clamp(5.0f + 255.0f * core + 255.0f * g * 0.85f);
                q[2] = fx_clamp(4.0f + 235.0f * core + 170.0f * g * 0.70f);
            }
    });
}

// ------------------------------------------------------------------ Thermal
// A thermal camera: the brightness of the picture is the temperature, shown through the "ironbow" palette (black - violet - red - orange -
// yellow - white). Every pixel of the picture is a sensor pixel (no coarse sensor: the detail stays), the palette is interpolated between its
// steps (no bands in smooth areas) and a little noise. Nothing is drawn over the picture (no crosshair, no scale).
void fx_thermal(const FxCtx& c) {
    const int w = c.w, h = c.h;
    static const std::array<std::array<float, 3>, 257> lut = [] {
        struct Stop { float p; float r, g, b; };
        static const Stop st[] = {{0.00f, 0, 0, 12}, {0.13f, 24, 0, 78}, {0.28f, 112, 0, 132}, {0.42f, 192, 24, 92}, {0.56f, 236, 74, 22}, {0.72f, 255, 150, 0}, {0.87f, 255, 226, 62}, {1.00f, 255, 255, 232}};
        std::array<std::array<float, 3>, 257> t{};
        for (int i = 0; i < 256; ++i) {
            const float p = static_cast<float>(i) / 255.0f;
            size_t s = 0;
            while (s + 2 < sizeof(st) / sizeof(st[0]) && p > st[s + 1].p) ++s;
            const float f = std::clamp((p - st[s].p) / (st[s + 1].p - st[s].p), 0.0f, 1.0f);
            t[static_cast<size_t>(i)] = {fx_lerp(st[s].r, st[s + 1].r, f), fx_lerp(st[s].g, st[s + 1].g, f), fx_lerp(st[s].b, st[s + 1].b, f)};
        }
        t[256] = t[255];   // (the guard entry of the interpolation)
        return t;
    }();
    // the temperature of every pixel (its brightness), then a light smoothing over the four neighbours (the sensor's own noise reduction)
    std::vector<float> temp(static_cast<size_t>(w) * h);
    par_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) temp[static_cast<size_t>(y) * w + x] = fx_luma(c.at(x, y)) * (1.0f / 255.0f);
    });
    const int grain = std::max(1, static_cast<int>(std::lround(w / 640.0)));   // (the noise is as fine as a pixel of a 640 wide picture)
    par_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* r = &temp[static_cast<size_t>(y) * w];
            const float* up = &temp[static_cast<size_t>(std::max(0, y - 1)) * w];
            const float* dn = &temp[static_cast<size_t>(std::min(h - 1, y + 1)) * w];
            for (int x = 0; x < w; ++x) {
                const float l = r[std::max(0, x - 1)], rr = r[std::min(w - 1, x + 1)];
                float v = (4.0f * r[x] + l + rr + up[x] + dn[x]) * 0.125f;
                v = std::clamp((v - 0.5f) * 1.12f + 0.5f, 0.0f, 1.0f);
                v += (fx_rand(c.tick * 7919U + static_cast<uint32_t>(x / grain), static_cast<uint32_t>(y / grain)) - 0.5f) * 0.024f;
                const float p = std::clamp(v, 0.0f, 1.0f) * 255.0f;
                const int i = std::min(255, static_cast<int>(p));
                const float f = p - static_cast<float>(i);
                const auto& a = lut[static_cast<size_t>(i)];
                const auto& b = lut[static_cast<size_t>(i) + 1];
                uint8_t* q = c.out(x, y);
                q[0] = fx_clamp(a[0] + (b[0] - a[0]) * f); q[1] = fx_clamp(a[1] + (b[1] - a[1]) * f); q[2] = fx_clamp(a[2] + (b[2] - a[2]) * f);
            }
        }
    });
}

// ------------------------------------------------------------------ Risograph
// A two-colour print: the red of the picture goes to a blue ink, the green to a fluorescent pink ink (the blue is left out, as a two-drum
// print does), each ink laid down as a grainy stochastic screen, the pink drum slightly out of register, all on warm paper with fibres.
void fx_riso(const FxCtx& c) {
    const int w = c.w, h = c.h;
    const int gp = c.gp();
    const float inkA[3] = {0.0f, 0.47f, 0.75f};      // blue (0, 120, 191)
    const float inkB[3] = {1.0f, 0.28f, 0.69f};      // fluorescent pink (255, 72, 176)
    const int mx = 2 * gp, my = -gp;                  // the pink drum's registration error
    static const std::array<std::array<float, 256>, 2> dens = [] {   // the ink each colour value asks for: where there is little red, much blue ink; little green, much pink
        std::array<std::array<float, 256>, 2> d{};
        for (int i = 0; i < 256; ++i) {
            const float v = 1.0f - std::clamp((static_cast<float>(i) / 255.0f - 0.5f) * 1.25f + 0.5f, 0.0f, 1.0f);
            d[0][static_cast<size_t>(i)] = std::pow(v, 1.25f);
            d[1][static_cast<size_t>(i)] = std::pow(v, 1.1f);
        }
        return d;
    }();
    par_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t* s = c.at(x, y);
                const uint8_t* s2 = c.at(std::clamp(x - mx, 0, w - 1), std::clamp(y - my, 0, h - 1));
                const float dA = dens[0][s[0]];    // where there is no red: blue ink
                const float dB = dens[1][s2[1]];   // where there is no green: pink ink    // where there is no green: pink ink
                const int gx = x / (2 * gp), gy = y / (2 * gp);   // (the grain is two pixels: ink spreads)
                const float thrA = (static_cast<float>(fx_blue32(gx, gy)) + 0.5f) / 1024.0f * 0.7f + fx_rand(static_cast<uint32_t>(gx), static_cast<uint32_t>(gy) + 31U) * 0.3f;
                const float thrB = (static_cast<float>(fx_blue32(gx + 13, gy + 7)) + 0.5f) / 1024.0f * 0.7f + fx_rand(static_cast<uint32_t>(gx) + 4001U, static_cast<uint32_t>(gy) + 77U) * 0.3f;
                const float aA = fx_smoothstep((dA * 1.08f - thrA) / 0.55f + 0.5f);
                const float aB = fx_smoothstep((dB * 1.08f - thrB) / 0.55f + 0.5f);
                const float fibre = (fx_rand(static_cast<uint32_t>(gx) + 99U, static_cast<uint32_t>(gy) + 5U) - 0.5f) * 7.0f;
                const float paper[3] = {247.0f, 240.0f, 224.0f};
                uint8_t* q = c.out(x, y);
                for (int k = 0; k < 3; ++k) {
                    const float v = (paper[k] + fibre) * fx_lerp(1.0f, inkA[k], aA * 0.96f) * fx_lerp(1.0f, inkB[k], aB * 0.96f);
                    q[k] = fx_clamp(v);
                }
            }
    });
}

// ------------------------------------------------------------------ LaserDisc
// A laser disc is a composite video signal: crisp for its day, but with the signature of the signal -- a ringing halo at sharp edges, the colour
// carried a little softer than the brightness, and the dot crawl: the colour sub-carrier leaks into fine detail and crawls as a rainbow checker
// along high-contrast edges. Slightly rich colour, a hint of noise.
void fx_laserdisc(const FxCtx& c) {
    const int w = c.w, h = c.h;
    const bool braille = c.braille;
    const int sc = braille ? 1 : std::max(1, w / 320);
    const int rr = braille ? 1 : std::max(1, w / 220);
    const int rc = braille ? 1 : std::max(1, w / 150);
    par_rows(h, [&](int y0, int y1) {
        std::vector<float> Y(static_cast<size_t>(w)), I(static_cast<size_t>(w)), Q(static_cast<size_t>(w)), Yb(static_cast<size_t>(w)), Ib(static_cast<size_t>(w)), Qb(static_cast<size_t>(w)), Ys(static_cast<size_t>(w)), pre(static_cast<size_t>(w) + 1);
        for (int y = y0; y < y1; ++y) {
            const uint8_t* s = c.at(0, y);
            for (int x = 0; x < w; ++x) fx_to_yiq(s[3 * x], s[3 * x + 1], s[3 * x + 2], Y[static_cast<size_t>(x)], I[static_cast<size_t>(x)], Q[static_cast<size_t>(x)]);
            fx_box_row(Y.data(), Yb.data(), pre.data(), w, rr + 1);
            fx_box_row(Y.data(), Ys.data(), pre.data(), w, 1);
            fx_box_row(I.data(), Ib.data(), pre.data(), w, rc);
            fx_box_row(Q.data(), Qb.data(), pre.data(), w, rc);
            uint8_t* o = c.out(0, y);
            for (int x = 0; x < w; ++x) {
                const size_t xi = static_cast<size_t>(x);
                const float ring = Ys[xi] - Yb[xi];                              // the detail: it overshoots next to an edge
                const float yy = Ys[xi] + 0.9f * ring;
                // the dot crawl: the detail of the brightness appears in the colour, in a pattern that moves with every frame
                const float ph = 6.2831853f * (0.25f * static_cast<float>(x / sc) + 0.5f * static_cast<float>(y / sc)) + 3.1415927f * static_cast<float>(c.tick);
                const float crawl = braille ? 0.0f : std::clamp(ring, -80.0f, 80.0f) * 0.7f;
                float ii = Ib[xi] * 1.06f + crawl * std::cos(ph), qq = Qb[xi] * 1.06f + crawl * std::sin(ph);
                const float n = (fx_rand(c.tick * 7919U + static_cast<uint32_t>(x / sc), static_cast<uint32_t>(y)) - 0.5f) * 5.0f;
                fx_from_yiq(yy + n, ii, qq, o + 3 * x);
            }
        }
    });
}

// ------------------------------------------------------------------ Halftone CMYK
// Four-colour printing: the picture is split into cyan, magenta, yellow and black, each ink printed as dots on a grid of its own angle (15, 75, 0
// and 45 degrees, so the grids make rosettes instead of moire), on warm paper. The ink colours are those of process printing; where they
// overlap they multiply, as ink does.
// `fine` (Halftone CMYK High-Res): a screen of about 100 lines to the picture's height instead of 38 (a cell of about 3.6 pixels in a 360 pixel
// picture, 7 in a 720 one), the colour of a dot taken from a 2 x 2 block instead of 3 x 3: the picture's detail is kept and the dots are small.
// `mix` (the Effect Contrast, video_fx_print_mix): how much of the print is in the picture -- 1 is the print as it comes off the press, less lets the
// picture itself show through it (the dots become a texture on the picture instead of replacing it).
// The radius of a dot depends on the cell only, so it is worked out once per cell and picture (a table per screen) and not again on every row that
// crosses the cell -- which, with the small cells of the fine screen, was more than one colour conversion per pixel.
void fx_halftone_cmyk(const FxCtx& c, bool fine, float mix) {
    const int w = c.w, h = c.h;
    const float cs = c.braille ? (fine ? 2.2f : 3.0f) : fine ? std::max(3.4f, static_cast<float>(h) / 100.0f) : std::max(6.0f, static_cast<float>(h) / 38.0f);
    const int bf = fine ? 2 : 3;   // the block of the picture that gives a dot its colour
    const float aa = (fine ? 0.55f : 0.9f) / cs, ics = 1.0f / cs, inv2aa = 1.0f / (2.0f * aa);   // (the edge of a dot is about a pixel wide: less for the fine screen, where a dot is only a few pixels)
    auto fl = [](float v) { const int i = static_cast<int>(v); return static_cast<float>(i - (v < static_cast<float>(i) ? 1 : 0)); };   // floor (no library call per pixel)
    struct Screen { float ca, sa; float ink[3]; };
    auto rad = [](float deg) { return deg * 0.017453293f; };
    const Screen sc[4] = {
        {std::cos(rad(15.0f)), std::sin(rad(15.0f)), {0.00f, 0.68f, 0.94f}},   // cyan
        {std::cos(rad(75.0f)), std::sin(rad(75.0f)), {0.93f, 0.00f, 0.55f}},   // magenta
        {std::cos(rad(0.0f)), std::sin(rad(0.0f)), {1.00f, 0.95f, 0.00f}},      // yellow
        {std::cos(rad(45.0f)), std::sin(rad(45.0f)), {0.14f, 0.12f, 0.13f}}};   // black
    // the position of a picture point on the grid of screen i, in cells
    auto grid_u = [&](int i, float x, float y) { return x * sc[i].ca * ics + y * sc[i].sa * ics; };
    auto grid_v = [&](int i, float x, float y) { return -x * sc[i].sa * ics + y * sc[i].ca * ics; };
    // the colour under a dot: the average of a 3 x 3 block of the picture (one pass over the picture, then a look-up per dot)
    const FxSmall avg = fx_small(w, h, bf, 3, [&](int x, int y, float* o) { const uint8_t* s = c.at(x, y); for (int k = 0; k < 3; ++k) o[k] = static_cast<float>(s[k]); });
    // tables over an ink amount 0 .. 1 (1024 steps): the skeleton black, and the radius of the dot for that amount (with the dot gain: the ink spreads)
    struct Luts {
        float black[1025], radius[1025];
        Luts() {
            for (int i = 0; i <= 1024; ++i) {
                const float a = static_cast<float>(i) / 1024.0f;
                black[i] = std::pow(a, 1.8f) * 0.9f;   // (a "skeleton" black: only the dark parts get black dots)
                const float g = std::pow(a, 0.86f);    // (dot gain)
                radius[i] = 0.5642f * std::sqrt(g) + 0.215f * fx_smoothstep((g - 0.6f) / 0.4f);   // the dot for an area g of the cell (it grows past the cell's edge when the ink is dense)
            }
        }
    };
    static const Luts lut;
    auto dot = [&](int i, int xc, int yc) {   // the radius of the dot of screen i at the picture point (xc, yc), in cells
        const float* a = &avg.v[(static_cast<size_t>(yc / bf) * avg.w + static_cast<size_t>(xc / bf)) * 3];
        const float r = a[0] / 255.0f, g = a[1] / 255.0f, b = a[2] / 255.0f;
        const float lm = 0.299f * r + 0.587f * g + 0.114f * b;   // (a little more colour: the inks have a smaller gamut than a screen)
        const float c0 = 1.0f - std::clamp(lm + (r - lm) * 1.2f, 0.0f, 1.0f), m0 = 1.0f - std::clamp(lm + (g - lm) * 1.2f, 0.0f, 1.0f), y0 = 1.0f - std::clamp(lm + (b - lm) * 1.2f, 0.0f, 1.0f);
        const float k = lut.black[static_cast<int>(std::min({c0, m0, y0}) * 1024.0f)];
        const float ink = i == 0 ? c0 - k : i == 1 ? m0 - k : i == 2 ? y0 - k : k;
        return lut.radius[static_cast<int>(std::clamp(ink, 0.0f, 1.0f) * 1024.0f)];
    };
    // How much of a pixel a dot covers depends only on the pixel's squared distance from the cell's centre and the dot's radius: one table of both
    // (512 x 128 steps; made again only when the edge width changes, i.e. for another picture height), so a pixel costs no square root and no curve.
    constexpr int kD2 = 512, kRq = 128;
    constexpr float kRmax = 0.8f;   // (the largest radius of the table above is 0.78 cells)
    struct CoverLut { float aa = -1.0f; std::vector<float> v; };
    thread_local CoverLut cl;
    if (cl.aa != aa) {
        cl.aa = aa;
        cl.v.assign(static_cast<size_t>(kD2) * kRq, 0.0f);
        for (int q = 0; q < kRq; ++q) {
            const float rr = (static_cast<float>(q) + 0.5f) * kRmax / kRq;
            const float lo = std::max(0.0f, rr - aa), hi = rr + aa;
            for (int d = 0; d < kD2; ++d) {
                const float d2 = (static_cast<float>(d) + 0.5f) * 0.5f / kD2;
                cl.v[static_cast<size_t>(q) * kD2 + static_cast<size_t>(d)] = d2 <= lo * lo ? 1.0f : d2 >= hi * hi ? 0.0f : 1.0f - fx_smoothstep((std::sqrt(d2) - (rr - aa)) * inv2aa);
            }
        }
    }
    const float* cov_lut = cl.v.data();
    // the dot radius of every cell of every screen that the picture can touch (as a row of the cover table): the cells that hold the four corners of the
    // picture and all between, and one more around for the rounding of the position
    struct Grid { int u0 = 0, v0 = 0, nu = 1, nv = 1; std::vector<uint16_t> r; };
    Grid grid[4];
    for (int i = 0; i < 4; ++i) {
        float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
        for (int k = 0; k < 4; ++k) {
            const float x = (k & 1) ? static_cast<float>(w - 1) : 0.0f, y = (k & 2) ? static_cast<float>(h - 1) : 0.0f;
            umin = std::min(umin, grid_u(i, x, y)); umax = std::max(umax, grid_u(i, x, y));
            vmin = std::min(vmin, grid_v(i, x, y)); vmax = std::max(vmax, grid_v(i, x, y));
        }
        Grid& g = grid[i];
        g.u0 = static_cast<int>(fl(umin)) - 2; g.v0 = static_cast<int>(fl(vmin)) - 2;
        g.nu = static_cast<int>(fl(umax)) + 3 - g.u0; g.nv = static_cast<int>(fl(vmax)) + 3 - g.v0;
        g.r.assign(static_cast<size_t>(g.nu) * static_cast<size_t>(g.nv), 0);
        par_rows(g.nv, [&](int a, int b) {
            for (int iv = a; iv < b; ++iv)
                for (int iu = 0; iu < g.nu; ++iu) {
                    const float cu = static_cast<float>(g.u0 + iu) + 0.5f, cv = static_cast<float>(g.v0 + iv) + 0.5f;   // the centre of the cell
                    const int xc = std::clamp(static_cast<int>((cu * sc[i].ca - cv * sc[i].sa) * cs), 0, w - 1), yc = std::clamp(static_cast<int>((cu * sc[i].sa + cv * sc[i].ca) * cs), 0, h - 1);
                    const int q = std::clamp(static_cast<int>(dot(i, xc, yc) * (kRq / kRmax)), 0, kRq - 1);
                    g.r[static_cast<size_t>(iv) * static_cast<size_t>(g.nu) + static_cast<size_t>(iu)] = static_cast<uint16_t>(q * kD2);
                }
        });
    }
    float au[4], bu[4], av[4], bv[4], inkm[4][3];   // the steps of the grid position per pixel across and down, and what full ink does to each channel
    for (int i = 0; i < 4; ++i) {
        au[i] = sc[i].ca * ics; bu[i] = sc[i].sa * ics; av[i] = -sc[i].sa * ics; bv[i] = sc[i].ca * ics;
        for (int k = 0; k < 3; ++k) inkm[i][k] = (sc[i].ink[k] - 1.0f) * 0.97f;
    }
    const float paper[3] = {247.0f, 242.0f, 230.0f};
    par_rows(h, [&](int y0, int y1) {
        // one screen at a time along a row (its numbers stay in registers), the cover of each pixel into a row of its own; then the inks of the four rows
        std::vector<float> cov(static_cast<size_t>(w) * 4);
        for (int y = y0; y < y1; ++y) {
            for (int i = 0; i < 4; ++i) {
                const Grid& g = grid[i];
                // (the grid position as the cells count it; the cell's whole part found with a floor that needs no library call. The table's first cell is
                // taken off as a whole number afterwards, so a pixel right on the border of two cells -- every 3.4 pixels of the fine yellow screen, which is
                // not turned -- falls into the same cell as before.)
                const float a_u = au[i], a_v = av[i], u_y = static_cast<float>(y) * bu[i], v_y = static_cast<float>(y) * bv[i];
                const uint16_t* R = g.r.data();
                const int nu = g.nu, num1 = g.nu - 1, nvm1 = g.nv - 1, u0 = g.u0, v0 = g.v0;
                float* C = cov.data() + static_cast<size_t>(i) * static_cast<size_t>(w);
                for (int x = 0; x < w; ++x) {
                    const float u = static_cast<float>(x) * a_u + u_y, vv = static_cast<float>(x) * a_v + v_y;
                    int fu = static_cast<int>(u), fv = static_cast<int>(vv);
                    fu -= u < static_cast<float>(fu) ? 1 : 0;
                    fv -= vv < static_cast<float>(fv) ? 1 : 0;
                    const float du = u - static_cast<float>(fu) - 0.5f, dv = vv - static_cast<float>(fv) - 0.5f;
                    const int iu = std::clamp(fu - u0, 0, num1), iv = std::clamp(fv - v0, 0, nvm1);
                    const int d = std::min(kD2 - 1, static_cast<int>((du * du + dv * dv) * (2.0f * kD2)));
                    C[x] = cov_lut[R[iv * nu + iu] + d];
                }
            }
            const float* C0 = cov.data();
            const float* C1 = C0 + w;
            const float* C2 = C1 + w;
            const float* C3 = C2 + w;
            for (int x = 0; x < w; ++x) {
                float v[3];
                for (int k = 0; k < 3; ++k)
                    v[k] = paper[k] * (1.0f + inkm[0][k] * C0[x]) * (1.0f + inkm[1][k] * C1[x]) * (1.0f + inkm[2][k] * C2[x]) * (1.0f + inkm[3][k] * C3[x]);
                uint8_t* q = c.out(x, y);
                if (mix < 1.0f) {   // (the picture shows through the print)
                    const uint8_t* s = c.at(x, y);
                    for (int k = 0; k < 3; ++k) q[k] = fx_clamp(fx_lerp(static_cast<float>(s[k]), v[k], mix));
                } else {
                    for (int k = 0; k < 3; ++k) q[k] = fx_clamp(v[k]);
                }
            }
        }
    });
}
