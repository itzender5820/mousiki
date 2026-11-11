// The looks of the video view share the rows of a picture between threads: the result must be the same as with one thread, for every look.
#include <algorithm>
#include "test_framework.h"
#include "video_player.h"

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
    CHECK(shifted_same(3, 4) > 0.999);        // bayer 4x4: every 4 pixels
    CHECK(shifted_same(4, 8) > 0.999);        // bayer 8x8: every 8
    CHECK(shifted_same(1, 8) < 0.9);          // blue noise: no repeat there
}

TEST(video_look_signature_changes_with_the_dither_pattern) {
    VideoLook a, b;
    a.fx = 20; b.fx = 20; b.dither = 4;
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
    CHECK_EQ(video_fx_from("night vision"), 29);
    CHECK(video_fx_pos(26) == video_fx_pos(8) + 1);   // the glow variant sits right below Phosphor Green
    CHECK_EQ(video_fx_step(29, 1), kVideoFxOrder[(video_fx_pos(29) + 1) % kVideoFxShown]);
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
    const VideoFrame lines = make(2, 0), grille = make(2, 1), mask = make(2, 2), slot = make(2, 3), lcd = make(2, 4), clean = make(2, 5);
    CHECK(row_var(lines) > 20);      // scanlines: the rows differ
    CHECK(col_var(grille) > 40);     // an aperture grille: vertical stripes
    CHECK(col_var(mask) > 20);       // a shadow mask: dots
    CHECK(col_var(slot) > 40);
    CHECK(col_var(lcd) > 40 && row_var(lcd) > 20);   // an lcd grid: gaps both ways
    CHECK(row_var(clean) < 6 && col_var(clean) < 20);   // clean: the vignette's slope only, no structure
    CHECK(grille.rgb != lines.rgb && mask.rgb != grille.rgb && clean.rgb != lines.rgb);
    // Scanlines itself (1) and the effects that do not use the layer ignore it
    CHECK(make(1, 3).rgb == make(1, 0).rgb);
    CHECK(make(21, 3).rgb == make(21, 0).rgb);
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

// The phosphor looks are tinted all the way down: the black of a tube is faintly green (or amber), not grey; the glow variant has a dark bezel.
TEST(video_phosphor_is_tinted_and_the_glow_has_a_bezel) {
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
    CHECK(g.rgb[0] == 0 && g.rgb[1] == 0 && g.rgb[2] == 0);                 // the corner is outside the curved glass
    const size_t mid = (static_cast<size_t>(H / 2) * W + W / 2) * 3;
    CHECK(g.rgb[mid + 1] > 200);                                              // the middle is bright green
    CHECK(g.rgb[mid + 1] > g.rgb[mid] + 20);   // (burnt towards white at full drive, but still green)
}
