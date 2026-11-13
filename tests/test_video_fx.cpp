// The looks of the video view share the rows of a picture between threads: the result must be the same as with one thread, for every look.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include "test_framework.h"
#include "video_player.h"
#include "settings.h"

using namespace muisc;

TEST(video_looks_give_the_same_picture_with_one_or_four_threads) {
    const int W = 301, H = 171;
    VideoFrame f;
    f.w = W; f.h = H; f.rgb.resize(static_cast<size_t>(W) * H * 3);
    uint32_t s = 12345;
    for (size_t i = 0; i < f.rgb.size(); ++i) { s = s * 1664525u + 1013904223u; f.rgb[i] = static_cast<uint8_t>((s >> 24) * (i % 97 < 50 ? 1 : 0) + (i / 3 % W) / 4); }
    VideoFrame prev = f;
    for (auto& b : prev.rgb) b ^= 5;
    for (int braille = 0; braille < 2; ++braille)
        for (int fx = 0; fx < kVideoFxCount; ++fx) {
            VideoLook l;
            l.fx = fx; l.contrast = 20; l.colors = 70; l.brightness = 55; l.chroma = fx % 4;
            VideoFrame a = f, b = f;
            VideoPlayer::set_fx_threads(1);
            VideoPlayer::apply_look(a, l, 1.7, braille != 0, &prev);
            std::vector<uint8_t> ha, la, hb, lb;
            VideoPlayer::quantize(a, ha, la);
            VideoPlayer::set_fx_threads(4);
            VideoPlayer::apply_look(b, l, 1.7, braille != 0, &prev);
            VideoPlayer::quantize(b, hb, lb);
            CHECK(a.rgb == b.rgb);
            CHECK(ha == hb && la == lb);
        }
    VideoPlayer::set_fx_threads(0);
}

// The 256-colour step must not draw a lattice: a smooth ramp between two cube levels, dithered with the old 4 x 4 Bayer matrix, repeated
// exactly every 4 pixels in both directions (a visible raster of little squares). Blue noise does not.
TEST(quantize_dithers_a_smooth_area_without_a_repeating_4x4_lattice) {
    const int W = 64, H = 64;
    VideoFrame f;
    f.w = W; f.h = H; f.rgb.resize(static_cast<size_t>(W) * H * 3);
    for (int i = 0; i < W * H; ++i) { f.rgb[i * 3] = 200; f.rgb[i * 3 + 1] = 60; f.rgb[i * 3 + 2] = 160; }   // between cube levels in every channel
    std::vector<uint8_t> hue, level;
    VideoPlayer::quantize(f, hue, level);
    int same = 0, total = 0;
    for (int y = 0; y + 4 < H; ++y)
        for (int x = 0; x + 4 < W; ++x) { ++total; if (hue[static_cast<size_t>(y) * W + x] == hue[static_cast<size_t>(y + 4) * W + x] && hue[static_cast<size_t>(y) * W + x] == hue[static_cast<size_t>(y) * W + x + 4]) ++same; }
    CHECK(same < total * 9 / 10);   // Bayer 4x4: 100 % (every pixel equals the one 4 away); blue noise: well below
    std::vector<int> seen(256, 0);
    for (uint8_t h : hue) seen[h] = 1;
    int kinds = 0;
    for (int v : seen) kinds += v;
    CHECK(kinds >= 2);   // (it still dithers: the mean tone is kept)
}

// The dither patterns (Video Dither Pattern): every one keeps the average tone, they differ from each other, and each stays the same from one
// frame to the next (position only: no shimmer).
TEST(dither_patterns_keep_the_tone_and_differ_from_each_other) {
    const int W = 128, H = 128;
    auto flat_grey = [&](int v) {
        VideoFrame f;
        f.w = W; f.h = H; f.rgb.assign(static_cast<size_t>(W) * H * 3, static_cast<uint8_t>(v));
        return f;
    };
    std::vector<std::vector<uint8_t>> out, flat;
    auto ramp = [&]() {   // every tone from black to white: the patterns cannot agree by chance at one tone
        VideoFrame f;
        f.w = W; f.h = H; f.rgb.resize(static_cast<size_t>(W) * H * 3);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) for (int k = 0; k < 3; ++k) f.rgb[(static_cast<size_t>(y) * W + x) * 3 + k] = static_cast<uint8_t>(x * 2);
        return f;
    };
    for (int d = 0; d < kDitherPatterns; ++d) {
        VideoFrame f = flat_grey(128);
        VideoPlayer::apply_fx(f, 21, 0.0, false, nullptr, nullptr, d);   // Dither B/W: one bit per pixel
        int on = 0;
        for (size_t i = 0; i < f.rgb.size(); i += 3) on += f.rgb[i] > 128 ? 1 : 0;
        const double share = static_cast<double>(on) / (W * H);
        CHECK(share > 0.38 && share <= 0.5001);   // (a mid grey: about the same share of white dots with every pattern)
        VideoFrame g = flat_grey(128);
        VideoPlayer::apply_fx(g, 21, 7.3, false, nullptr, nullptr, d);   // another moment: the same dots
        CHECK(f.rgb == g.rgb);
        flat.push_back(f.rgb);
        VideoFrame r = ramp();
        VideoPlayer::apply_fx(r, 21, 0.0, false, nullptr, nullptr, d);
        out.push_back(r.rgb);
    }
    CHECK(out[0] == out[4]);                  // auto is Bayer 8x8 for this look: what it always was
    for (int a = 1; a < kDitherPatterns; ++a)
        for (int b = a + 1; b < kDitherPatterns; ++b)
            CHECK(out[static_cast<size_t>(a)] != out[static_cast<size_t>(b)]);
    // the regular patterns repeat, blue noise does not repeat within 8 pixels
    auto shifted_same = [&](int d, int dx) {
        int same = 0, total = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x + dx < W; ++x) { ++total; if (flat[static_cast<size_t>(d)][(static_cast<size_t>(y) * W + x) * 3] == flat[static_cast<size_t>(d)][(static_cast<size_t>(y) * W + x + dx) * 3]) ++same; }
        return static_cast<double>(same) / total;
    };
    CHECK(shifted_same(2, 2) > 0.999);        // bayer 2x2: every 2 pixels
    CHECK(shifted_same(3, 4) > 0.999);        // bayer 4x4: every 4
    CHECK(shifted_same(4, 8) > 0.999);        // bayer 8x8: every 8
    CHECK(shifted_same(1, 8) < 0.9);          // blue noise: no repeat there
}

TEST(video_look_signature_changes_with_the_dither_pattern) {
    VideoLook a, b;
    a.fx = 20; b.fx = 20; b.dither = 5;
    CHECK(a.sig() != b.sig());   // (the picture is made again when the pattern changes)
    CHECK(!b.active() || b.active() == a.active());   // (the pattern alone does not make a look "active")
}

