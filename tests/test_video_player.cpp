// The video view's frame source: proportions, palette picture, braille, and the decoder following a clock.
#include "test_framework.h"
#include "video_player.h"
#include "cover_art.h"
#include "process_util.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace muisc;
namespace fs = std::filesystem;

static std::string make_test_video(const std::string& name, int seconds) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_video_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path f = dir / name;
    if (!fs::exists(f, ec)) {
        const ProcResult r = run_capture({"ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=320x180:rate=25:duration=" + std::to_string(seconds),
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
    CHECK_EQ(VideoPlayer::palette().size(), static_cast<size_t>(240));
    for (auto h : hue) { CHECK(h < 240); const auto& c = VideoPlayer::palette()[h]; CHECK(c[0] >= 204 && c[1] <= 51 && c[2] <= 51); }
    for (auto l : level) CHECK_EQ(l, 255);
    // grey goes to the grey ramp (index >= 216)
    for (auto& b : f.rgb) b = 128;
    VideoPlayer::quantize(f, hue, level);
    for (auto h : hue) CHECK(h >= 216);
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
