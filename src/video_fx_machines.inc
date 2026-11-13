// The looks of old computers, built as their hardware built a picture (included by video_player.cpp after video_fx_retro.inc):
//   ZX Spectrum (29): 8 colours x 2 brightnesses, but only TWO colours in every 8 x 8 cell (ink and paper): the famous colour clash
//   Amstrad CPC (30): the 27 colours of the hardware palette (three levels per colour gun), wide pixels (mode 0: 160 x 200), ordered dither
//   Teletext (31): 40 x 24 character cells of 2 x 3 mosaic blocks, two of the eight colours in every cell
//   Amiga HAM (32): hold-and-modify -- a palette of 16, and every pixel either takes one of them or changes ONE colour gun of the pixel before it
//   ASCII (37): the picture drawn with the 95 printable ASCII characters, about 100 across: edges as lines along them (| / \\ - _), every other cell the
//   character whose shape is nearest to the picture there (4 x 8 blocks of ink); the shades spread over the characters by a contrast-limited equalisation

// ------------------------------------------------------------------ ZX Spectrum
void fx_spectrum(const FxCtx& c) {
    const int w = c.w, h = c.h;
    const int vs = std::max(1, w / 256);   // real pixels per picture pixel: about 256 across
    const int vw = (w + vs - 1) / vs, vh = (h + vs - 1) / vs;
    static const FxGammaLut lift(0.68f);
    std::vector<float> img(static_cast<size_t>(vw) * vh * 3);   // the picture at the Spectrum's resolution, a little more colourful and lighter
    par_rows(vh, [&](int r0, int r1) {
        for (int vy = r0; vy < r1; ++vy)
            for (int vx = 0; vx < vw; ++vx) {
                float sum[3] = {0, 0, 0};
                int n = 0;
                for (int y = vy * vs; y < std::min(h, vy * vs + vs); ++y)
                    for (int x = vx * vs; x < std::min(w, vx * vs + vs); ++x) { const uint8_t* s = c.at(x, y); sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2]; ++n; }
                float* o = &img[(static_cast<size_t>(vy) * vw + vx) * 3];
                const float m[3] = {sum[0] / static_cast<float>(n), sum[1] / static_cast<float>(n), sum[2] / static_cast<float>(n)};
                const float l = 0.299f * m[0] + 0.587f * m[1] + 0.114f * m[2];
                for (int k = 0; k < 3; ++k) o[k] = lift.at(std::clamp((l + (m[k] - l) * 1.3f - 128.0f) * 1.12f + 128.0f, 0.0f, 255.0f));
            }
    });
    // the palette: bit 0 blue, bit 1 red, bit 2 green (as on the machine); 0xCD normal, 0xFF bright, black is black in both
    auto pal = [](int idx, int bright, float* o) {
        const float lv = idx == 0 ? 0.0f : bright ? 255.0f : 205.0f;
        o[0] = (idx & 2) ? lv : 0.0f; o[1] = (idx & 4) ? lv : 0.0f; o[2] = (idx & 1) ? lv : 0.0f;
    };
    const int pat = fx_pat(c.dither, 3);
    const int ncx = (vw + 7) / 8, ncy = (vh + 7) / 8;
    par_rows(ncy, [&](int r0, int r1) {
        for (int cy = r0; cy < r1; ++cy)
            for (int cx = 0; cx < ncx; ++cx) {
                const int px0 = cx * 8, py0 = cy * 8;
                const int cw = std::min(8, vw - px0), ch = std::min(8, vh - py0);
                float best_err = 1e30f, bi[3] = {0, 0, 0}, bp[3] = {0, 0, 0};
                for (int bright = 0; bright < 2; ++bright) {
                    float pc[8][3];
                    for (int i = 0; i < 8; ++i) pal(i, bright, pc[i]);
                    int hist[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                    for (int y = 0; y < ch; ++y)
                        for (int x = 0; x < cw; ++x) {
                            const float* p = &img[(static_cast<size_t>(py0 + y) * vw + px0 + x) * 3];
                            int bj = 0;
                            float bd = 1e30f;
                            for (int i = 0; i < 8; ++i) {
                                const float dr = p[0] - pc[i][0], dg = p[1] - pc[i][1], db = p[2] - pc[i][2];
                                const float dd = 0.30f * dr * dr + 0.59f * dg * dg + 0.11f * db * db;
                                if (dd < bd) { bd = dd; bj = i; }
                            }
                            ++hist[bj];
                        }
                    int top[3] = {0, 1, 2};   // the three colours this cell uses most (ties: the lower index)
                    {
                        int order[8] = {0, 1, 2, 3, 4, 5, 6, 7};
                        std::stable_sort(order, order + 8, [&](int a, int b) { return hist[a] > hist[b]; });
                        top[0] = order[0]; top[1] = order[1]; top[2] = order[2];
                    }
                    for (int a = 0; a < 3; ++a)
                        for (int b = a + 1; b < 3; ++b) {
                            const float* A = pc[top[a]];
                            const float* B = pc[top[b]];
                            const float ab[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]};
                            const float len2 = std::max(1.0f, ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]);
                            float err = 0.0f;
                            for (int y = 0; y < ch; ++y)
                                for (int x = 0; x < cw; ++x) {
                                    const float* p = &img[(static_cast<size_t>(py0 + y) * vw + px0 + x) * 3];
                                    const float tp = std::clamp(((p[0] - A[0]) * ab[0] + (p[1] - A[1]) * ab[1] + (p[2] - A[2]) * ab[2]) / len2, 0.0f, 1.0f);
                                    const float rank = (static_cast<float>(fx_dither_rank(pat, px0 + x, py0 + y)) + 0.5f) / 1024.0f;
                                    const float* P = tp > rank ? B : A;
                                    const float dr = p[0] - P[0], dg = p[1] - P[1], db = p[2] - P[2];
                                    err += 0.30f * dr * dr + 0.59f * dg * dg + 0.11f * db * db;
                                }
                            if (err < best_err) { best_err = err; for (int k = 0; k < 3; ++k) { bi[k] = B[k]; bp[k] = A[k]; } }
                        }
                }
                const float ab[3] = {bi[0] - bp[0], bi[1] - bp[1], bi[2] - bp[2]};
                const float len2 = std::max(1.0f, ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]);
                for (int y = 0; y < ch; ++y)
                    for (int x = 0; x < cw; ++x) {
                        const float* p = &img[(static_cast<size_t>(py0 + y) * vw + px0 + x) * 3];
                        const float tp = std::clamp(((p[0] - bp[0]) * ab[0] + (p[1] - bp[1]) * ab[1] + (p[2] - bp[2]) * ab[2]) / len2, 0.0f, 1.0f);
                        const float rank = (static_cast<float>(fx_dither_rank(pat, px0 + x, py0 + y)) + 0.5f) / 1024.0f;
                        const float* P = tp > rank ? bi : bp;
                        const uint8_t col[3] = {fx_clamp(P[0]), fx_clamp(P[1]), fx_clamp(P[2])};
                        for (int yy = (py0 + y) * vs; yy < std::min(h, (py0 + y) * vs + vs); ++yy) {
                            uint8_t* q = c.out((px0 + x) * vs, yy);
                            for (int xx = (px0 + x) * vs; xx < std::min(w, (px0 + x) * vs + vs); ++xx, q += 3) { q[0] = col[0]; q[1] = col[1]; q[2] = col[2]; }
                        }
                    }
            }
    });
}