// Video Gradient Saturation: 100 = the gradient's colours as they are, lower = paler, 0 = grey of the same brightness; the look's signature changes with
// it (the picture is made again) and the default member is 100.
TEST(video_gradient_saturation_pales_the_gradient_colours_and_100_is_the_old_look) {
    auto make = [&](int sat) {
        VideoFrame f;
        f.w = 96; f.h = 64; f.rgb.assign(static_cast<size_t>(f.w) * f.h * 3, 220);
        VideoFxGradient g;
        const unsigned char c[3][3] = {{150, 0, 200}, {75, 100, 200}, {0, 200, 200}};
        for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) g.c[i][k] = c[i][k];
        g.sat = sat;
        VideoPlayer::apply_fx(f, 6, 0.0, false, &g, nullptr);
        return f;
    };
    auto max_sat = [&](const VideoFrame& f) {   // the most saturated pixel
        double hi = 0.0;
        for (size_t i = 0; i < f.rgb.size(); i += 3) {
            const double mx = std::max<int>(f.rgb[i], std::max<int>(f.rgb[i + 1], f.rgb[i + 2])), mn = std::min<int>(f.rgb[i], std::min<int>(f.rgb[i + 1], f.rgb[i + 2]));
            if (mx > 30) hi = std::max(hi, (mx - mn) / mx);
        }
        return hi;
    };
    const double full = max_sat(make(100)), mid = max_sat(make(50)), none = max_sat(make(0));
    CHECK(full > 0.6);
    CHECK(mid < full - 0.1);
    CHECK(none < 0.08);                      // (grey)
    VideoFxGradient dflt;
    CHECK_EQ(dflt.sat, 100);                 // (a gradient nobody set is the colours as they are)
    VideoLook a, b;
    a.fx = 6; b.fx = 6; b.grad.sat = 40;
    CHECK(a.sig() != b.sig());
}

// The effect list: every effect that is offered is listed once, the names are unique and read back, and the new looks keep their numbers.
TEST(video_fx_list_names_and_order) {
    std::vector<int> seen;
    for (int i = 0; i < kVideoFxShown; ++i) {
        const int fx = kVideoFxOrder[i];
        CHECK(fx >= 0 && fx < kVideoFxCount);
        CHECK(std::find(seen.begin(), seen.end(), fx) == seen.end());
        seen.push_back(fx);
        CHECK_EQ(video_fx_from(video_fx_name(fx)), fx);
    }
    CHECK_EQ(video_fx_from("phosphor green glow"), 26);
    CHECK_EQ(video_fx_from("cga"), 27);
    CHECK_EQ(video_fx_from("c64"), 28);
    CHECK_EQ(video_fx_from("night vision"), 0);   // (gone again: a setting of it reads as Off)
    CHECK_EQ(video_fx_canon(kVideoFxCount), 0);   // (a number that is no effect reads as Off)
    CHECK_EQ(video_fx_from("zx spectrum"), 29);
    CHECK_EQ(video_fx_from("amstrad cpc"), 30);
    CHECK_EQ(video_fx_from("teletext"), 31);
    CHECK_EQ(video_fx_from("amiga ham"), 32);
    CHECK_EQ(video_fx_from("silent film"), 33);
    CHECK_EQ(video_fx_from("technicolor"), 34);
    CHECK_EQ(video_fx_from("vector"), 35);
    CHECK_EQ(video_fx_from("blueprint"), 0);      // (gone: a setting of it reads as Off)
    CHECK_EQ(video_fx_canon(36), 0);
    for (int i = 0; i < kVideoFxShown; ++i) CHECK(kVideoFxOrder[i] != 36);
    CHECK_EQ(video_fx_from("ascii"), 37);
    CHECK_EQ(video_fx_from("thermal"), 38);
    CHECK_EQ(video_fx_from("risograph"), 39);
    CHECK_EQ(video_fx_from("laserdisc"), 40);
    CHECK_EQ(video_fx_from("halftone cmyk"), 41);
    CHECK_EQ(video_fx_from("halftone cmyk high-res"), 42);
    CHECK(video_fx_pos(42) == video_fx_pos(41) + 1);   // the fine screen sits right below the plain one
    CHECK_EQ(kVideoFxShown, 40);
    CHECK_EQ(video_fx_canon(-1), -1);   // (-1 = "as in Settings" stays)
    CHECK(video_fx_pos(26) == video_fx_pos(8) + 1);   // the glow variant sits right below Phosphor Green
    CHECK_EQ(video_fx_step(28, 1), kVideoFxOrder[(video_fx_pos(28) + 1) % kVideoFxShown]);
}

// The base layer: another structure under the tube looks. 0 is what the looks always drew; the others replace the lines.
TEST(video_base_layers) {
    const int W = 480, H = 270;
    VideoFrame f;
    f.w = W; f.h = H; f.rgb.assign(static_cast<size_t>(W) * H * 3, 180);   // a flat grey: any structure is the layer's
    auto make = [&](int fx, int base) { VideoFrame a = f; VideoPlayer::apply_fx(a, fx, 0.0, false, nullptr, nullptr, 0, base); return a; };
    auto col_var = [&](const VideoFrame& a) {   // how much the pixels of one row differ from each other
        int lo = 255, hi = 0;
        for (int x = 200; x < 260; ++x) { const int v = a.rgb[(static_cast<size_t>(100) * W + x) * 3 + 1]; lo = std::min(lo, v); hi = std::max(hi, v); }
        return hi - lo;
    };
    auto row_var = [&](const VideoFrame& a) {   // how much the rows of one column differ from each other
        int lo = 255, hi = 0;
        for (int y = 90; y < 120; ++y) { const int v = a.rgb[(static_cast<size_t>(y) * W + 200) * 3 + 1]; lo = std::min(lo, v); hi = std::max(hi, v); }
        return hi - lo;
    };
    const VideoFrame lines = make(2, 0), grille = make(2, 1), mask = make(2, 2), slot = make(2, 3), clean = make(2, 4);
    CHECK(row_var(lines) > 20);      // scanlines: the rows differ
    CHECK(col_var(grille) > 40);     // an aperture grille: vertical stripes
    CHECK(col_var(mask) > 20);       // a shadow mask: dots
    CHECK(col_var(slot) > 40);
    CHECK(row_var(clean) < 6 && col_var(clean) < 20);   // clean: the vignette's slope only, no structure
    CHECK(grille.rgb != lines.rgb && mask.rgb != grille.rgb && clean.rgb != lines.rgb);
    // Scanlines itself (1) and the effects that do not use the layer ignore it
    CHECK(make(1, 3).rgb == make(1, 0).rgb);
    CHECK(make(21, 3).rgb == make(21, 0).rgb);
    // the aperture grille has no horizontal lines (the damper wires were taken out): a row of a flat picture is as bright as its neighbours
    for (int yy : {H / 3, 2 * H / 3}) {
        int up = 0, mid = 0, dn = 0;
        for (int x = 200; x < 260; ++x) {
            up += grille.rgb[(static_cast<size_t>(yy - 1) * W + x) * 3 + 1]; mid += grille.rgb[(static_cast<size_t>(yy) * W + x) * 3 + 1]; dn += grille.rgb[(static_cast<size_t>(yy + 1) * W + x) * 3 + 1];
        }
        CHECK(std::abs(mid - (up + dn) / 2) <= 60 * 3);   // (the vignette's slope only: a wire would be about 30 % darker = 60 * 15 less)
    }
    // deterministic, and the same with one thread or four
    for (int fx : {2, 3, 4, 7, 8, 9, 26})
        for (int base = 1; base < kBaseLayers; ++base) {
            VideoPlayer::set_fx_threads(1);
            const VideoFrame a = make(fx, base);
            VideoPlayer::set_fx_threads(4);
            const VideoFrame b = make(fx, base);
            CHECK(a.rgb == b.rgb);
        }
    VideoPlayer::set_fx_threads(0);
    // the look's signature changes with the layer only for the looks that use it
    VideoLook l; l.fx = 2; const std::string s0 = l.sig(); l.base = 2;
    CHECK(l.sig() != s0);
    l.fx = 21; const std::string t0 = l.sig(); l.base = 0;
    CHECK_EQ(l.sig(), t0);
}

