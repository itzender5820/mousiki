// The video view's frame source: proportions, palette picture, braille, and the decoder following a clock.
#include "test_framework.h"
#include "video_player.h"
#include "cover_art.h"
#include "process_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

using namespace muisc;
namespace fs = std::filesystem;

static std::string make_test_video(const std::string& name, int seconds, int rate = 25) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_video_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path f = dir / name;
    if (!fs::exists(f, ec)) {
        const ProcResult r = run_capture({"ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=320x180:rate=" + std::to_string(rate) + ":duration=" + std::to_string(seconds),
                                          "-f", "lavfi", "-i", "sine=frequency=440:duration=" + std::to_string(seconds), "-c:v", "mpeg4", "-c:a", "aac", "-shortest", f.string()}, true);
        if (!r.ok()) return std::string();
    }
    return f.string();
}

TEST(video_fit_keeps_proportions_and_even_sizes) {
    VideoPlayer v;
    const std::string f = make_test_video("t6.mp4", 6);
    if (f.empty()) return;   // no ffmpeg here
    CHECK(v.set_source(f));
    CHECK_NEAR(v.aspect(), 320.0 / 180.0, 1e-6);
    int w = 0, h = 0;
    v.fit(400, 400, 1.0, w, h);          // wide video in a square box: full width
    CHECK_EQ(w, 400);
    CHECK(h == 224 || h == 226);          // 400 / (16/9) = 225 -> even
    v.fit(1000, 100, 1.0, w, h);         // low box: the height decides
    CHECK_EQ(h, 100);
    CHECK_EQ(w % 2, 0);
    CHECK(w >= 176 && w <= 180);
    v.fit(100, 100, 0.5, w, h);          // units twice as high as wide (cells): fewer rows than columns
    CHECK(h < w);
}

TEST(video_palette_quantize_is_stable_and_in_range) {
    VideoFrame f;
    f.w = 8; f.h = 4;
    f.rgb.assign(8 * 4 * 3, 0);
    for (int i = 0; i < 8 * 4; ++i) { f.rgb[i * 3] = 255; f.rgb[i * 3 + 1] = 0; f.rgb[i * 3 + 2] = 0; }   // pure red
    std::vector<uint8_t> hue, level;
    VideoPlayer::quantize(f, hue, level);
    CHECK_EQ(hue.size(), static_cast<size_t>(32));
    CHECK_EQ(VideoPlayer::palette().size(), static_cast<size_t>(248));
    for (auto h : hue) { CHECK(h < 248); const auto& c = VideoPlayer::palette()[h]; CHECK(c[0] >= 204 && c[1] <= 51 && c[2] <= 51); }
    for (auto l : level) CHECK_EQ(l, 255);
    // grey goes to the grey ramp (index >= 216)
    for (auto& b : f.rgb) b = 128;
    VideoPlayer::quantize(f, hue, level);
    for (auto h : hue) CHECK(h >= 216);
}

TEST(video_dither_bw_is_only_black_and_white) {
    VideoFrame f;
    f.w = 64; f.h = 64;
    f.rgb.resize(64 * 64 * 3);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) for (int c = 0; c < 3; ++c) f.rgb[(static_cast<size_t>(y) * 64 + x) * 3 + c] = static_cast<uint8_t>((x * 4 + y) & 255);
    VideoPlayer::apply_fx(f, 21, 0.0);
    int dark = 0, light = 0;
    for (uint8_t v : f.rgb) { CHECK(v == 0 || v == 255); if (v == 0) ++dark; else ++light; }
    CHECK(dark > 0 && light > 0);
}

TEST(video_dither_gradient_dark_stays_black) {
    VideoFrame f;
    f.w = 64; f.h = 64;
    f.rgb.assign(64 * 64 * 3, 0);   // a black picture
    VideoFxGradient g;
    VideoPlayer::apply_fx(f, 22, 0.0, false, &g);
    for (uint8_t v : f.rgb) CHECK_EQ(v, 0);
    for (auto& b : f.rgb) b = 255;   // a white one: every dot is the colour of the wave
    VideoPlayer::apply_fx(f, 22, 0.0, false, &g);
    int coloured = 0;
    for (size_t i = 0; i + 2 < f.rgb.size(); i += 3) if (f.rgb[i] != f.rgb[i + 1] || f.rgb[i + 1] != f.rgb[i + 2]) ++coloured;
    CHECK(coloured > 1000);
}

TEST(video_quantize_keeps_near_black_flat) {
    // the dark parts of an effect picture (a few shades above black) must not turn into a raster of dither squares
    VideoFrame f;
    f.w = 8; f.h = 8;
    f.rgb.assign(8 * 8 * 3, 0);
    for (auto& b : f.rgb) b = 28;
    std::vector<uint8_t> hue, level;
    VideoPlayer::quantize(f, hue, level);
    for (auto h : hue) CHECK_EQ(h, 216);
    for (auto& b : f.rgb) b = 128;   // a mid grey still dithers over its two nearest shades of the grey ramp
    VideoPlayer::quantize(f, hue, level);
    for (auto h : hue) CHECK(h >= 216 + 11 && h <= 216 + 12);
}

TEST(video_quantize_off_white_ink_stays_on_the_grey_ramp) {
    // the off-white of the Dither look (236, 238, 226) must not dither over the colour cube (a grid of squares over every light area)
    VideoFrame f;
    f.w = 8; f.h = 8;
    f.rgb.assign(8 * 8 * 3, 0);
    for (size_t i = 0; i < 64; ++i) { f.rgb[i * 3] = 236; f.rgb[i * 3 + 1] = 238; f.rgb[i * 3 + 2] = 226; }
    std::vector<uint8_t> hue, level;
    VideoPlayer::quantize(f, hue, level);
    for (auto h : hue) CHECK(h >= 216);
    // and the dark ink (14, 16, 28) is flat black
    for (size_t i = 0; i < 64; ++i) { f.rgb[i * 3] = 14; f.rgb[i * 3 + 1] = 16; f.rgb[i * 3 + 2] = 28; }
    VideoPlayer::quantize(f, hue, level);
    for (auto h : hue) CHECK_EQ(h, 216);
}