// ------------------------------------------------------------------ Amstrad CPC
void fx_cpc(const FxCtx& c) {
    const int w = c.w, h = c.h;
    const int bw = c.braille ? 2 : std::max(2, (w + 159) / 160);   // mode 0: 160 pixels across, each twice as wide as tall
    const int bh = std::max(1, bw / 2);
    const int pat = fx_pat(c.dither, 3);
    const int brows = (h + bh - 1) / bh;
    par_rows(brows, [&](int r0, int r1) {
        for (int br = r0; br < r1; ++br) {
            const int by = br * bh;
            for (int bx = 0, bc = 0; bx < w; bx += bw, ++bc) {
                const int x1 = std::min(w, bx + bw), y1 = std::min(h, by + bh);
                float sum[3] = {0, 0, 0};
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x) { const uint8_t* s = c.at(x, y); sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2]; }
                const float n = static_cast<float>((x1 - bx) * (y1 - by));
                const float rank = (static_cast<float>(fx_dither_rank(pat, bc, br)) + 0.5f) / 1024.0f;
                uint8_t col[3];
                for (int k = 0; k < 3; ++k) {
                    const float v = std::clamp(((sum[k] / n) - 128.0f) * 1.12f + 128.0f, 0.0f, 255.0f) / 127.5f;   // 0 .. 2: the three levels of a gun
                    const int lv = std::clamp(static_cast<int>(std::floor(v + 0.5f + (rank - 0.5f) * 0.72f)), 0, 2);   // (a little less than a full step of dither: less colour noise)
                    col[k] = lv == 0 ? 0 : lv == 1 ? 127 : 255;
                }
                for (int y = by; y < y1; ++y) {
                    uint8_t* q = c.out(bx, y);
                    for (int x = bx; x < x1; ++x, q += 3) { q[0] = col[0]; q[1] = col[1]; q[2] = col[2]; }
                }
            }
        }
    });
}