// Clean is the default; the old "lcd grid" (gone) reads as clean, a number or a name of the five layers as itself.
TEST(video_base_layer_default_and_names) {
    CHECK_EQ(kBaseLayers, 5);
    CHECK_EQ(base_from("clean"), 4);
    CHECK_EQ(base_from("lcd grid"), 4);
    CHECK_EQ(base_from("scanlines"), 0);
    CHECK_EQ(base_from("slot mask"), 3);
    CHECK_EQ(Settings().video_base, 4);
    CHECK_EQ(VideoLook().base, 4);
}

// The Effect Contrast: how hard the blacks and whites of the photographic looks are. The palette and mask looks never change; the default
// (reduced) is soft for all the others (the dark parts keep a tint / no black holes), "full" is the hard look they had, "low" the softest.
TEST(video_effect_contrast_softens_only_the_photographic_looks) {
    CHECK_EQ(kFxContrasts, 3);
    CHECK_EQ(kFxContrastDefault, 1);
    CHECK_EQ(fxc_from("full"), 0); CHECK_EQ(fxc_from("reduced"), 1); CHECK_EQ(fxc_from("low"), 2);
    CHECK_EQ(fxc_from("auto"), 1); CHECK_EQ(fxc_from("nonsense"), 1); CHECK_EQ(fxc_from("2"), 1);   // (anything unknown, also the "auto" of the first version: the default)
    for (int i = 0; i < kFxContrasts; ++i) CHECK_EQ(fxc_from(fxc_name(i)), i);
    CHECK_EQ(VideoLook().fxc, 1);
    CHECK_EQ(Settings().video_fxc, 1);
    // the palette, mask and line looks need their hard blacks: no tone at any level
    for (int fx : {8, 9, 10, 12, 13, 16, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 35, 37, 38, 39, 41, 42})
        for (int level = 0; level < kFxContrasts; ++level) {
            const FxTone t = video_fx_tone(fx, level);
            CHECK(!video_fx_softens(fx));
            CHECK(t.lift == 0.0f && t.cap == 0.0f && t.floor == 0.0f && t.flash == 1.0f);
        }
    for (int fx : {1, 2, 3, 4, 5, 6, 7, 15, 17, 33, 34, 40}) CHECK(video_fx_softens(fx));
    // the default is the same soft tone for every look that can be soft (a lift of 16 levels, the white lowered by 6, a shade floor, softer flashes)
    for (int fx : {1, 2, 3, 4, 5, 6, 7, 15, 17, 33, 34, 40}) {
        const FxTone d = video_fx_tone(fx, kFxContrastDefault);
        CHECK(d.lift == 16.0f && d.cap == 6.0f && d.floor > 0.0f && d.gamma < 1.15f && d.flash < 1.0f);
        CHECK(d.lift == video_fx_tone(fx, 1).lift);
    }
    // the levels grow softer: full (nothing) < reduced < low, for every look that can be soft
    for (int fx : {1, 2, 3, 4, 5, 6, 7, 15, 17, 33, 34, 40}) {
        const FxTone f = video_fx_tone(fx, 0), r = video_fx_tone(fx, 1), l = video_fx_tone(fx, 2);
        CHECK(f.lift == 0.0f && f.cap == 0.0f && f.floor == 0.0f && f.flash == 1.0f);
        CHECK(r.lift > f.lift && l.lift > r.lift && l.cap > r.cap && l.floor > r.floor && l.gamma < r.gamma && l.flash < r.flash);
    }
    // the signature (the picture is made again when the level changes) follows the level only for the looks that read it
    VideoLook a; a.fx = 6; const std::string s6 = a.sig(); a.fxc = 2;
    CHECK(a.sig() != s6);
    VideoLook b; b.fx = 12; const std::string s12 = b.sig(); b.fxc = 2;
    CHECK_EQ(b.sig(), s12);
}