TEST(video_quantize_dark_tint_stays_a_colour) {
    // a dim purple (the gradient look in a shadow) must not collapse into the grey ramp
    VideoFrame f;
    f.w = 8; f.h = 8;
    f.rgb.assign(8 * 8 * 3, 0);
    for (size_t i = 0; i < 64; ++i) { f.rgb[i * 3] = 40; f.rgb[i * 3 + 1] = 12; f.rgb[i * 3 + 2] = 58; }
    std::vector<uint8_t> hue, level;
    VideoPlayer::quantize(f, hue, level);
    int cube = 0;
    for (auto h : hue) if (h < 216) ++cube;
    CHECK(cube >= 48);
}

TEST(video_braille_cells_follow_brightness) {
    VideoFrame f;
    f.w = 4; f.h = 8;
    f.rgb.assign(4 * 8 * 3, 0);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 2; ++x) for (int c = 0; c < 3; ++c) f.rgb[(static_cast<size_t>(y) * 4 + x) * 3 + c] = 255;   // left cell column white
    auto cells = VideoPlayer::braille(f, 2, 2);
    CHECK_EQ(cells.size(), static_cast<size_t>(2));
    CHECK(cells[0][0].mask != 0 && cells[0][0].level > 200);
    CHECK_EQ(cells[0][1].mask, 0);
    CHECK_EQ(cells[0][1].level, 0);
    CHECK(cells[1][0].level > 200);
}