// ------------------------------------------------------------------ Teletext
void fx_teletext(const FxCtx& c) {
    const int w = c.w, h = c.h;
    static const FxGammaLut lift(0.8f);
    const int cols = 40, rows = 24;   // characters; every one is 2 x 3 mosaic blocks
    auto x_of = [&](int sx) { return static_cast<int>(static_cast<long long>(sx) * w / (cols * 2)); };
    auto y_of = [&](int sy) { return static_cast<int>(static_cast<long long>(sy) * h / (rows * 3)); };
    par_rows(rows, [&](int r0, int r1) {
        for (int cy = r0; cy < r1; ++cy)
            for (int cx = 0; cx < cols; ++cx) {
                float sub[6][3];
                for (int j = 0; j < 6; ++j) {   // the six blocks: 2 across, 3 down
                    const int sx = cx * 2 + (j & 1), sy = cy * 3 + (j >> 1);
                    const int xa = x_of(sx), xb = std::max(xa + 1, x_of(sx + 1)), ya = y_of(sy), yb = std::max(ya + 1, y_of(sy + 1));
                    float sum[3] = {0, 0, 0};
                    int n = 0;
                    for (int y = ya; y < std::min(h, yb); ++y)
                        for (int x = xa; x < std::min(w, xb); ++x) { const uint8_t* s = c.at(x, y); sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2]; ++n; }
                    const float inv = 1.0f / static_cast<float>(std::max(1, n));
                    const float m[3] = {sum[0] * inv, sum[1] * inv, sum[2] * inv};
                    const float l = 0.299f * m[0] + 0.587f * m[1] + 0.114f * m[2];
                    for (int k = 0; k < 3; ++k) sub[j][k] = lift.at(std::clamp((l + (m[k] - l) * 1.3f - 128.0f) * 1.12f + 128.0f, 0.0f, 255.0f));
                }
                int ba = 0, bb = 1;
                float best = 1e30f;
                for (int a = 0; a < 8; ++a)
                    for (int b = a + 1; b < 8; ++b) {
                        float err = 0.0f;
                        for (int j = 0; j < 6; ++j) {
                            float da = 0.0f, db = 0.0f;
                            for (int k = 0; k < 3; ++k) {
                                const float wk = k == 0 ? 0.30f : k == 1 ? 0.59f : 0.11f;
                                const float ca = (a >> k & 1) ? 255.0f : 0.0f, cb = (b >> k & 1) ? 255.0f : 0.0f;   // bit 0 red, bit 1 green, bit 2 blue
                                da += wk * (sub[j][k] - ca) * (sub[j][k] - ca);
                                db += wk * (sub[j][k] - cb) * (sub[j][k] - cb);
                            }
                            err += std::min(da, db);
                        }
                        if (err < best) { best = err; ba = a; bb = b; }
                    }
                for (int j = 0; j < 6; ++j) {
                    float da = 0.0f, db = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        const float wk = k == 0 ? 0.30f : k == 1 ? 0.59f : 0.11f;
                        const float ca = (ba >> k & 1) ? 255.0f : 0.0f, cb = (bb >> k & 1) ? 255.0f : 0.0f;
                        da += wk * (sub[j][k] - ca) * (sub[j][k] - ca);
                        db += wk * (sub[j][k] - cb) * (sub[j][k] - cb);
                    }
                    const int pick = da <= db ? ba : bb;
                    const uint8_t col[3] = {static_cast<uint8_t>((pick & 1) ? 255 : 0), static_cast<uint8_t>((pick & 2) ? 255 : 0), static_cast<uint8_t>((pick & 4) ? 255 : 0)};
                    const int sx = cx * 2 + (j & 1), sy = cy * 3 + (j >> 1);
                    const int xa = x_of(sx), xb = std::max(xa + 1, x_of(sx + 1)), ya = y_of(sy), yb = std::max(ya + 1, y_of(sy + 1));
                    for (int y = ya; y < std::min(h, yb); ++y) {
                        uint8_t* q = c.out(xa, y);
                        for (int x = xa; x < std::min(w, xb); ++x, q += 3) { q[0] = col[0]; q[1] = col[1]; q[2] = col[2]; }
                    }
                }
            }
    });
}