// The pictures: the dark parts of a Gradient picture no longer fall to black, the Glitch has no black holes and no pure white, "full" is the old
// hard look, and a palette look is the same at every level.
TEST(video_effect_contrast_changes_the_pictures) {
    const int W = 160, H = 90;
    auto flat = [&](int v) { VideoFrame f; f.w = W; f.h = H; f.rgb.assign(static_cast<size_t>(W) * H * 3, static_cast<uint8_t>(v)); return f; };
    auto look = [&](VideoFrame f, int fx, int level, double t = 0.0) { VideoLook l; l.fx = fx; l.fxc = level; VideoPlayer::apply_look(f, l, t, false, nullptr); return f; };
    auto mean = [&](const VideoFrame& f) { double s = 0; for (uint8_t b : f.rgb) s += b; return s / static_cast<double>(f.rgb.size()); };
    // Gradient: a dark picture (14 of 255) came out nearly black; now (reduced, the default) it keeps a tint, and "low" is lighter still
    const double g_full = mean(look(flat(14), 6, 0)), g_def = mean(look(flat(14), 6, kFxContrastDefault)), g_low = mean(look(flat(14), 6, 2));
    CHECK(g_full < 20.0);
    CHECK(g_def > g_full + 15.0);
    CHECK(g_low > g_def);
    // ... and the light parts stay light: the distance between a dark and a light picture is smaller than in the hard look, but still large
    const double span_full = mean(look(flat(220), 6, 0)) - g_full, span_def = mean(look(flat(220), 6, kFxContrastDefault)) - g_def;
    CHECK(span_def < span_full && span_def > 0.5 * span_full);
    // "full" is the look it was: no lift (a black picture stays black); the tube looks lift the blacks by 16 levels by default
    CHECK(mean(look(flat(0), 6, 0)) == 0.0);
    {
        const VideoFrame a = look(flat(0), 1, kFxContrastDefault), b = look(flat(0), 1, 0);   // Scanlines
        for (uint8_t v : a.rgb) CHECK_EQ(static_cast<int>(v), 16);
        for (uint8_t v : b.rgb) CHECK_EQ(static_cast<int>(v), 0);
    }
    // Glitch (default): a black picture has no black left and no pure white, whatever the flashes and specks do
    int hi_def = 0, lo_def = 255, hi_full = 0, lo_full = 255;
    double row_full = 0, row_def = 0;   // the brightest row of a dark picture: a slice that flashes in negative
    for (int i = 0; i < 320; ++i) {
        const double t = i * 0.125;
        const VideoFrame a = look(flat(10), 4, kFxContrastDefault, t), f = look(flat(10), 4, 0, t);
        for (uint8_t v : a.rgb) { hi_def = std::max<int>(hi_def, v); lo_def = std::min<int>(lo_def, v); }
        for (uint8_t v : f.rgb) { hi_full = std::max<int>(hi_full, v); lo_full = std::min<int>(lo_full, v); }
        for (int y = 0; y < H; ++y) {
            double sa = 0, sf = 0;
            for (int x = 0; x < W; ++x) for (int k = 0; k < 3; ++k) { sa += a.rgb[(static_cast<size_t>(y) * W + x) * 3 + k]; sf += f.rgb[(static_cast<size_t>(y) * W + x) * 3 + k]; }
            row_def = std::max(row_def, sa / (3.0 * W)); row_full = std::max(row_full, sf / (3.0 * W));
        }
    }
    CHECK(lo_def >= 16 && hi_def <= 249);                        // (the picture's 10 + a lift of 16 - ...; the white lowered by 6)
    CHECK(hi_full == 255);                                       // (the specks are pure white / colour)
    CHECK(row_full > 225.0);                                     // a true negative of a dark picture: a bright slab ...
    CHECK(row_def < 215.0 && row_def > 100.0);                   // ... that is softer, but still a flash
    // a palette look is the same at every level (Game Boy, Dither B/W, ASCII), a photographic one is not
    VideoFrame ramp; ramp.w = W; ramp.h = H; ramp.rgb.resize(static_cast<size_t>(W) * H * 3);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) for (int k = 0; k < 3; ++k) ramp.rgb[(static_cast<size_t>(y) * W + x) * 3 + k] = static_cast<uint8_t>(x * 255 / (W - 1));
    for (int fx : {12, 21, 37, 13, 38}) CHECK(look(ramp, fx, 0).rgb == look(ramp, fx, 2).rgb);
    CHECK(look(ramp, 6, 0).rgb != look(ramp, 6, 2).rgb);
    CHECK(look(ramp, 2, 0).rgb != look(ramp, 2, 2).rgb);
    // deterministic, and the same with one thread or four
    VideoPlayer::set_fx_threads(1);
    const VideoFrame one = look(ramp, 6, kFxContrastDefault, 1.2);
    VideoPlayer::set_fx_threads(4);
    CHECK(look(ramp, 6, kFxContrastDefault, 1.2).rgb == one.rgb);
    VideoPlayer::set_fx_threads(0);
}

// The phosphor looks are tinted all the way down: the black of a tube is faintly green (or amber), not grey; the glow variant keeps square corners (no curved glass, no black bezel).
TEST(video_phosphor_is_tinted_and_the_glow_has_square_corners) {
    const int W = 320, H = 180;
    VideoFrame black;
    black.w = W; black.h = H; black.rgb.assign(static_cast<size_t>(W) * H * 3, 0);
    for (int fx : {8, 26, 9}) {
        VideoFrame a = black;
        VideoPlayer::apply_look(a, [&] { VideoLook l; l.fx = fx; return l; }(), 0.0);
        const size_t i = (static_cast<size_t>(H / 2) * W + W / 2) * 3;
        if (fx == 9) CHECK(a.rgb[i] > a.rgb[i + 1] && a.rgb[i + 1] > a.rgb[i + 2]);   // amber: red > green > blue
        else CHECK(a.rgb[i + 1] > a.rgb[i] && a.rgb[i + 1] > a.rgb[i + 2]);           // green
    }
    VideoFrame white = black;
    std::fill(white.rgb.begin(), white.rgb.end(), 255);
    VideoFrame g = white;
    VideoPlayer::apply_fx(g, 26, 0.0);
    CHECK(g.rgb[1] > 60);                                                   // the corner pixel is lit (a vignette only dims it): no black rounded corner
    const size_t mid = (static_cast<size_t>(H / 2) * W + W / 2) * 3;
    CHECK(g.rgb[mid + 1] > 200);                                              // the middle is bright green
    CHECK(g.rgb[mid + 1] > g.rgb[mid] + 20);   // (burnt towards white at full drive, but still green)
}

// The looks of old computers follow their hardware: the Spectrum has two colours in every 8 x 8 cell, the CPC three levels per colour gun, teletext two
// colours in every 2 x 3 character, HAM four bits a colour, the pixel look 32 colours, ASCII a foreground and a background in every cell.
namespace {
VideoFrame retro_test_picture(int W, int H) {   // smooth colour fields: a hue sweep across, a brightness ramp down
    VideoFrame f;
    f.w = W; f.h = H; f.rgb.resize(static_cast<size_t>(W) * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double hue = static_cast<double>(x) / W * 6.0;
            const double r = std::clamp(std::fabs(hue - 3.0) - 1.0, 0.0, 1.0), g = std::clamp(2.0 - std::fabs(hue - 2.0), 0.0, 1.0), b = std::clamp(2.0 - std::fabs(hue - 4.0), 0.0, 1.0);
            const double k = 0.25 + 0.75 * (1.0 - static_cast<double>(y) / H);
            uint8_t* q = &f.rgb[(static_cast<size_t>(y) * W + x) * 3];
            q[0] = static_cast<uint8_t>(255 * r * k); q[1] = static_cast<uint8_t>(255 * g * k); q[2] = static_cast<uint8_t>(255 * b * k);
        }
    return f;
}
int colours_in(const VideoFrame& f, int x0, int y0, int x1, int y1) {   // how many different colours are in a rectangle
    std::set<int> s;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) { const uint8_t* q = &f.rgb[(static_cast<size_t>(y) * f.w + x) * 3]; s.insert(q[0] << 16 | q[1] << 8 | q[2]); }
    return static_cast<int>(s.size());
}
}  // namespace