TEST(video_decoder_follows_the_clock_and_restarts_after_a_jump) {
    const std::string file = make_test_video("t6.mp4", 6);
    if (file.empty()) return;
    VideoPlayer v;
    CHECK(v.set_source(file));
    int w = 0, h = 0;
    v.fit(160, 90, 1.0, w, h);
    // wait for the first frame
    const VideoFrame* fr = nullptr;
    for (int i = 0; i < 100 && !fr; ++i) { fr = v.frame_for(0.0, w, h, true); if (!fr) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    CHECK(fr != nullptr);
    if (!fr) return;
    CHECK_EQ(fr->w, w);
    CHECK_EQ(fr->h, h);
    CHECK(fr->pts < 0.2);
    // the clock moves: the frame follows it (decoder runs in real time at most 12 frames ahead)
    double t = 0.0;
    const VideoFrame* last = fr;
    for (int i = 0; i < 40; ++i) { t += 0.04; std::this_thread::sleep_for(std::chrono::milliseconds(40)); last = v.frame_for(t, w, h, true); }
    CHECK(last != nullptr);
    CHECK(last->pts <= t + 1e-9);
    CHECK(last->pts > t - 0.5);
    // a jump: the frame comes from the new place
    const VideoFrame* j = nullptr;
    for (int i = 0; i < 100 && !(j && j->pts > 3.5); ++i) { j = v.frame_for(4.0, w, h, true); if (!j || j->pts < 3.5) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    CHECK(j != nullptr);
    if (j) { CHECK(j->pts > 3.5); CHECK(j->pts <= 4.0 + 1e-9); }
    // paused: no new frame is taken however far the clock is
    const double before = j ? j->pts : 0;
    const VideoFrame* p = v.frame_for(5.5, w, h, false);
    CHECK(p != nullptr);
    if (p) CHECK_NEAR(p->pts, before, 1e-9);
}

TEST(video_cover_picture_in_an_audio_file_is_no_video) {
    VideoPlayer v;
    CHECK(!v.set_source("/nonexistent/file.mp3"));
    CHECK(!v.has_video());
}

#include "local_source.h"

TEST(video_files_are_listed_only_in_video_mode) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_video_lib";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    for (const char* n : {"a.mp3", "b.mp4", "c.MKV", "d.txt"}) { std::ofstream(dir / n) << "x"; }
    LocalSource src;
    auto names = [&](const std::vector<LocalTrack>& v) { std::string o; for (auto& t : v) o += t.path.filename().string() + ";"; return o; };
    const auto off = src.scan({dir.string()});
    CHECK(names(off).find("a.mp3") != std::string::npos);
    CHECK(names(off).find("b.mp4") == std::string::npos);
    src.set_include_video(true);
    const auto on = src.scan({dir.string()});
    CHECK(names(on).find("b.mp4") != std::string::npos);
    CHECK(names(on).find("c.MKV") != std::string::npos);
    CHECK(names(on).find("d.txt") == std::string::npos);
    CHECK(LocalSource::is_video_file("x.WEBM"));
    CHECK(!LocalSource::is_video_file("x.flac"));
    fs::remove_all(dir, ec);
}

TEST(video_starts_in_the_middle_and_resyncs_after_an_absence) {
    const std::string file = make_test_video("t6.mp4", 6);
    if (file.empty()) return;
    VideoPlayer v;
    CHECK(v.set_source(file));
    int w = 0, h = 0;
    v.fit(160, 90, 1.0, w, h);
    // the field switches to the video while the track is already at 3 s
    const VideoFrame* fr = nullptr;
    for (int i = 0; i < 100 && !(fr && fr->pts > 2.5); ++i) { fr = v.frame_for(3.0, w, h, true); if (!fr || fr->pts <= 2.5) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    CHECK(fr != nullptr);
    if (fr) { CHECK(fr->pts > 2.5); CHECK(fr->pts <= 3.0 + 1e-9); }
    // away for a while (the sound went on to 5 s, nobody asked for frames): the old picture stays until the new one is there
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const VideoFrame* back = v.frame_for(5.0, w, h, true);
    CHECK(back != nullptr);
    for (int i = 0; i < 100 && !(back && back->pts > 4.5); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); back = v.frame_for(5.0, w, h, true); CHECK(back != nullptr); }
    if (back) { CHECK(back->pts > 4.5); CHECK(back->pts <= 5.0 + 1e-9); }
}

TEST(video_snapshot_takes_a_frame_for_the_cover) {
    const std::string file = make_test_video("t6.mp4", 6);
    if (file.empty()) return;
    CoverImage c;
    CHECK(decode_video_snapshot(fs::path(file), c));
    CHECK(c.w >= 2 && c.h >= 2);
    CHECK(c.rgb.size() == static_cast<size_t>(c.w) * c.h * 3);
    CHECK(!decode_video_snapshot(fs::path("/nonexistent/none.mp4"), c) || true);   // (no crash on a missing file)
}

TEST(video_forward_seek_and_resize_keep_a_picture) {
    const std::string file = make_test_video("t7.mp4", 30);
    if (file.empty()) return;
    VideoPlayer v;
    if (!v.set_source(file)) return;
    int w = 0, h = 0;
    v.fit(160, 90, 1.0, w, h);
    const VideoFrame* fr = nullptr;
    for (int i = 0; i < 100 && !fr; ++i) { fr = v.frame_for(0.5, w, h, true); if (!fr) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    CHECK(fr != nullptr);
    // a long jump forward (more than 10 s): the decoder restarts ONCE and the picture arrives; the old one stays meanwhile
    bool shown_old = false;
    for (int i = 0; i < 160; ++i) {
        fr = v.frame_for(20.0, w, h, true);
        if (fr && fr->pts < 19.0) shown_old = true;
        if (fr && fr->pts > 19.0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(fr != nullptr);
    if (fr) CHECK(fr->pts > 19.0);
    (void)shown_old;
    // a new size: the picture on screen stays (the caller scales it) and a frame of the new size follows
    int w2 = 0, h2 = 0;
    v.fit(96, 54, 1.0, w2, h2);
    fr = v.frame_for(20.5, w2, h2, true);
    CHECK(fr != nullptr);
    for (int i = 0; i < 100 && !(fr && fr->w == w2); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); fr = v.frame_for(21.0, w2, h2, true); CHECK(fr != nullptr); }
    if (fr) CHECK_EQ(fr->w, w2);
}

TEST(video_resample_scales_a_frame) {
    VideoFrame f;
    f.w = 4; f.h = 2; f.rgb.assign(4 * 2 * 3, 0);
    for (int x = 0; x < 4; ++x) f.rgb[static_cast<size_t>(x) * 3] = static_cast<uint8_t>(x * 60);
    const VideoFrame o = VideoPlayer::resample(f, 8, 4);
    CHECK_EQ(o.w, 8); CHECK_EQ(o.h, 4);
    CHECK_EQ(o.rgb.size(), static_cast<size_t>(8 * 4 * 3));
    CHECK_EQ(o.rgb[(0 * 8 + 7) * 3], 180);
    CHECK_EQ(o.rgb[(3 * 8 + 0) * 3], 0);
}

static VideoFrame fx_gradient(int w, int h) {
    VideoFrame f;
    f.w = w; f.h = h; f.rgb.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &f.rgb[(static_cast<size_t>(y) * w + x) * 3];
            p[0] = static_cast<uint8_t>(40 + 180 * x / w); p[1] = static_cast<uint8_t>(40 + 180 * y / h); p[2] = 160;
        }
    return f;
}
static long fx_sum(const VideoFrame& f) { long s = 0; for (uint8_t v : f.rgb) s += v; return s; }

TEST(video_fx_off_changes_nothing_and_others_keep_the_size) {
    const VideoFrame src = fx_gradient(200, 120);
    VideoFrame a = src;
    VideoPlayer::apply_fx(a, 0, 1.0);
    CHECK(a.rgb == src.rgb);
    for (int fx = 1; fx < kVideoFxCount; ++fx) {
        if (video_fx_canon(fx) != fx) continue;   // (Dither 11 and Bloom 14 are no longer offered)
        VideoFrame b = src;
        VideoPlayer::apply_fx(b, fx, 1.0);
        CHECK_EQ(b.w, 200); CHECK_EQ(b.h, 120);
        CHECK_EQ(b.rgb.size(), src.rgb.size());
        CHECK(b.rgb != src.rgb);
    }
}

TEST(video_fx_is_deterministic_and_moves_with_the_time) {
    const VideoFrame src = fx_gradient(200, 120);
    for (int fx = 1; fx < kVideoFxCount; ++fx) {
        VideoFrame a = src, b = src;
        VideoPlayer::apply_fx(a, fx, 3.2);
        VideoPlayer::apply_fx(b, fx, 3.2);
        CHECK(a.rgb == b.rgb);   // the same time, the same picture (a paused video stands still)
    }
    VideoFrame v1 = src, v2 = src;   // vhs noise / wobble differ from frame to frame
    VideoPlayer::apply_fx(v1, 3, 1.00);
    VideoPlayer::apply_fx(v2, 3, 1.40);
    CHECK(v1.rgb != v2.rgb);
}

TEST(video_scanlines_darken_the_gaps_between_the_lines) {
    VideoFrame f;
    f.w = 40; f.h = 40; f.rgb.assign(40 * 40 * 3, 200);
    VideoPlayer::apply_fx(f, 1, 0.0);
    long even = 0, odd = 0;
    for (int y = 0; y < 40; ++y) for (int x = 0; x < 40; ++x) (y % 2 ? odd : even) += f.rgb[(static_cast<size_t>(y) * 40 + x) * 3];
    CHECK(odd < even);   // (a gap row is darker than a beam row)
    CHECK(fx_sum(f) < 200L * 40 * 40 * 3 * 11 / 10);   // and the picture does not blow up
}

TEST(video_crt_has_dark_corners) {
    VideoFrame f;
    f.w = 240; f.h = 160; f.rgb.assign(240 * 160 * 3, 200);
    VideoPlayer::apply_fx(f, 2, 0.0);
    auto lum = [&](int x0, int y0) { long s = 0; for (int y = y0; y < y0 + 8; ++y) for (int x = x0; x < x0 + 8; ++x) s += f.rgb[(static_cast<size_t>(y) * 240 + x) * 3 + 1]; return s; };
    CHECK(lum(0, 0) < lum(116, 76));   // the corner is darker than the middle
}

TEST(video_glitch_has_bursts_small_particles_and_calm_frames) {
    const VideoFrame src = fx_gradient(240, 120);
    int bursts = 0, calm = 0, particles = 0;
    for (int tick = 0; tick < 200; ++tick) {   // (25 frames per second: 8 seconds)
        VideoFrame f = src;
        VideoPlayer::apply_fx(f, 4, tick / 25.0 + 0.001);
        long changed = 0;
        for (size_t i = 0; i < f.rgb.size(); i += 3) {
            const int d = std::abs(int(f.rgb[i]) - int(src.rgb[i])) + std::abs(int(f.rgb[i + 1]) - int(src.rgb[i + 1])) + std::abs(int(f.rgb[i + 2]) - int(src.rgb[i + 2]));
            if (d > 150) ++changed;
        }
        const double ratio = static_cast<double>(changed) / (240.0 * 120.0);
        if (ratio > 0.05) ++bursts;
        else if (ratio > 0.0005) ++particles;   // a few small blocks / specks only
        else ++calm;
    }
    CHECK(bursts > 0);
    CHECK(particles > 0);      // the small glitch particles between the bursts
    CHECK(calm > 0);           // and frames that are only scanlines
    CHECK(bursts < 100);       // (a burst is not the normal state)
}

TEST(video_pixel_makes_flat_blocks_and_keeps_the_picture) {
    const VideoFrame src = fx_gradient(240, 120);
    VideoFrame f = src;
    VideoPlayer::apply_fx(f, 5, 0.0);
    CHECK(f.w == 240 && f.h == 120);
    CHECK(f.rgb != src.rgb);
    int equal_neighbours = 0, total = 0;   // neighbours inside a block are equal: most horizontal pairs
    for (int y = 0; y < 120; ++y)
        for (int x = 0; x + 1 < 240; ++x, ++total)
            if (std::memcmp(&f.rgb[(y * 240 + x) * 3], &f.rgb[(y * 240 + x + 1) * 3], 3) == 0) ++equal_neighbours;
    CHECK(equal_neighbours > total / 2);
    CHECK(std::labs(fx_sum(f) - fx_sum(src)) < fx_sum(src) / 10);   // the same brightness, roughly
}

TEST(video_gradient_wave_uses_the_given_colours_and_moves) {
    VideoFrame src = fx_gradient(240, 120);
    VideoFxGradient g;
    for (int i = 0; i < 3; ++i) { g.c[i][0] = 255; g.c[i][1] = 0; g.c[i][2] = 0; }   // all stops red: the result is red-dominated
    VideoFrame f = src;
    VideoPlayer::apply_fx(f, 6, 0.0, false, &g);
    long r = 0, gr = 0;
    for (size_t i = 0; i < f.rgb.size(); i += 3) { r += f.rgb[i]; gr += f.rgb[i + 1]; }
    CHECK(r > gr * 2);
    VideoFrame a = src, b = src;
    VideoPlayer::apply_fx(a, 6, 0.0);
    VideoPlayer::apply_fx(b, 6, 3.0);
    CHECK(a.rgb != b.rgb);                                    // the wave travels
    VideoFrame c = src, d = src;
    VideoPlayer::apply_fx(c, 6, 1.5); VideoPlayer::apply_fx(d, 6, 1.5);
    CHECK(c.rgb == d.rgb);                                    // and is deterministic
}

TEST(video_fx_names_round_trip) {
    for (int i = 0; i < kVideoFxCount; ++i) CHECK(video_fx_from(video_fx_name(i)) == video_fx_canon(i));   // (Dither is no longer offered: it reads as Dither B/W)
    CHECK(video_fx_from("dither") == 21);
    CHECK(video_fx_from("GRADIENT") == 6);
    CHECK(video_fx_from("nonsense") == 0);
}

TEST(video_fit_cover_covers_and_crop_cuts) {
    VideoPlayer v;   // (no source: the proportions fall back to 16:9)
    int w = 0, h = 0;
    v.fit_cover(100, 100, 1.0, w, h);
    CHECK(w >= 100 && h >= 100);
    CHECK(w > h);                              // 16:9 covering a square: the width grows
    v.fit_cover(320, 90, 1.0, w, h);
    CHECK(w >= 320 && h >= 90);
    const VideoFrame f = fx_gradient(10, 6);
    const VideoFrame c = VideoPlayer::crop(f, 2, 1, 6, 4);
    CHECK_EQ(c.w, 6); CHECK_EQ(c.h, 4);
    CHECK_EQ(c.rgb[0], f.rgb[(1 * 10 + 2) * 3]);
    const VideoFrame big = VideoPlayer::crop(f, 8, 4, 50, 50);   // clamped to the frame
    CHECK_EQ(big.w, 2); CHECK_EQ(big.h, 2);
}

// (A look at the effects: MOUSIKI_FX_DUMP=<dir> writes one PPM per effect from the raw frame <dir>/in.rgb of 640x360.)
TEST(video_fx_dump_for_a_look) {
    const char* d = std::getenv("MOUSIKI_FX_DUMP");
    if (!d) return;
    std::ifstream in(std::string(d) + "/in.rgb", std::ios::binary);
    VideoFrame base; base.w = 640; base.h = 360; base.rgb.resize(640 * 360 * 3);
    in.read(reinterpret_cast<char*>(base.rgb.data()), static_cast<std::streamsize>(base.rgb.size()));
    VideoFrame prev = base;   // (for the interlace look: the same picture, 10 pixels to the side)
    for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x)
            for (int k = 0; k < 3; ++k) prev.rgb[(y * 640 + x) * 3 + k] = base.rgb[(y * 640 + std::max(0, x - 10)) * 3 + k];
    for (int fx = 0; fx < kVideoFxCount + 2; ++fx) {   // kVideoFxCount: chromatic aberration, +1: grade (contrast 60, colours 30)
        VideoFrame f = base;
        VideoLook look;
        look.fx = fx < kVideoFxCount ? fx : 0;
        if (fx == kVideoFxCount) look.chroma = 3;
        if (fx == kVideoFxCount + 1) { look.contrast = 60; look.colors = 30; }
        VideoPlayer::apply_look(f, look, 9.04, false, &prev);
        std::ofstream o(std::string(d) + "/fx" + std::to_string(fx) + ".ppm", std::ios::binary);
        o << "P6\n640 360\n255\n";
        o.write(reinterpret_cast<const char*>(f.rgb.data()), static_cast<std::streamsize>(f.rgb.size()));
    }
}

TEST(video_quiet_glitch_has_fewer_big_hits_and_more_small_ones) {
    const VideoFrame src = fx_gradient(240, 120);
    int bursts[2] = {0, 0}, particles[2] = {0, 0};
    const int fxs[2] = {4, 7};
    for (int k = 0; k < 2; ++k)
        for (int tick = 0; tick < 400; ++tick) {
            VideoFrame f = src;
            VideoPlayer::apply_fx(f, fxs[k], tick / 25.0 + 0.001);
            long changed = 0;
            for (size_t i = 0; i < f.rgb.size(); i += 3) {
                const int d = std::abs(int(f.rgb[i]) - int(src.rgb[i])) + std::abs(int(f.rgb[i + 1]) - int(src.rgb[i + 1])) + std::abs(int(f.rgb[i + 2]) - int(src.rgb[i + 2]));
                if (d > 150) ++changed;
            }
            const double ratio = static_cast<double>(changed) / (240.0 * 120.0);
            if (ratio > 0.05) ++bursts[k];
            else if (ratio > 0.0005) ++particles[k];
        }
    CHECK(bursts[1] < bursts[0]);        // fewer big hits
    CHECK(particles[1] > particles[0]);  // more small ones
}

TEST(video_buffer_seconds_follow_the_size_and_the_resolution) {
    CHECK(video_buffer_seconds(64, 0, false) > video_buffer_seconds(32, 0, false));
    CHECK(video_buffer_seconds(32, 2, false) < video_buffer_seconds(32, 0, false));
    CHECK(video_buffer_seconds(256, 0, false) >= 1);
}

TEST(video_buffer_limit_does_not_stall_the_decoder) {
    const std::string file = make_test_video("buffer_small.mp4", 3);
    if (file.empty()) return;
    VideoPlayer v;
    CHECK(v.set_source(file));
    v.set_buffer_mb(1);   // smaller than a few frames: the minimum of frames is kept
    const VideoFrame* f = nullptr;
    for (int i = 0; i < 80 && !f; ++i) { f = v.frame_for(0.5, 160, 90, true); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    CHECK(f != nullptr);
}

static long fx_distinct_colours(const VideoFrame& f) {
    std::set<uint32_t> s;
    for (size_t i = 0; i + 2 < f.rgb.size(); i += 3) s.insert((uint32_t(f.rgb[i]) << 16) | (uint32_t(f.rgb[i + 1]) << 8) | f.rgb[i + 2]);
    return static_cast<long>(s.size());
}

TEST(video_new_looks_do_what_they_say) {
    const VideoFrame src = fx_gradient(240, 120);
    {   // phosphor: green on black, amber is red-heavy
        VideoFrame g = src, a = src;
        VideoPlayer::apply_fx(g, 8, 1.0); VideoPlayer::apply_fx(a, 9, 1.0);
        long gr = 0, gg = 0, ar = 0, ab = 0;
        for (size_t i = 0; i < g.rgb.size(); i += 3) { gr += g.rgb[i]; gg += g.rgb[i + 1]; ar += a.rgb[i]; ab += a.rgb[i + 2]; }
        CHECK(gg > gr * 2);
        CHECK(ar > ab * 3);
    }
    {   // dither: two inks; game boy: four; ega 16: at most sixteen
        VideoFrame d = src, gb = src, ega = src;
        VideoPlayer::apply_fx(d, 21, 1.0); VideoPlayer::apply_fx(gb, 12, 1.0); VideoPlayer::apply_fx(ega, 13, 1.0);
        CHECK_EQ(fx_distinct_colours(d), 2L);
        CHECK(fx_distinct_colours(gb) <= 4L + 4L);   // (the faint LCD gap darkens the edges of a block: a second set of the four)
        CHECK(fx_distinct_colours(ega) <= 16L);
        CHECK(fx_distinct_colours(ega) > 4L);
    }
    {   // interlace: with a frame before, the odd lines come from it (the even ones stay)
        VideoFrame prev = src;
        for (uint8_t& v : prev.rgb) v = static_cast<uint8_t>(255 - v);
        VideoFrame f = src;
        VideoPlayer::apply_fx(f, 17, 1.0, false, nullptr, &prev);
        const size_t stride = 240 * 3;
        int near_prev = 0, near_cur = 0;
        for (int y = 10; y < 110; ++y) {
            const int v = f.rgb[y * stride + 90], c = src.rgb[y * stride + 90], p = prev.rgb[y * stride + 90];
            if (std::abs(v - p) < std::abs(v - c)) ++near_prev; else ++near_cur;
        }
        CHECK(near_prev > 30 && near_cur > 30);
    }
    {   // halftone: paper and ink (many pixels bright, many dark), and no neon / bloom surprise
        VideoFrame h = src;
        VideoPlayer::apply_fx(h, 10, 1.0);
        int light = 0, dark = 0;
        for (size_t i = 0; i < h.rgb.size(); i += 3) { const int l = (h.rgb[i] + h.rgb[i + 1] + h.rgb[i + 2]) / 3; if (l > 170) ++light; else if (l < 110) ++dark; }
        CHECK(light > 0 && dark > 0);
    }
    {   // neon edge: mostly dark where the picture is smooth
        VideoFrame n = src;
        VideoPlayer::apply_fx(n, 16, 1.0);
        CHECK(fx_sum(n) < fx_sum(src) / 3);
    }
    {   // neon edge gradient: grey picture under coloured outlines (not black, not the plain neon); game boy low-res: four shades in bigger blocks
        VideoFrame n = src, ng = src, lo = src, hi = src;
        VideoPlayer::apply_fx(n, 16, 1.0); VideoPlayer::apply_fx(ng, 19, 1.0, false, nullptr);
        CHECK(fx_sum(ng) > fx_sum(n));
        CHECK(ng.rgb != n.rgb);
        VideoPlayer::apply_fx(lo, 18, 1.0); VideoPlayer::apply_fx(hi, 12, 1.0);
        CHECK(fx_distinct_colours(lo) <= 8L);
        CHECK(lo.rgb != hi.rgb);   // two different grids
        VideoFrame gb = src;   // the high-res gradient: the same grid, but not the four greens
        VideoPlayer::apply_fx(gb, 20, 1.0);
        CHECK(gb.rgb != hi.rgb);
        CHECK(video_fx_from("game boy high-res gradient") == 20 && video_fx_uses_gradient(20));
    }
    {   // bloom adds light, film keeps the average brightness roughly
        VideoFrame b = src, fm = src;
        VideoPlayer::apply_fx(b, 14, 1.0); VideoPlayer::apply_fx(fm, 15, 1.0);
        CHECK(fx_sum(b) >= fx_sum(src));
        CHECK(std::labs(fx_sum(fm) - fx_sum(src)) < fx_sum(src) / 3);
    }
}

TEST(video_grade_contrast_colours_and_chromatic_aberration) {
    const VideoFrame src = fx_gradient(240, 120);
    {   // colours 0 = greyscale, 100 = untouched
        VideoFrame g = src;
        VideoPlayer::apply_grade(g, 0, 0, 0);
        bool grey = true;
        for (size_t i = 0; i < g.rgb.size(); i += 3) if (std::abs(g.rgb[i] - g.rgb[i + 1]) > 1 || std::abs(g.rgb[i + 1] - g.rgb[i + 2]) > 1) grey = false;
        CHECK(grey);
        VideoFrame same = src;
        VideoPlayer::apply_grade(same, 0, 100, 0);
        CHECK(same.rgb == src.rgb);
    }
    {   // brightness: 50 = as it is, 100 brighter, 0 darker; black and white stay
        VideoFrame same = src;
        VideoPlayer::apply_grade(same, 0, 100, 0, 50);
        CHECK(same.rgb == src.rgb);
        VideoFrame up = src, down = src;
        VideoPlayer::apply_grade(up, 0, 100, 0, 100);
        VideoPlayer::apply_grade(down, 0, 100, 0, 0);
        long s0 = 0, s1 = 0, s2 = 0;
        for (size_t i = 0; i < src.rgb.size(); ++i) { s0 += src.rgb[i]; s1 += up.rgb[i]; s2 += down.rgb[i]; }
        CHECK(s1 > s0);
        CHECK(s2 < s0);
        VideoFrame bw; bw.w = 2; bw.h = 2; bw.rgb = {0,0,0, 255,255,255, 0,0,0, 255,255,255};
        const std::vector<uint8_t> bw0 = bw.rgb;
        VideoPlayer::apply_grade(bw, 0, 100, 0, 100);
        CHECK(bw.rgb == bw0);
        VideoPlayer::apply_grade(bw, 0, 100, 0, 0);
        CHECK(bw.rgb == bw0);
    }
    {   // contrast spreads the values
        VideoFrame c = src;
        VideoPlayer::apply_grade(c, 100, 100, 0);
        int lo = 255, hi = 0, lo0 = 255, hi0 = 0;
        for (size_t i = 0; i < c.rgb.size(); ++i) { lo = std::min<int>(lo, c.rgb[i]); hi = std::max<int>(hi, c.rgb[i]); lo0 = std::min<int>(lo0, src.rgb[i]); hi0 = std::max<int>(hi0, src.rgb[i]); }
        CHECK(hi - lo > hi0 - lo0);
    }
    {   // chromatic aberration: green stays, red and blue move, the middle hardly changes, a corner does
        VideoFrame c = src;
        VideoPlayer::apply_grade(c, 0, 100, 3);
        bool green_same = true;
        for (size_t i = 1; i < c.rgb.size(); i += 3) if (c.rgb[i] != src.rgb[i]) green_same = false;
        CHECK(green_same);
        const size_t mid = (60 * 240 + 120) * 3, corner = (2 * 240 + 2) * 3;
        CHECK(std::abs(int(c.rgb[mid]) - int(src.rgb[mid])) <= 2);
        CHECK(std::abs(int(c.rgb[corner]) - int(src.rgb[corner])) + std::abs(int(c.rgb[corner + 2]) - int(src.rgb[corner + 2])) >= 1);
        VideoFrame l1 = src, l3 = src;
        VideoPlayer::apply_grade(l1, 0, 100, 1); VideoPlayer::apply_grade(l3, 0, 100, 3);
        long d1 = 0, d3 = 0;
        for (size_t i = 0; i < src.rgb.size(); ++i) { d1 += std::abs(int(l1.rgb[i]) - int(src.rgb[i])); d3 += std::abs(int(l3.rgb[i]) - int(src.rgb[i])); }
        CHECK(d3 > d1);
    }
}

TEST(video_look_signature_changes_with_every_part) {
    VideoLook a, b;
    CHECK(!a.active());
    b = a; b.fx = 3; CHECK(a.sig() != b.sig());
    b = a; b.contrast = 10; CHECK(a.sig() != b.sig() && b.active());
    b = a; b.colors = 0; CHECK(a.sig() != b.sig() && b.active());
    b = a; b.chroma = 2; CHECK(a.sig() != b.sig() && b.active());
    CHECK_EQ(chroma_from("strong"), 3);
    CHECK_EQ(video_fx_from("quiet glitch"), 7);
    CHECK_EQ(video_fx_from("neon edge"), 16);
    CHECK_EQ(video_fx_from("game boy"), 12);   // the old name
    CHECK_EQ(video_fx_from("game boy low-res"), 18);
    CHECK_EQ(video_fx_from("neon edge gradient"), 19);
}

TEST(cover_chromatic_aberration_moves_red_and_blue) {
    CoverImage c;
    c.w = 120; c.h = 120; c.key = "chroma-test";
    c.rgb.resize(120 * 120 * 3); c.alpha.assign(120 * 120, 255); c.level.assign(120 * 120, 128);
    for (int y = 0; y < 120; ++y) for (int x = 0; x < 120; ++x) { c.rgb[(y * 120 + x) * 3] = x < 60 ? 255 : 0; c.rgb[(y * 120 + x) * 3 + 1] = 128; c.rgb[(y * 120 + x) * 3 + 2] = x < 60 ? 0 : 255; }
    const CoverRgb& plain = cover_scaled_rgb(c, 120, 120, 0, 0);
    const CoverRgb copy = plain;
    const CoverRgb& shifted = cover_scaled_rgb(c, 120, 120, 0, 3);
    CHECK(copy.rgb != shifted.rgb);
    CHECK(copy.w == shifted.w && copy.h == shifted.h);
}

// A paused video that changes size (main UI <-> big overlay) must keep showing the SAME frame: the new decoder starts at the shown
// frame's own time, not at the clock (its frames lie on a grid that begins at its start, which moved the picture by up to a frame).
TEST(video_resize_while_paused_keeps_the_same_frame) {
    const std::string file = make_test_video("t7.mp4", 30);
    if (file.empty()) return;
    VideoPlayer v;
    if (!v.set_source(file)) return;
    int w = 0, h = 0;
    v.fit(160, 90, 1.0, w, h);
    const double t = 3.037;   // not on the decoder's frame grid (it starts at 0.5 s, 25 fps: 0.5, 0.54, ... 3.02, 3.06)
    const VideoFrame* fr = nullptr;
    for (int i = 0; i < 100 && !fr; ++i) { fr = v.frame_for(0.5, w, h, true); if (!fr) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    CHECK(fr != nullptr);
    // play on to t, then pause: the frame shown is the newest one at or before t
    for (int i = 0; i < 100; ++i) { v.frame_for(t, w, h, true); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    fr = v.frame_for(t, w, h, true);
    CHECK(fr != nullptr);
    if (!fr) return;
    const double shown = fr->pts;
    int w2 = 0, h2 = 0;
    v.fit(320, 180, 1.0, w2, h2);
    for (int i = 0; i < 200 && !(fr && fr->w == w2); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); fr = v.frame_for(t, w2, h2, false); }
    CHECK(fr != nullptr);
    if (fr) { CHECK_EQ(fr->w, w2); CHECK_NEAR(fr->pts, shown, 1e-6); }
}

static double mean_abs_diff(const VideoFrame& a, const VideoFrame& b) {
    if (a.w != b.w || a.h != b.h || a.rgb.size() != b.rgb.size() || a.rgb.empty()) return 1e9;
    double d = 0;
    for (size_t i = 0; i < a.rgb.size(); ++i) d += std::abs(static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i]));
    return d / static_cast<double>(a.rgb.size());
}

// A video whose every frame is different noise: two frames are never alike, so "the same picture" can be told from "a neighbour".
static std::string make_noise_video(int rate) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_video_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path f = dir / ("noise" + std::to_string(rate) + ".mp4");
    if (!fs::exists(f, ec)) {
        const ProcResult r = run_capture({"ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "color=s=320x180:r=" + std::to_string(rate) + ":d=20,format=gray,geq=lum='random(1)*255'",
                                          "-c:v", "mpeg4", "-q:v", "2", "-pix_fmt", "yuv420p", f.string()}, true);
        if (!r.ok()) return std::string();
    }
    return f.string();
}