// ------------------------------------------------------------------ Amiga HAM (HAM6: 12 bit colour, 16 palette entries)
void fx_ham(const FxCtx& c) {
    const int w = c.w, h = c.h;
    const int bs = c.braille ? 1 : std::clamp(w / 320, 1, 4);
    const int vw = (w + bs - 1) / bs, vh = (h + bs - 1) / bs;
    std::vector<uint8_t> img(static_cast<size_t>(vw) * vh * 3);   // the picture at the Amiga's resolution, 4 bits a colour (0 .. 15)
    par_rows(vh, [&](int r0, int r1) {
        for (int vy = r0; vy < r1; ++vy)
            for (int vx = 0; vx < vw; ++vx) {
                float sum[3] = {0, 0, 0};
                int n = 0;
                for (int y = vy * bs; y < std::min(h, vy * bs + bs); ++y)
                    for (int x = vx * bs; x < std::min(w, vx * bs + bs); ++x) { const uint8_t* s = c.at(x, y); sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2]; ++n; }
                for (int k = 0; k < 3; ++k) img[(static_cast<size_t>(vy) * vw + vx) * 3 + static_cast<size_t>(k)] = static_cast<uint8_t>(std::clamp(static_cast<int>(sum[k] / static_cast<float>(n) / 17.0f + 0.5f), 0, 15));
            }
    });
    // the 16 palette colours: a median cut of the picture's colours (4 bits a colour), the same way every time
    struct Entry { int rgb[3]; int count; };
    std::vector<Entry> all;
    {
        std::vector<int> hist(4096, 0);
        for (size_t i = 0; i < img.size(); i += 3) ++hist[static_cast<size_t>(img[i]) * 256 + static_cast<size_t>(img[i + 1]) * 16 + img[i + 2]];
        for (int i = 0; i < 4096; ++i) if (hist[static_cast<size_t>(i)] > 0) all.push_back({{i >> 8, (i >> 4) & 15, i & 15}, hist[static_cast<size_t>(i)]});
    }
    std::vector<std::vector<Entry>> boxes;
    boxes.push_back(all);
    while (boxes.size() < 16) {
        int pick = -1;
        long long bestScore = 0;
        for (size_t b = 0; b < boxes.size(); ++b) {
            if (boxes[b].size() < 2) continue;
            int lo[3] = {15, 15, 15}, hi[3] = {0, 0, 0};
            long long pop = 0;
            for (const Entry& e : boxes[b]) { pop += e.count; for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], e.rgb[k]); hi[k] = std::max(hi[k], e.rgb[k]); } }
            const long long score = pop * (std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) + 1);
            if (score > bestScore) { bestScore = score; pick = static_cast<int>(b); }
        }
        if (pick < 0) break;
        std::vector<Entry> box = std::move(boxes[static_cast<size_t>(pick)]);
        int lo[3] = {15, 15, 15}, hi[3] = {0, 0, 0};
        for (const Entry& e : box) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], e.rgb[k]); hi[k] = std::max(hi[k], e.rgb[k]); }
        int axis = 0;
        for (int k = 1; k < 3; ++k) if (hi[k] - lo[k] > hi[axis] - lo[axis]) axis = k;
        std::stable_sort(box.begin(), box.end(), [&](const Entry& a, const Entry& b) { return a.rgb[axis] < b.rgb[axis]; });
        long long total = 0, run = 0;
        for (const Entry& e : box) total += e.count;
        size_t cut = 1;
        for (size_t i = 0; i < box.size(); ++i) { run += box[i].count; if (run * 2 >= total) { cut = std::clamp<size_t>(i + 1, 1, box.size() - 1); break; } }
        boxes[static_cast<size_t>(pick)] = std::vector<Entry>(box.begin(), box.begin() + static_cast<long>(cut));
        boxes.push_back(std::vector<Entry>(box.begin() + static_cast<long>(cut), box.end()));
    }
    int pal[16][3] = {};
    int np = 0;
    for (const auto& b : boxes) {
        if (np >= 16 || b.empty()) continue;
        long long s[3] = {0, 0, 0}, n = 0;
        for (const Entry& e : b) { for (int k = 0; k < 3; ++k) s[k] += static_cast<long long>(e.rgb[k]) * e.count; n += e.count; }
        for (int k = 0; k < 3; ++k) pal[np][k] = static_cast<int>((s[k] + n / 2) / std::max(1LL, n));
        ++np;
    }
    if (np == 0) { np = 1; pal[0][0] = pal[0][1] = pal[0][2] = 0; }
    // the darkest colour is entry 0: every line starts with it (the border colour)
    int dark = 0;
    for (int i = 1; i < np; ++i) if (pal[i][0] * 3 + pal[i][1] * 6 + pal[i][2] < pal[dark][0] * 3 + pal[dark][1] * 6 + pal[dark][2]) dark = i;
    if (dark != 0) for (int k = 0; k < 3; ++k) std::swap(pal[0][k], pal[dark][k]);
    par_rows(vh, [&](int r0, int r1) {
        for (int vy = r0; vy < r1; ++vy) {
            int cur[3] = {pal[0][0], pal[0][1], pal[0][2]};
            for (int vx = 0; vx < vw; ++vx) {
                const uint8_t* t = &img[(static_cast<size_t>(vy) * vw + vx) * 3];
                // the choices: one of the palette colours, or the pixel before with ONE colour gun set to the target's value
                int best[3] = {cur[0], cur[1], cur[2]};
                float bd = 1e30f;
                auto consider = [&](const int* cand) {
                    const float dr = static_cast<float>(cand[0] - t[0]), dg = static_cast<float>(cand[1] - t[1]), db = static_cast<float>(cand[2] - t[2]);
                    const float dd = 0.30f * dr * dr + 0.59f * dg * dg + 0.11f * db * db;
                    if (dd < bd) { bd = dd; best[0] = cand[0]; best[1] = cand[1]; best[2] = cand[2]; }
                };
                for (int i = 0; i < np; ++i) consider(pal[i]);
                for (int k = 0; k < 3; ++k) { int m[3] = {cur[0], cur[1], cur[2]}; m[k] = t[k]; consider(m); }
                cur[0] = best[0]; cur[1] = best[1]; cur[2] = best[2];
                const uint8_t col[3] = {static_cast<uint8_t>(cur[0] * 17), static_cast<uint8_t>(cur[1] * 17), static_cast<uint8_t>(cur[2] * 17)};
                for (int y = vy * bs; y < std::min(h, vy * bs + bs); ++y) {
                    uint8_t* q = c.out(vx * bs, y);
                    for (int x = vx * bs; x < std::min(w, vx * bs + bs); ++x, q += 3) { q[0] = col[0]; q[1] = col[1]; q[2] = col[2]; }
                }
            }
        }
    });
}

// ------------------------------------------------------------------ ASCII
// The picture drawn with characters, as ASCII art is drawn by hand. About 100 across (6 x 12 pixels in a 640 wide picture: every character can be read
// as itself; it was 180 across, where a character was a 3 x 7 smudge and they all looked alike). Two kinds of cell:
//  - an edge (the cell's brightness changes clearly in one direction, among the strongest edges of the picture): a line along it, | / \ - _ ;
//  - every other cell: of the 95 printable ASCII characters (video_fx_ascii_font.inc; the Latin-1 letters and symbols of before -- ß, ¶, Æ -- filled the
//    bright parts with shapes that do not look like ASCII art) the one whose
//    SHAPE comes nearest to the picture in that cell (4 x 8 blocks of brightness against the 4 x 8 pattern of ink), among those about as bright.
// The brightness of the cells is spread over the whole range of the characters (an equalisation over the picture, mixed with the plain brightness),
// so a bright picture is not a wall of the inkiest character but shows its shades. The character takes the colour of the cell.
#include "video_fx_ascii_font.inc"