TEST(video_computer_looks_follow_their_hardware) {
    {   // ZX Spectrum: 256 x 192, at most two colours in every 8 x 8 cell, and only the 15 colours of the machine
        VideoFrame f = retro_test_picture(256, 192);
        VideoPlayer::apply_fx(f, 29, 0.0);
        bool ok = true;
        for (int cy = 0; cy < 24; ++cy) for (int cx = 0; cx < 32; ++cx) if (colours_in(f, cx * 8, cy * 8, cx * 8 + 8, cy * 8 + 8) > 2) ok = false;
        CHECK(ok);   // (the colour clash)
        for (size_t i = 0; i < f.rgb.size(); i += 3)
            for (int k = 0; k < 3; ++k) CHECK(f.rgb[i + static_cast<size_t>(k)] == 0 || f.rgb[i + static_cast<size_t>(k)] == 205 || f.rgb[i + static_cast<size_t>(k)] == 255);
        CHECK(colours_in(f, 0, 0, 256, 192) > 6);   // (and it is not a single colour)
    }
    {   // Amstrad CPC: three levels per colour gun
        VideoFrame f = retro_test_picture(320, 200);
        VideoPlayer::apply_fx(f, 30, 0.0);
        for (uint8_t v : f.rgb) CHECK(v == 0 || v == 127 || v == 255);
    }
    {   // Teletext: eight colours, two in every 2 x 3 character of the 40 x 24 grid (320 x 240: 8 x 10 pixels)
        VideoFrame f = retro_test_picture(320, 240);
        VideoPlayer::apply_fx(f, 31, 0.0);
        for (uint8_t v : f.rgb) CHECK(v == 0 || v == 255);
        bool ok = true;
        for (int cy = 0; cy < 24; ++cy) for (int cx = 0; cx < 40; ++cx) if (colours_in(f, cx * 8, cy * 10, cx * 8 + 8, cy * 10 + 10) > 2) ok = false;
        CHECK(ok);
    }
    {   // Amiga HAM: four bits a colour; a smooth picture needs more than 16 colours (the modified ones), which a plain palette look could not give
        VideoFrame f = retro_test_picture(320, 180);
        VideoPlayer::apply_fx(f, 32, 0.0);
        for (uint8_t v : f.rgb) CHECK(v % 17 == 0);
        CHECK(colours_in(f, 0, 0, 320, 180) > 16);
    }
    {   // Pixel: the 32 colours of the palette
        VideoFrame f = retro_test_picture(320, 180);
        VideoPlayer::apply_fx(f, 5, 0.0);
        CHECK(colours_in(f, 0, 0, 320, 180) <= 32);
    }
    {   // ASCII: about 100 characters across (cells of 5 x 10 pixels at 480 wide: big enough to read), many different characters, and in every cell one
        // colour on its dark background
        VideoFrame f = retro_test_picture(480, 270);
        VideoPlayer::apply_fx(f, 37, 0.0);
        std::set<uint64_t> shapes;
        bool ok = true;
        for (int cy = 0; cy < 27; ++cy)
            for (int cx = 0; cx < 96; ++cx) {
                int lo = 255 * 3, hi = 0;
                for (int y = 0; y < 10; ++y) for (int x = 0; x < 5; ++x) { const uint8_t* q = &f.rgb[(static_cast<size_t>(cy * 10 + y) * 480 + cx * 5 + x) * 3]; const int v = q[0] + q[1] + q[2]; lo = std::min(lo, v); hi = std::max(hi, v); }
                uint64_t mask = 0;
                for (int y = 0; y < 10; ++y) for (int x = 0; x < 5; ++x) { const uint8_t* q = &f.rgb[(static_cast<size_t>(cy * 10 + y) * 480 + cx * 5 + x) * 3]; mask = mask * 2 + (q[0] + q[1] + q[2] > (lo + hi) / 2 ? 1 : 0); }
                if (hi - lo > 60) shapes.insert(mask);
                if (lo > 120) ok = false;   // (the darkest pixel of a cell is the dark background: no cell is lit everywhere)
            }
        CHECK(ok);
        CHECK(shapes.size() >= 30);   // (dozens of different characters on a smooth picture)
    }
}