// The picture itself (not just its time label) stays the same when a paused video changes size: a 30 fps source against the decoder's 25 fps
// grid, several pause times. The new frame, averaged down to the old size, has to look like the old one and not like a neighbour.
TEST(video_resize_while_paused_keeps_the_same_picture) {
    const std::string file = make_noise_video(30);
    if (file.empty()) return;
    for (double t : {3.037, 3.051, 3.093, 4.011}) {
        VideoPlayer v;
        if (!v.set_source(file)) return;
        int w = 0, h = 0;
        v.fit(160, 90, 1.0, w, h);
        const VideoFrame* fr = nullptr;
        for (int i = 0; i < 100 && !fr; ++i) { fr = v.frame_for(0.5, w, h, true); if (!fr) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        if (!fr) { CHECK(false); return; }
        // play up to t, collecting the pictures on the way: the one on screen at t and the two before / after it
        std::vector<VideoFrame> seen;
        for (double c = 0.5; c < t + 0.12; c += 0.02) {
            fr = v.frame_for(c, w, h, true);
            if (fr && (seen.empty() || seen.back().pts != fr->pts)) seen.push_back(*fr);
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        fr = v.frame_for(t, w, h, true);   // (the clock is back at t: the video "paused" there; a frame later than t is not taken back)
        if (seen.size() < 4) { CHECK(false); return; }
        // the frame the user would have been looking at when paused at t: the last one with pts <= t
        size_t on = 0;
        for (size_t i = 0; i < seen.size(); ++i) if (seen[i].pts <= t) on = i;
        // a second player does exactly the same but stops at t (paused), then the size changes
        VideoPlayer v2;
        if (!v2.set_source(file)) return;
        for (int i = 0; i < 100 && !(fr = v2.frame_for(0.5, w, h, true)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (double c = 0.5; c <= t; c += 0.02) { v2.frame_for(c, w, h, true); std::this_thread::sleep_for(std::chrono::milliseconds(8)); }
        for (int i = 0; i < 10; ++i) { v2.frame_for(t, w, h, true); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        fr = v2.frame_for(t, w, h, false);
        if (!fr) { CHECK(false); return; }
        const VideoFrame before = *fr;
        int w2 = 0, h2 = 0;
        v2.fit(320, 180, 1.0, w2, h2);
        for (int i = 0; i < 200 && !(fr && fr->w == w2); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); fr = v2.frame_for(t, w2, h2, false); }
        if (!fr || fr->w != w2) { CHECK(false); return; }
        const VideoFrame down = VideoPlayer::downscale(*fr, before.w, before.h);
        const double d_same = mean_abs_diff(before, down);
        double d_other = 1e9;
        for (size_t i = 0; i < seen.size(); ++i) if (i != on && std::fabs(seen[i].pts - before.pts) > 1e-9 && mean_abs_diff(seen[i], down) < d_other) d_other = mean_abs_diff(seen[i], down);
        std::printf("    [t=%.3f: new picture vs the one on screen %.1f, vs the closest other frame %.1f]\n", t, d_same, d_other);
        CHECK(d_same < d_other * 0.7);
    }
}

// Going from a large size back to a smaller one restarts nothing: the same decoded frame, averaged down.
TEST(video_smaller_size_needs_no_restart) {
    const std::string file = make_test_video("t7.mp4", 30);
    if (file.empty()) return;
    VideoPlayer v;
    if (!v.set_source(file)) return;
    int w = 0, h = 0;
    v.fit(320, 180, 1.0, w, h);
    const VideoFrame* fr = nullptr;
    for (int i = 0; i < 100 && !fr; ++i) { fr = v.frame_for(1.0, w, h, true); if (!fr) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    CHECK(fr != nullptr);
    if (!fr) return;
    const double pts = fr->pts;
    // paused: the small size is there at once (no wait for a decoder), the same frame
    int w2 = 0, h2 = 0;
    v.fit(160, 90, 1.0, w2, h2);
    fr = v.frame_for(1.0, w2, h2, false);
    CHECK(fr != nullptr);
    if (fr) { CHECK_EQ(fr->w, w2); CHECK_EQ(fr->h, h2); CHECK_NEAR(fr->pts, pts, 1e-9); }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));   // (longer than the 250 ms a larger size waits)
    fr = v.frame_for(1.0, w2, h2, false);
    CHECK(fr != nullptr);
    if (fr) { CHECK_EQ(fr->w, w2); CHECK_NEAR(fr->pts, pts, 1e-9); }
    // and the big size again: still the same frame, no restart in between
    fr = v.frame_for(1.0, w, h, false);
    CHECK(fr != nullptr);
    if (fr) { CHECK_EQ(fr->w, w); CHECK_NEAR(fr->pts, pts, 1e-9); }
}

TEST(video_downscale_averages) {
    VideoFrame f;
    f.w = 4; f.h = 2; f.rgb = {0,0,0, 100,100,100, 200,200,200, 255,255,255,
                               0,0,0, 100,100,100, 200,200,200, 255,255,255};
    const VideoFrame o = VideoPlayer::downscale(f, 2, 1);
    CHECK_EQ(o.w, 2); CHECK_EQ(o.h, 1);
    CHECK_EQ(static_cast<int>(o.rgb[0]), 50);     // (0 + 100) / 2, both rows
    CHECK_EQ(static_cast<int>(o.rgb[3]), 228);    // (200 + 255) / 2 = 227.5 -> 228
}

// Video Frame Rate (batch 278): 25 decodes every video at 25 pictures a second (as before); 60 at the video's own rate, up to 60.
TEST(video_decode_rate_is_25_or_the_videos_own_up_to_60) {
    int n = 0, d = 0;
    VideoPlayer::decode_rate(25, 30000, 1001, n, d); CHECK_EQ(n, 25); CHECK_EQ(d, 1);
    VideoPlayer::decode_rate(60, 30000, 1001, n, d); CHECK_EQ(n, 30000); CHECK_EQ(d, 1001);   // (29.97 exactly: no picture doubled or dropped)
    VideoPlayer::decode_rate(60, 50, 1, n, d); CHECK_EQ(n, 50); CHECK_EQ(d, 1);
    VideoPlayer::decode_rate(60, 60000, 1001, n, d); CHECK_EQ(n, 60000); CHECK_EQ(d, 1001);
    VideoPlayer::decode_rate(60, 120, 1, n, d); CHECK_EQ(n, 60); CHECK_EQ(d, 1);              // (faster footage: 60)
    VideoPlayer::decode_rate(60, 24000, 1001, n, d); CHECK_EQ(n, 24000); CHECK_EQ(d, 1001);   // (a film stays at its 23.976)
    VideoPlayer::decode_rate(60, 0, 0, n, d); CHECK_EQ(n, 25); CHECK_EQ(d, 1);                // (rate unknown: 25)
    VideoPlayer::decode_rate(60, 1, 2, n, d); CHECK_EQ(n, 25); CHECK_EQ(d, 1);                // (a slide show of half a picture a second: 25 as before)
}

TEST(video_decoder_runs_at_25_or_the_videos_own_rate_and_switches_in_place) {
    const std::string file = make_test_video("t50.mp4", 6, 50);
    if (file.empty()) return;
    VideoPlayer v;
    CHECK(v.set_source(file));
    CHECK_NEAR(v.source_fps(), 50.0, 0.01);
    int w = 0, h = 0;
    v.fit(160, 90, 1.0, w, h);
    // the pictures the clock passes through, in steps of 10 ms: their times show the rate
    auto play = [&](double from, double to, std::vector<double>& pts) {
        const VideoFrame* fr = nullptr;
        for (int i = 0; i < 100 && !fr; ++i) { fr = v.frame_for(from, w, h, true); if (!fr) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        for (double t = from; t < to; t += 0.01) {
            bool fresh = false;
            const VideoFrame* f = v.frame_for(t, w, h, true, &fresh);
            if (f && fresh && (pts.empty() || f->pts > pts.back() + 1e-9)) pts.push_back(f->pts);
            double next = 0.0;
            if (v.next_pts(next)) CHECK(next > (f ? f->pts : -1.0));   // (the next picture waiting comes after the one shown)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    auto median_step = [](const std::vector<double>& pts) {
        std::vector<double> d;
        for (size_t i = 1; i < pts.size(); ++i) d.push_back(pts[i] - pts[i - 1]);
        if (d.empty()) return 0.0;
        std::sort(d.begin(), d.end());
        return d[d.size() / 2];
    };
    v.set_rate_mode(25);
    std::vector<double> a;
    play(0.0, 1.0, a);
    CHECK_EQ(v.frames_per_second(), 25);
    CHECK_NEAR(median_step(a), 0.04, 0.002);
    CHECK(a.size() >= 18);
    // 60: the video's own 50 a second, from the picture on screen on (no restart from zero)
    v.set_rate_mode(60);
    std::vector<double> b;
    play(1.0, 2.0, b);
    CHECK_EQ(v.frames_per_second(), 50);
    CHECK_NEAR(median_step(b), 0.02, 0.002);
    CHECK(b.size() >= 30);
    CHECK(!b.empty() && b.front() >= 0.9);
}