struct AsciiSet {
    static constexpr int kDims = 32;                           // 4 x 8 blocks of a glyph (a block is 2 x 2 pixels of the 8 x 16 bitmap)
    float v[kAsciiGlyphCount][kDims];                          // the ink of every block (1 = the inkiest character's average), without the glyph's mean; in the order of the mean
    float mean[kAsciiGlyphCount];                              // the mean ink of the glyph (the same scale)
    float energy[kAsciiGlyphCount];                            // the sum of the squares of v
    int idx[kAsciiGlyphCount];                                 // the glyph (kAsciiGlyphs) of entry i
    AsciiSet() {
        float raw[kAsciiGlyphCount][kDims], m[kAsciiGlyphCount];
        float top = 0.0f;
        for (int g = 0; g < kAsciiGlyphCount; ++g) {
            float sum = 0.0f;
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 4; ++i) {
                    int n = 0;
                    for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) n += (kAsciiGlyphs[g][2 * j + dy] >> (7 - (2 * i + dx))) & 1;
                    raw[g][j * 4 + i] = static_cast<float>(n) * 0.25f;
                    sum += raw[g][j * 4 + i];
                }
            m[g] = sum / kDims;
            top = std::max(top, m[g]);
            idx[g] = g;
        }
        const float scale = 1.0f / (top * 0.97f);   // (a brightness of 1 is what one of the inkiest characters has)
        std::stable_sort(idx, idx + kAsciiGlyphCount, [&](int a, int b) { return m[a] < m[b]; });
        for (int i = 0; i < kAsciiGlyphCount; ++i) {
            mean[i] = m[idx[i]] * scale;
            energy[i] = 0.0f;
            for (int d = 0; d < kDims; ++d) { v[i][d] = (raw[idx[i]][d] - m[idx[i]]) * scale; energy[i] += v[i][d] * v[i][d]; }
        }
    }
};