TEST(video_print_and_display_looks_have_their_signature) {
    const int W = 320, H = 180;
    auto flat = [&](int r, int g, int b) { VideoFrame f; f.w = W; f.h = H; f.rgb.resize(static_cast<size_t>(W) * H * 3); for (size_t i = 0; i < f.rgb.size(); i += 3) { f.rgb[i] = static_cast<uint8_t>(r); f.rgb[i + 1] = static_cast<uint8_t>(g); f.rgb[i + 2] = static_cast<uint8_t>(b); } return f; };
    auto mean = [&](const VideoFrame& f, int k) { double s = 0; for (size_t i = static_cast<size_t>(k); i < f.rgb.size(); i += 3) s += f.rgb[i]; return s / (static_cast<double>(f.rgb.size()) / 3.0); };
    {   // Vector: a flat picture has no edges: black, lit only faintly
        VideoFrame f = flat(180, 120, 90);
        VideoPlayer::apply_fx(f, 35, 0.0);
        CHECK(mean(f, 0) < 12 && mean(f, 1) < 12 && mean(f, 2) < 12);
    }
    {   // Blueprint is no longer offered: its number reads as Off and the picture is left as it is
        VideoFrame f = flat(120, 120, 120), g = f;
        VideoPlayer::apply_fx(g, 36, 0.0);
        CHECK(g.rgb == f.rgb);
    }
    {   // Silent film: sepia (red over green over blue), whatever the colours were
        VideoFrame f = flat(60, 120, 200);
        VideoPlayer::apply_fx(f, 33, 0.0);
        CHECK(mean(f, 0) > mean(f, 1) && mean(f, 1) > mean(f, 2));
    }
    {   // Thermal: black stays dark, white goes to the hot end (white-yellow)
        VideoFrame dark = flat(0, 0, 0), lit = flat(255, 255, 255);
        VideoPlayer::apply_fx(dark, 38, 0.0);
        VideoPlayer::apply_fx(lit, 38, 0.0);
        const uint8_t* d = &dark.rgb[(static_cast<size_t>(10) * W + 10) * 3];
        const uint8_t* l = &lit.rgb[(static_cast<size_t>(10) * W + 10) * 3];
        CHECK(d[0] < 40 && d[1] < 20 && d[2] < 60);
        CHECK(l[0] > 220 && l[1] > 200);
        // nothing is drawn over the picture: no temperature scale at the right edge, no crosshair in the middle -- every pixel of a flat picture is
        // the same as the corner (up to the noise: blue changes fast at the hot end of the palette)
        for (int y = H / 8; y < H - H / 8; y += 7) {
            const uint8_t* e = &lit.rgb[(static_cast<size_t>(y) * W + W - W / 40 - 8) * 3];
            CHECK(std::abs(e[0] - l[0]) < 12 && std::abs(e[1] - l[1]) < 12 && std::abs(e[2] - l[2]) < 45);
        }
        for (int y = H / 2 - H / 8; y <= H / 2 + H / 8; ++y)
            for (int x = W / 2 - W / 8; x <= W / 2 + W / 8; ++x) {
                const uint8_t* m = &lit.rgb[(static_cast<size_t>(y) * W + x) * 3];
                CHECK(m[0] > 200 && m[1] > 180);   // (a crosshair had dark edges and a 245 grey: the hot end is yellow-white, never grey, never dark)
            }
        {   // the same on a mid-grey picture: the middle is as hot as the corner (no cross at any palette step)
            VideoFrame mid = flat(128, 128, 128);
            VideoPlayer::apply_fx(mid, 38, 0.0);
            const uint8_t* c0 = &mid.rgb[(static_cast<size_t>(10) * W + 10) * 3];
            for (int y = H / 2 - H / 8; y <= H / 2 + H / 8; ++y)
                for (int x = W / 2 - W / 8; x <= W / 2 + W / 8; ++x) {
                    const uint8_t* m = &mid.rgb[(static_cast<size_t>(y) * W + x) * 3];
                    CHECK(std::abs(m[0] - c0[0]) < 40 && std::abs(m[1] - c0[1]) < 40 && std::abs(m[2] - c0[2]) < 60);
                }
        }
        // every pixel is a sensor pixel: a line two pixels wide stays hot and sharp (a coarse sensor would smear it into a dim, wide band)
        VideoFrame line = flat(0, 0, 0);
        for (int y = 0; y < H; ++y) for (int x = 100; x < 102; ++x) { uint8_t* q = &line.rgb[(static_cast<size_t>(y) * W + x) * 3]; q[0] = q[1] = q[2] = 255; }
        VideoPlayer::apply_fx(line, 38, 0.0);
        CHECK(line.rgb[(static_cast<size_t>(20) * W + 100) * 3] > 200);   // (the line: hot)
        CHECK(line.rgb[(static_cast<size_t>(20) * W + 96) * 3] < 60);     // (four pixels away: dark again)
    }
    {   // Technicolor: the colours are richer than they were
        VideoFrame f = flat(170, 110, 90), g = f;
        VideoPlayer::apply_fx(g, 34, 0.0);
        auto sat = [&](const VideoFrame& a) { const uint8_t* q = &a.rgb[(static_cast<size_t>(90) * W + 160) * 3]; return static_cast<double>(std::max({q[0], q[1], q[2]}) - std::min({q[0], q[1], q[2]})); };
        CHECK(sat(g) > sat(f) + 10);
    }
    {   // Risograph does not move (a print has no time); Riso is made of paper and two inks only: its blue never beats the paper's
        VideoFrame a = flat(90, 160, 200), b = a;
        VideoPlayer::apply_fx(a, 39, 0.0);
        VideoPlayer::apply_fx(b, 39, 9.0);
        CHECK(a.rgb == b.rgb);
        for (size_t i = 0; i < a.rgb.size(); i += 3) CHECK(a.rgb[i] <= 252 && a.rgb[i + 2] <= 252);
    }
    {   // LaserDisc: a flat picture stays as it is (up to a little noise)
        VideoFrame f = flat(150, 100, 60);
        VideoPlayer::apply_fx(f, 40, 0.0);
        CHECK(std::fabs(mean(f, 0) - 150) < 8 && std::fabs(mean(f, 1) - 100) < 8 && std::fabs(mean(f, 2) - 60) < 8);
    }
    {   // Halftone CMYK: a mid grey is paper with ink dots: lighter than the grey and darker than the paper, and not flat
        VideoFrame f = flat(128, 128, 128);
        VideoPlayer::apply_fx(f, 41, 0.0);
        CHECK(mean(f, 1) > 40 && mean(f, 1) < 235);
        CHECK(colours_in(f, 0, 0, W, H) > 8);
        // High-Res: the same print with a much finer screen -- about as much ink (the same mean), many more dots (more changes along a line)
        VideoFrame g = flat(128, 128, 128);
        VideoPlayer::apply_fx(g, 42, 0.0);
        CHECK(std::fabs(mean(g, 1) - mean(f, 1)) < 30);
        auto block_spread = [&](const VideoFrame& a) {   // how much 8 x 8 blocks differ from each other: a fine screen has more dots in a block, so it is more even
            std::vector<double> m;
            for (int by = 0; by + 8 <= H; by += 8)
                for (int bx = 0; bx + 8 <= W; bx += 8) {
                    double sum = 0;
                    for (int y = by; y < by + 8; ++y) for (int x = bx; x < bx + 8; ++x) sum += a.rgb[(static_cast<size_t>(y) * W + x) * 3 + 1];
                    m.push_back(sum / 64.0);
                }
            double mu = 0, var = 0;
            for (double v : m) mu += v;
            mu /= static_cast<double>(m.size());
            for (double v : m) var += (v - mu) * (v - mu);
            return std::sqrt(var / static_cast<double>(m.size()));
        };
        CHECK(block_spread(g) < block_spread(f) * 0.75);
    }
    {   // Interlace: with the frame before, the lines of the old field are those of the frame before (a comb where something moved)
        VideoFrame prev = flat(0, 0, 0), f = flat(255, 255, 255);
        VideoPlayer::apply_fx(f, 17, 0.0, false, nullptr, &prev);   // (tick 0: the even lines are new, the odd lines come from the frame before)
        CHECK(f.rgb[(static_cast<size_t>(10) * W + 5) * 3] > 200);
        CHECK(f.rgb[(static_cast<size_t>(11) * W + 5) * 3] < 30);
        VideoFrame g = flat(255, 255, 255);
        VideoPlayer::apply_fx(g, 17, 0.04, false, nullptr, &prev);  // (the next frame: the other field is new)
        CHECK(g.rgb[(static_cast<size_t>(10) * W + 5) * 3] < 30);
        CHECK(g.rgb[(static_cast<size_t>(11) * W + 5) * 3] > 200);
    }
    {   // VHS: the colour is smeared sideways much more than the brightness -- a one pixel wide red line turns into a wider, paler one
        VideoFrame f = flat(60, 60, 60);
        for (int y = 0; y < H; ++y) { uint8_t* q = &f.rgb[(static_cast<size_t>(y) * W + 160) * 3]; q[0] = 255; q[1] = 20; q[2] = 20; }
        VideoFrame g = f;
        VideoPlayer::apply_fx(g, 3, 0.0, false, nullptr, nullptr, 0, 4);
        int wide = 0;
        for (int x = 150; x < 176; ++x) if (g.rgb[(static_cast<size_t>(90) * W + x) * 3] > g.rgb[(static_cast<size_t>(90) * W + x) * 3 + 1] + 12) ++wide;
        CHECK(wide >= 4);
    }
}

// ---- Halftone CMYK: the dot radii of a screen are worked out once per cell and picture, not on every row that crosses the cell (batch 275) ----
namespace {
VideoFrame halftone_photo(int w, int h, uint32_t seed) {   // a picture with smooth ramps, a checker, a disc and noise: every kind of dot
    VideoFrame f;
    f.w = w; f.h = h; f.rgb.resize(static_cast<size_t>(w) * h * 3);
    uint32_t s = seed;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            s = s * 1664525u + 1013904223u;
            const int noise = static_cast<int>((s >> 24) % 24);
            const double dx = x - w * 0.62, dy = y - h * 0.4;
            const bool disc = dx * dx + dy * dy < (h * 0.22) * (h * 0.22);
            const bool check = ((x / 13) + (y / 13)) % 2 == 0;
            uint8_t* q = &f.rgb[(static_cast<size_t>(y) * w + x) * 3];
            q[0] = static_cast<uint8_t>(std::min(255, x * 255 / std::max(1, w - 1) / (check ? 1 : 2) + noise));
            q[1] = static_cast<uint8_t>(std::min(255, y * 255 / std::max(1, h - 1) + (disc ? 60 : 0) + noise));
            q[2] = static_cast<uint8_t>(disc ? 220 - noise : (check ? 40 : 160) + noise);
        }
    return f;
}
}  // namespace

// The print exactly as the look made it before batch 275 (a square root and a curve per pixel and screen, the dot's colour worked out per pixel): the
// reference for the fast version, which takes the dot radius per cell from a table and the cover from a table of distance x radius.
VideoFrame halftone_reference(const VideoFrame& src, bool fine, bool braille) {
    const int w = src.w, h = src.h;
    VideoFrame out = src;
    const float cs = braille ? (fine ? 2.2f : 3.0f) : fine ? std::max(3.4f, static_cast<float>(h) / 100.0f) : std::max(6.0f, static_cast<float>(h) / 38.0f);
    const int bf = fine ? 2 : 3;
    const float aa = (fine ? 0.55f : 0.9f) / cs, ics = 1.0f / cs;
    auto smooth = [](float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); };
    const float ang[4] = {15.0f, 75.0f, 0.0f, 45.0f};
    const float ink[4][3] = {{0.00f, 0.68f, 0.94f}, {0.93f, 0.00f, 0.55f}, {1.00f, 0.95f, 0.00f}, {0.14f, 0.12f, 0.13f}};
    // the 3 x 3 (2 x 2) block averages
    const int sw = (w + bf - 1) / bf, sh = (h + bf - 1) / bf;
    std::vector<float> avg(static_cast<size_t>(sw) * sh * 3, 0.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 3; ++k) avg[(static_cast<size_t>(y / bf) * sw + x / bf) * 3 + k] += src.rgb[(static_cast<size_t>(y) * w + x) * 3 + k];
    for (int by = 0; by < sh; ++by)
        for (int bx = 0; bx < sw; ++bx)
            for (int k = 0; k < 3; ++k) avg[(static_cast<size_t>(by) * sw + bx) * 3 + k] /= static_cast<float>(std::min(bf, h - by * bf) * std::min(bf, w - bx * bf));
    auto radius = [&](int i, int xc, int yc) {
        const float* a = &avg[(static_cast<size_t>(yc / bf) * sw + xc / bf) * 3];
        const float r = a[0] / 255.0f, g = a[1] / 255.0f, b = a[2] / 255.0f, lm = 0.299f * r + 0.587f * g + 0.114f * b;
        const float c0 = 1.0f - std::clamp(lm + (r - lm) * 1.2f, 0.0f, 1.0f), m0 = 1.0f - std::clamp(lm + (g - lm) * 1.2f, 0.0f, 1.0f), y0 = 1.0f - std::clamp(lm + (b - lm) * 1.2f, 0.0f, 1.0f);
        const float k = std::pow(std::floor(std::min({c0, m0, y0}) * 1024.0f) / 1024.0f, 1.8f) * 0.9f;
        const float in = std::clamp(i == 0 ? c0 - k : i == 1 ? m0 - k : i == 2 ? y0 - k : k, 0.0f, 1.0f);
        const float gg = std::pow(std::floor(in * 1024.0f) / 1024.0f, 0.86f);
        return 0.5642f * std::sqrt(gg) + 0.215f * smooth((gg - 0.6f) / 0.4f);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float v[3] = {247.0f, 242.0f, 230.0f};
            for (int i = 0; i < 4; ++i) {
                const float ca = std::cos(ang[i] * 0.017453293f), sa = std::sin(ang[i] * 0.017453293f);
                const float u = x * ca * ics + y * sa * ics, vv = -x * sa * ics + y * ca * ics;
                const float cu = std::floor(u) + 0.5f, cv = std::floor(vv) + 0.5f;
                const int xc = std::clamp(static_cast<int>((cu * ca - cv * sa) * cs), 0, w - 1), yc = std::clamp(static_cast<int>((cu * sa + cv * ca) * cs), 0, h - 1);
                const float rr = radius(i, xc, yc), d = std::sqrt((u - cu) * (u - cu) + (vv - cv) * (vv - cv));
                const float cover = d <= std::max(0.0f, rr - aa) ? 1.0f : d >= rr + aa ? 0.0f : 1.0f - smooth((d - (rr - aa)) / (2.0f * aa));
                for (int k = 0; k < 3; ++k) v[k] *= 1.0f + (ink[i][k] - 1.0f) * cover * 0.97f;
            }
            for (int k = 0; k < 3; ++k) out.rgb[(static_cast<size_t>(y) * w + x) * 3 + k] = static_cast<uint8_t>(std::clamp(std::lround(v[k]), 0L, 255L));
        }
    return out;
}

TEST(video_halftone_cmyk_at_full_strength_is_the_exact_print) {
    struct Case { int w, h, fx; bool braille; };
    const Case cases[] = {{301, 171, 41, false}, {301, 171, 42, false}, {640, 360, 41, false}, {640, 360, 42, false}, {97, 61, 41, false}, {97, 61, 42, false},
                          {160, 96, 41, true}, {160, 96, 42, true}, {1280, 720, 42, false}};
    for (const Case& c : cases) {
        const VideoFrame src = halftone_photo(c.w, c.h, 7u + static_cast<uint32_t>(c.w));
        VideoFrame f = src;
        VideoPlayer::set_fx_threads(2);
        VideoPlayer::apply_fx(f, c.fx, 0.0, c.braille, nullptr, nullptr, 0, 0, 0);   // (the last argument: the Effect Contrast, 0 = full)
        const VideoFrame ref = halftone_reference(src, c.fx == 42, c.braille);
        double sum = 0.0;
        int off = 0;
        for (size_t i = 0; i < f.rgb.size(); i += 3) {
            int worst = 0;
            for (int k = 0; k < 3; ++k) { const int d = std::abs(static_cast<int>(f.rgb[i + k]) - static_cast<int>(ref.rgb[i + k])); sum += d; worst = std::max(worst, d); }
            if (worst > 8) ++off;
        }
        const double mean = sum / static_cast<double>(f.rgb.size()), share = off / (static_cast<double>(f.rgb.size()) / 3.0);
        // (the tables round the edge of a dot to a fraction of a pixel; a pixel right on the border of two cells can fall into the other one)
        CHECK(mean < 1.5);
        CHECK(share < 0.003);
        if (!(mean < 1.5 && share < 0.003)) std::printf("  halftone %dx%d fx %d braille %d: mean difference %.3f, %.4f of the pixels more than 8 levels off\n", c.w, c.h, c.fx, c.braille ? 1 : 0, mean, share);
    }
    VideoPlayer::set_fx_threads(0);
}