void fx_ascii(const FxCtx& c) {
    const int w = c.w, h = c.h;
    const int cw = c.braille ? 6 : std::max(4, (w + 50) / 100), ch = 2 * cw;
    const int cols = (w + cw - 1) / cw, rows = (h + ch - 1) / ch;
    static const AsciiSet set;
    // the glyphs at the size of the cell: the 8 x 16 bitmaps averaged over the area every pixel of the cell covers (0 .. 255), a little stronger (thin strokes
    // are lighter than the ink they stand for after the averaging)
    std::vector<uint8_t> atlas(static_cast<size_t>(kAsciiGlyphCount) * cw * ch);
    for (int g = 0; g < kAsciiGlyphCount; ++g)
        for (int py = 0; py < ch; ++py)
            for (int px = 0; px < cw; ++px) {
                const float x0 = static_cast<float>(px) * 8.0f / cw, x1 = static_cast<float>(px + 1) * 8.0f / cw, y0 = static_cast<float>(py) * 16.0f / ch, y1 = static_cast<float>(py + 1) * 16.0f / ch;
                float acc = 0.0f, area = 0.0f;
                for (int sy = static_cast<int>(y0); sy < std::min(16, static_cast<int>(std::ceil(y1))); ++sy) {
                    const float wy = std::min(y1, static_cast<float>(sy + 1)) - std::max(y0, static_cast<float>(sy));
                    for (int sx = static_cast<int>(x0); sx < std::min(8, static_cast<int>(std::ceil(x1))); ++sx) {
                        const float wx = std::min(x1, static_cast<float>(sx + 1)) - std::max(x0, static_cast<float>(sx));
                        acc += wx * wy * static_cast<float>((kAsciiGlyphs[g][sy] >> (7 - sx)) & 1);
                        area += wx * wy;
                    }
                }
                atlas[(static_cast<size_t>(g) * ch + py) * cw + px] = static_cast<uint8_t>(std::min(255.0f, acc / std::max(area, 1e-3f) * 1.3f * 255.0f + 0.5f));
            }
    // the brightness plane (a fine cell sees single pixels: a light 1-2-1 smoothing keeps noise from changing the characters)
    std::vector<float> plane(static_cast<size_t>(w) * h), smooth(static_cast<size_t>(w) * h);
    par_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) for (int x = 0; x < w; ++x) plane[static_cast<size_t>(y) * w + x] = fx_luma(c.at(x, y));
    });
    par_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* up = &plane[static_cast<size_t>(std::max(0, y - 1)) * w];
            const float* mid = &plane[static_cast<size_t>(y) * w];
            const float* dn = &plane[static_cast<size_t>(std::min(h - 1, y + 1)) * w];
            float* o = &smooth[static_cast<size_t>(y) * w];
            for (int x = 0; x < w; ++x) {
                const int xl = std::max(0, x - 1), xr = std::min(w - 1, x + 1);
                o[x] = (4.0f * mid[x] + 2.0f * (mid[xl] + mid[xr] + up[x] + dn[x]) + (up[xl] + up[xr] + dn[xl] + dn[xr])) * 0.0625f;
            }
        }
    });
    // every cell: its mean brightness, and how its brightness changes (the structure tensor of the gradient: how strong, in which direction, and
    // where in the cell the change lies, from top to bottom)
    const size_t ncell = static_cast<size_t>(cols) * static_cast<size_t>(rows);
    std::vector<float> cl(ncell), cs(ncell), cdir(ncell), ccoh(ncell), cpos(ncell);
    par_rows(rows * 32, [&](int a0, int a1) {
        for (int cy = a0 / 32; cy < a1 / 32; ++cy)
            for (int cx = 0; cx < cols; ++cx) {
                const int ox = cx * cw, oy = cy * ch;
                double sum = 0.0, jxx = 0.0, jyy = 0.0, jxy = 0.0, wy = 0.0, wsum = 0.0;
                int n = 0;
                for (int y = oy; y < std::min(h, oy + ch); ++y) {
                    const float* r0 = &smooth[static_cast<size_t>(std::max(0, y - 1)) * w];
                    const float* r1 = &smooth[static_cast<size_t>(y) * w];
                    const float* r2 = &smooth[static_cast<size_t>(std::min(h - 1, y + 1)) * w];
                    for (int x = ox; x < std::min(w, ox + cw); ++x) {
                        const int xl = std::max(0, x - 1), xr = std::min(w - 1, x + 1);
                        const float gx = (r0[xr] - r0[xl]) + 2.0f * (r1[xr] - r1[xl]) + (r2[xr] - r2[xl]);   // (Sobel)
                        const float gy = (r2[xl] - r0[xl]) + 2.0f * (r2[x] - r0[x]) + (r2[xr] - r0[xr]);
                        const double g2 = static_cast<double>(gx) * gx + static_cast<double>(gy) * gy;
                        jxx += static_cast<double>(gx) * gx; jyy += static_cast<double>(gy) * gy; jxy += static_cast<double>(gx) * gy;
                        wy += g2 * (y - oy); wsum += g2;
                        sum += r1[x];
                        ++n;
                    }
                }
                const size_t k = static_cast<size_t>(cy) * static_cast<size_t>(cols) + static_cast<size_t>(cx);
                const double tr = jxx + jyy;
                cl[k] = static_cast<float>(sum / std::max(1, n));
                cs[k] = static_cast<float>(tr / std::max(1, n) / 16.0);   // (Sobel counts a step four times over: in the units of a plain difference)
                ccoh[k] = tr > 1e-6 ? static_cast<float>(std::sqrt((jxx - jyy) * (jxx - jyy) + 4.0 * jxy * jxy) / tr) : 0.0f;
                cdir[k] = static_cast<float>(0.5 * std::atan2(2.0 * jxy, jxx - jyy));   // the direction of the change (radians; 0 = across, from left to right)
                cpos[k] = wsum > 1e-6 ? static_cast<float>(wy / wsum / std::max(1, ch - 1)) : 0.5f;
            }
    });
    // local contrast: a cell's brightness is pushed away from the mean of the cells around it (a box of about a sixteenth of the picture's width), so
    // a white cat in front of a white wall still stands apart from it; the blocks of the cell are moved by the same amount
    std::vector<float> loc(ncell, 0.0f);
    {
        const int R = std::max(2, cols / 16);
        std::vector<float> tmp(ncell);
        for (int cy = 0; cy < rows; ++cy) {   // (across, then down: a box blur in two passes)
            double acc = 0.0;
            int n = 0;
            for (int cx = -R; cx < cols + R; ++cx) {
                if (cx + R < cols) { acc += cl[static_cast<size_t>(cy) * cols + static_cast<size_t>(cx + R)]; ++n; }
                if (cx - R - 1 >= 0) { acc -= cl[static_cast<size_t>(cy) * cols + static_cast<size_t>(cx - R - 1)]; --n; }
                if (cx >= 0 && cx < cols) tmp[static_cast<size_t>(cy) * cols + static_cast<size_t>(cx)] = static_cast<float>(acc / std::max(1, n));
            }
        }
        for (int cx = 0; cx < cols; ++cx) {
            double acc = 0.0;
            int n = 0;
            for (int cy = -R; cy < rows + R; ++cy) {
                if (cy + R < rows) { acc += tmp[static_cast<size_t>(cy + R) * cols + static_cast<size_t>(cx)]; ++n; }
                if (cy - R - 1 >= 0) { acc -= tmp[static_cast<size_t>(cy - R - 1) * cols + static_cast<size_t>(cx)]; --n; }
                if (cy >= 0 && cy < rows) { const size_t k = static_cast<size_t>(cy) * cols + static_cast<size_t>(cx); loc[k] = 1.1f * (cl[k] - static_cast<float>(acc / std::max(1, n))); }
            }
        }
        for (size_t k = 0; k < ncell; ++k) cl[k] = std::clamp(cl[k] + loc[k], 0.0f, 255.0f);
    }
    // the tone spread over the characters: the cells' brightness equalised over the picture (its histogram made flat), half and half with the plain
    // brightness -- a bright picture keeps its shades instead of being one dense character all over
    std::array<float, 256> eq{};
    {
        std::array<double, 256> hist{};
        for (float v : cl) hist[static_cast<size_t>(std::clamp(static_cast<int>(v + 0.5f), 0, 255))] += 1.0;
        // (contrast limited: no shade counts for more than four times its share, the rest is spread evenly -- a plain dark background, one big
        // peak, is not lifted to a middle grey)
        const double limit = 4.0 * static_cast<double>(ncell) / 256.0;
        double excess = 0.0;
        for (double& b : hist) if (b > limit) { excess += b - limit; b = limit; }
        for (double& b : hist) b += excess / 256.0;
        double acc = 0.0;
        for (int i = 0; i < 256; ++i) {
            acc += hist[static_cast<size_t>(i)];
            eq[static_cast<size_t>(i)] = std::clamp(static_cast<float>(0.5 * (acc / static_cast<double>(std::max<size_t>(1, ncell))) * 255.0 + 0.5 * i), 0.0f, 255.0f);
        }
    }
    // the edges: the cells whose change is among the strongest fifth of the picture (and a real one: at least 24 levels across a few pixels), with one
    // clear direction
    float edge_min = 24.0f * 24.0f / 6.0f;
    {
        std::vector<float> sorted(cs);
        const size_t q = sorted.size() * 80 / 100;
        if (q < sorted.size()) { std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(q), sorted.end()); edge_min = std::max(edge_min, sorted[q]); }
    }
    static const int g_bar = [] { for (int g = 0; g < kAsciiGlyphCount; ++g) if (kAsciiGlyphCodes[g] == U'|') return g; return 0; }();
    static const int g_slash = [] { for (int g = 0; g < kAsciiGlyphCount; ++g) if (kAsciiGlyphCodes[g] == U'/') return g; return 0; }();
    static const int g_back = [] { for (int g = 0; g < kAsciiGlyphCount; ++g) if (kAsciiGlyphCodes[g] == U'\\') return g; return 0; }();
    static const int g_dash = [] { for (int g = 0; g < kAsciiGlyphCount; ++g) if (kAsciiGlyphCodes[g] == U'-') return g; return 0; }();
    static const int g_under = [] { for (int g = 0; g < kAsciiGlyphCount; ++g) if (kAsciiGlyphCodes[g] == U'_') return g; return 0; }();
    // the tone the cell's blocks are matched at: a little more contrast, lifted (a dark picture would be only dots otherwise)
    static const std::array<float, 256> tone = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) t[static_cast<size_t>(i)] = std::pow(std::clamp((static_cast<float>(i) / 255.0f - 0.5f) * 1.15f + 0.5f, 0.0f, 1.0f), 0.8f);
        return t;
    }();
    // which pixels of the cell are the block (i, j): at least one each, also where the cell has fewer pixels than blocks
    int bx0[4], bx1[4], by0[8], by1[8];
    for (int i = 0; i < 4; ++i) { bx0[i] = i * cw / 4; bx1[i] = std::max(bx0[i] + 1, (i + 1) * cw / 4); }
    for (int j = 0; j < 8; ++j) { by0[j] = j * ch / 8; by1[j] = std::max(by0[j] + 1, (j + 1) * ch / 8); }
    static const std::array<float, 256> jit = [] { std::array<float, 256> j{}; for (size_t i = 0; i < 256; ++i) j[i] = fx_rand(static_cast<uint32_t>(i), 4242U); return j; }();
    constexpr float kBright = 3.0f * AsciiSet::kDims;   // the weight of the brightness against the shape (the shape decides between the characters that are about as bright)
    constexpr float kJitter = 0.45f;
    constexpr int kWindow = 26;   // characters looked at on each side of the cell's own brightness (in the order of their ink)
    // (par_rows shares out rows of pixels, at least 32 for a thread: a cell row counts for 32, so that the cells are shared out too; every cell is independent)
    par_rows(rows * 32, [&](int a0, int a1) {
        for (int cy = a0 / 32; cy < a1 / 32; ++cy)
            for (int cx = 0; cx < cols; ++cx) {
                const int ox = cx * cw, oy = cy * ch;
                // the brightness of the 4 x 8 blocks
                float t[AsciiSet::kDims];
                float mean = 0.0f;
                for (int j = 0; j < 8; ++j)
                    for (int i = 0; i < 4; ++i) {
                        float sum = 0.0f;
                        int n = 0;
                        for (int y = by0[j]; y < by1[j]; ++y) {
                            const float* row = &smooth[static_cast<size_t>(std::min(h - 1, oy + y)) * w];
                            for (int x = bx0[i]; x < bx1[i]; ++x) { sum += row[std::min(w - 1, ox + x)]; ++n; }
                        }
                        t[j * 4 + i] = tone[static_cast<size_t>(std::clamp(static_cast<int>(eq[static_cast<size_t>(std::clamp(static_cast<int>(sum / static_cast<float>(n) + loc[static_cast<size_t>(cy) * static_cast<size_t>(cols) + static_cast<size_t>(cx)] + 0.5f), 0, 255))] + 0.5f), 0, 255))];
                        mean += t[j * 4 + i];
                    }
                mean *= 1.0f / AsciiSet::kDims;
                float tm = 0.0f;
                for (float& e : t) { e = std::clamp(mean + (e - mean) * 1.6f, 0.0f, 1.0f); tm += e; }   // (a clear shape in the cell counts more than the plain brightness)
                tm *= 1.0f / AsciiSet::kDims;
                // how much of a shape the cell has: a flat cell is matched by its brightness alone (the characters that are about as bright take turns), a cell with
                // an edge or a corner by the shape too
                float var = 0.0f;
                for (float e : t) var += (e - tm) * (e - tm);
                const float shape = std::clamp(var * (1.0f / AsciiSet::kDims) * 30.0f, 0.1f, 1.0f);
                const bool flat = shape <= 0.1f;
                // the characters about as bright as the cell: the one whose pattern of ink is nearest (brightness first, then the shape)
                const int p = static_cast<int>(std::lower_bound(set.mean, set.mean + kAsciiGlyphCount, tm) - set.mean);
                const int lo = std::max(0, p - kWindow), hi = std::min(kAsciiGlyphCount, p + kWindow);
                int best = lo;
                float bd = 1e30f;
                const uint32_t cell_hash = static_cast<uint32_t>(cx) * 73856093U ^ static_cast<uint32_t>(cy) * 19349663U;
                for (int i = lo; i < hi; ++i) {
                    const float* gv = set.v[i];
                    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
                    if (flat) { s0 = 0.0f; }   // (nothing to match: the brightness and the take-turns jitter decide)
                    else for (int d = 0; d < AsciiSet::kDims; d += 4) { s0 += t[d] * gv[d]; s1 += t[d + 1] * gv[d + 1]; s2 += t[d + 2] * gv[d + 2]; s3 += t[d + 3] * gv[d + 3]; }
                    const float dm = set.mean[i] - tm;
                    // (a little jitter, fixed for the cell and the glyph: where several characters fit about equally, as in a flat area, they take turns
                    // and the picture has the varied look of text; the same for every frame, so a still picture stays still)
                    const float dist = kBright * dm * dm + shape * (set.energy[i] - 2.0f * ((s0 + s1) + (s2 + s3))) + kJitter * jit[(cell_hash + static_cast<uint32_t>(i) * 37U) & 255U];
                    if (dist < bd) { bd = dist; best = i; }
                }
                int glyph = tm < 0.05f ? 0 : set.idx[best];   // (a dark cell stays empty: ASCII art leaves the dark ground blank)
                const size_t k = static_cast<size_t>(cy) * static_cast<size_t>(cols) + static_cast<size_t>(cx);
                if (cs[k] >= edge_min && ccoh[k] > 0.55f && !c.braille) {   // an edge: a line along it (across the direction of the change)
                    const float deg = cdir[k] * 57.29578f;                   // -90 .. 90; 0 = the change goes from left to right (a vertical edge)
                    if (deg > -22.5f && deg < 22.5f) glyph = g_bar;
                    else if (deg >= 67.5f || deg <= -67.5f) glyph = cpos[k] > 0.62f ? g_under : g_dash;
                    else glyph = deg > 0.0f ? g_slash : g_back;   // (y grows downwards: a change towards the lower right (or upper left) lies along a line from the lower left to the upper right: /)
                }
                const uint8_t* cov = &atlas[static_cast<size_t>(glyph) * cw * ch];
                // the colour of the cell (its mean), the character takes it a little more colourful; bright cells glow, dark ones are dim
                float rgb[3] = {0, 0, 0};
                int n = 0;
                for (int y = oy; y < std::min(h, oy + ch); ++y)
                    for (int x = ox; x < std::min(w, ox + cw); ++x) { const uint8_t* s = c.at(x, y); rgb[0] += s[0]; rgb[1] += s[1]; rgb[2] += s[2]; ++n; }
                for (float& e : rgb) e /= static_cast<float>(std::max(1, n));
                const float lum = (0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2]) / 255.0f;
                const float mx = std::max({rgb[0], rgb[1], rgb[2], 24.0f});
                const float target = 255.0f * (0.45f + 0.55f * std::pow(lum, 0.6f));   // (the characters of a dark part are dimmer: the ground stays calm)
                float fg[3];
                for (int k = 0; k < 3; ++k) {
                    const float chroma = rgb[k] / mx;
                    fg[k] = std::clamp((lum + (chroma - lum) * 1.3f) * target, 0.0f, 255.0f);
                    fg[k] = std::max(fg[k], chroma * target * 0.85f);
                }
                const float bg[3] = {fg[0] * 0.05f + 4.0f, fg[1] * 0.05f + 6.0f, fg[2] * 0.05f + 5.0f};
                for (int y = oy; y < std::min(h, oy + ch); ++y) {
                    uint8_t* q = c.out(ox, y);
                    const uint8_t* cr = cov + static_cast<size_t>(y - oy) * cw;
                    for (int x = ox; x < std::min(w, ox + cw); ++x, q += 3) {
                        const float a = static_cast<float>(cr[x - ox]) * (1.0f / 255.0f);
                        for (int k = 0; k < 3; ++k) q[k] = fx_clamp(bg[k] + (fg[k] - bg[k]) * a);
                    }
                }
            }
    });
}