TEST(video_halftone_cmyk_effect_contrast_is_the_strength_of_the_print) {
    const VideoFrame src = halftone_photo(320, 180, 99u);
    VideoFrame full = src;
    VideoPlayer::apply_fx(full, 41, 0.0, false, nullptr, nullptr, 0, 0, 0);
    double dist[3] = {0, 0, 0};
    for (int level = 0; level < kFxContrasts; ++level) {
        VideoFrame f = src;
        VideoPlayer::apply_fx(f, 41, 0.0, false, nullptr, nullptr, 0, 0, level);
        const float mix = video_fx_print_mix(level);
        int bad = 0;
        for (size_t i = 0; i < f.rgb.size(); ++i) {
            const float want = src.rgb[i] + (full.rgb[i] - static_cast<float>(src.rgb[i])) * mix;
            if (std::fabs(f.rgb[i] - want) > 1.6f) ++bad;
            dist[level] += std::abs(static_cast<int>(f.rgb[i]) - static_cast<int>(src.rgb[i]));
        }
        CHECK_EQ(bad, 0);   // (the print over the picture at its strength: the print's own rounding is the only difference)
    }
    CHECK(dist[2] < dist[1] && dist[1] < dist[0]);   // low is nearer to the picture than reduced, reduced than full
    CHECK(video_fx_print_mix(0) == 1.0f && video_fx_print_mix(kFxContrastDefault) == 0.70f && video_fx_print_mix(2) == 0.50f);
    // the strength is part of the look's signature (a cached picture is made again when it changes), for the two CMYK looks only
    VideoLook a, b;
    a.fx = b.fx = 42; a.fxc = 0; b.fxc = 2;
    CHECK(a.sig() != b.sig());
    a.fx = b.fx = 10;
    CHECK(a.sig() == b.sig());
}

// VHS (batch 275): a tape that tracks well is steady -- the lines sway by a fraction of a pixel; only now and then (in about one stretch of 8 s in
// three, for 1 to 3 s) the tracking slips and the lines wobble strongly. A vertical edge shows it: its place along the rows.
TEST(video_vhs_is_steady_and_slips_only_now_and_then) {
    const int W = 640, H = 360;
    VideoFrame edge;
    edge.w = W; edge.h = H; edge.rgb.assign(static_cast<size_t>(W) * H * 3, 30);
    for (int y = 0; y < H; ++y)
        for (int x = W / 2; x < W; ++x) for (int k = 0; k < 3; ++k) edge.rgb[(static_cast<size_t>(y) * W + x) * 3 + k] = 220;
    auto spread = [&](double t) {   // how far the edge moves between the rows (the head-switching rows at the bottom left out)
        VideoFrame f = edge;
        VideoPlayer::apply_fx(f, 3, t);
        double lo = 1e9, hi = -1e9;
        for (int y = 0; y < H - H / 24 - 2; ++y) {
            double where = W / 2;
            for (int x = W / 4; x < 3 * W / 4; ++x) {
                const int a = f.rgb[(static_cast<size_t>(y) * W + x) * 3 + 1], b = f.rgb[(static_cast<size_t>(y) * W + x + 1) * 3 + 1];
                if (a < 125 && b >= 125) { where = x + (125.0 - a) / std::max(1, b - a); break; }
            }
            lo = std::min(lo, where); hi = std::max(hi, where);
        }
        return hi - lo;
    };
    int steady = 0, wobbly = 0, n = 0;
    double steady_worst = 0.0;
    for (double t = 0.0; t < 160.0; t += 0.5, ++n) {
        const double s = spread(t);
        if (s < 2.5) { ++steady; steady_worst = std::max(steady_worst, s); }
        if (s > 5.0) ++wobbly;
    }
    CHECK(steady > n * 6 / 10);    // most of the time the edge is straight within a little more than a pixel (noise and the slow sway)
    CHECK(wobbly > 0);             // ... and now and then the tracking slips
    CHECK(wobbly < n * 3 / 10);
    if (!(steady > n * 6 / 10 && wobbly > 0 && wobbly < n * 3 / 10)) std::printf("  vhs: %d steady, %d wobbly of %d frames\n", steady, wobbly, n);
}

// ASCII (batch 275): an edge is drawn as a line along it (| / \\ - _), the rest with characters by shape and brightness. A bright disc and a bright bar on a
// dark ground: the cells on the bar's left and right side carry a vertical stroke (ink in at most three of the cell's six columns, in most of its rows); the
// cells on the disc's rim carry a stroke that follows the rim: rising on its upper left and lower right, falling on its upper right and lower left.
TEST(video_ascii_draws_edges_as_lines_along_them) {
    const int W = 640, H = 360, cw = 6, ch = 12;   // (cells of 6 x 12 at 640 wide)
    VideoFrame f;
    f.w = W; f.h = H; f.rgb.assign(static_cast<size_t>(W) * H * 3, 20);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double dx = x - 420.0, dy = y - 180.0;
            const bool lit = (x >= 63 && x < 160 && y > 30 && y < 330) || dx * dx + dy * dy < 120.0 * 120.0;
            if (lit) { uint8_t* q = &f.rgb[(static_cast<size_t>(y) * W + x) * 3]; q[0] = 230; q[1] = 200; q[2] = 120; }
        }
    VideoPlayer::apply_fx(f, 37, 0.0);
    auto ink = [&](int cx, int cy, int& cols, int& rows, double& slope) {   // which of the cell's columns / rows have ink, and how the ink leans (+ = rises to the right)
        bool cu[64] = {}, ru[64] = {};
        double sx = 0, sy = 0, sxy = 0, sxx = 0;
        int n = 0;
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                const uint8_t* q = &f.rgb[(static_cast<size_t>(cy * ch + y) * W + cx * cw + x) * 3];
                if (q[0] + q[1] + q[2] > 300) { cu[x] = ru[y] = true; sx += x; sy += -y; sxy += -static_cast<double>(x) * y; sxx += x * x; ++n; }
            }
        cols = rows = 0;
        for (int i = 0; i < cw; ++i) cols += cu[i];
        for (int i = 0; i < ch; ++i) rows += ru[i];
        const double d = n * sxx - sx * sx;
        slope = n > 2 && std::fabs(d) > 1e-9 ? (n * sxy - sx * sy) / d : 0.0;
    };
    int vertical = 0, checked = 0;
    for (int cy = 5; cy < 25; ++cy) {
        for (int cx : {10, 26}) {   // (the bar's sides: x = 63 lies in cell 10, x = 159 in cell 26)
            int cols = 0, rows = 0; double sl = 0;
            ink(cx, cy, cols, rows, sl);
            ++checked;
            if (cols <= 3 && rows >= 8) ++vertical;
        }
    }
    CHECK(vertical >= checked * 3 / 4);
    if (vertical < checked * 3 / 4) std::printf("  ascii edges: %d of %d cells on the bar's sides are a vertical stroke\n", vertical, checked);
    // the disc's rim at 45 degrees runs like a circle drawn by hand: rising (/) on the upper left and the lower right, falling on the upper right and the lower left
    auto lean = [&](double ang_deg) {
        const double a = ang_deg * 3.14159265 / 180.0;
        const int x = static_cast<int>(420.0 + 120.0 * std::cos(a)), y = static_cast<int>(180.0 - 120.0 * std::sin(a));
        int cols = 0, rows = 0; double sl = 0;
        ink(x / cw, y / ch, cols, rows, sl);
        return sl;
    };
    CHECK(lean(135.0) > 0.5);    // upper left: rising
    CHECK(lean(315.0) > 0.5);    // lower right: rising
    CHECK(lean(45.0) < -0.5);    // upper right: falling
    CHECK(lean(225.0) < -0.5);   // lower left: falling
}
