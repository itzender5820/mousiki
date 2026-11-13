// The worker that makes the video's picture (video_pipe.h): the same pixels as on the spot, the newest frame wins, stale pictures are never shown.
#include <chrono>
#include <thread>

#include "test_framework.h"
#include "video_pipe.h"

using namespace muisc;

namespace {
VideoFrame test_frame(int w, int h, uint32_t seed) {
    VideoFrame f;
    f.w = w; f.h = h; f.rgb.resize(static_cast<size_t>(w) * h * 3);
    uint32_t s = seed;
    for (size_t i = 0; i < f.rgb.size(); ++i) { s = s * 1664525u + 1013904223u; f.rgb[i] = static_cast<uint8_t>((s >> 24) / 2 + (i / 3 % static_cast<size_t>(w)) * 255 / static_cast<size_t>(w) / 2); }
    return f;
}

VideoPicParams params(int fx, bool tc) {
    VideoPicParams p;
    p.look.fx = fx; p.look.contrast = 10; p.look.colors = 90;
    p.t = 2.5; p.truecolor = tc; p.round_corners = true;
    p.vcols = 30; p.vrows = 10; p.trim = 3;
    p.cut_c = 10; p.cut_r = 6; p.cut_w = 12; p.cut_h = 3;
    return p;
}

// waits (at most 10 s) until the pipe has a picture for `key`
bool wait_take(VideoPipe& pipe, const std::string& key, int w, int h, VideoPic& out) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < end) {
        if (pipe.take(key, w, h, out.rgb, out.hue, out.level)) { out.w = w; out.h = h; return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
}  // namespace

TEST(video_picture_has_round_corners_a_trimmed_edge_and_a_lyric_hole) {
    const int W = 300, H = 160;
    const VideoFrame f = test_frame(W, H, 7);
    for (int tc = 0; tc < 2; ++tc) {
        VideoPicParams p = params(0, tc != 0);
        p.look = VideoLook();   // no look
        VideoPic pic;
        video_make_picture(f, nullptr, p, pic);
        CHECK_EQ(pic.w, W);
        CHECK_EQ(pic.h, H);
        CHECK_EQ(pic.level.size(), static_cast<size_t>(W) * H);
        if (tc) { CHECK_EQ(pic.rgb.size(), static_cast<size_t>(W) * H * 3); CHECK(pic.hue.empty()); }
        else { CHECK_EQ(pic.hue.size(), static_cast<size_t>(W) * H); CHECK(pic.rgb.empty()); }
        CHECK_EQ(pic.level[0], 0);                                           // the corner is cut round
        CHECK_EQ(pic.level[static_cast<size_t>(H - 1) * W], 0);
        CHECK_EQ(pic.level[static_cast<size_t>(W - p.trim - 1)], 0);         // (the right corner sits left of the trimmed columns)
        for (int y = 0; y < H; ++y) for (int x = W - p.trim; x < W; ++x) CHECK_EQ(pic.level[static_cast<size_t>(y) * W + x], 0);   // the trim
        CHECK_EQ(pic.level[static_cast<size_t>(H / 2) * W + W / 2 - 40], 255);   // the middle of the picture is there
        const int xa = p.cut_c * W / p.vcols, ya = p.cut_r * H / p.vrows;
        CHECK_EQ(pic.level[static_cast<size_t>(ya) * W + xa], 0);            // the lyric hole
        CHECK_EQ(pic.level[static_cast<size_t>(ya + 2) * W + xa + 5], 0);
    }
}

TEST(video_pipe_gives_the_same_pixels_as_making_the_picture_on_the_spot) {
    const int W = 301, H = 171;
    const VideoFrame f = test_frame(W, H, 11);
    VideoFrame prev = test_frame(W, H, 12);
    VideoPipe pipe;
    for (int fx : {0, 1, 3, 4, 6, 17, 20}) {
        for (int tc = 0; tc < 2; ++tc) {
            const VideoPicParams p = params(fx, tc != 0);
            VideoPic want;
            video_make_picture(f, &prev, p, want);
            const std::string key = "k" + std::to_string(fx) + (tc ? "t" : "p");
            CHECK(pipe.submit(key, f, 0, 0, W, H, &prev, p));
            VideoPic got;
            CHECK(wait_take(pipe, key, W, H, got));
            CHECK(got.rgb == want.rgb);
            CHECK(got.hue == want.hue);
            CHECK(got.level == want.level);
        }
    }
}

TEST(video_pipe_cuts_the_visible_part_out_of_the_frame_like_crop) {
    const VideoFrame f = test_frame(320, 180, 3);
    VideoPipe pipe;
    const VideoFrame part = VideoPlayer::crop(f, 20, 10, 280, 160);
    VideoPicParams p = params(4, false);
    VideoPic want;
    video_make_picture(part, nullptr, p, want);
    CHECK(pipe.submit("c", f, 20, 10, 280, 160, nullptr, p));
    VideoPic got;
    CHECK(wait_take(pipe, "c", 280, 160, got));
    CHECK(got.hue == want.hue && got.level == want.level);
}

TEST(video_pipe_newest_frame_wins_and_nothing_piles_up) {
    const int W = 301, H = 171;
    const VideoFrame f = test_frame(W, H, 5);
    VideoPipe pipe;
    VideoPicParams p = params(4, false);
    for (int i = 0; i < 40; ++i) CHECK(pipe.submit("k" + std::to_string(i), f, 0, 0, W, H, nullptr, p));   // (much faster than the worker can follow)
    VideoPic got;
    CHECK(wait_take(pipe, "k39", W, H, got));   // the last one is made, whatever happened to the ones before it
    CHECK(pipe.dropped() >= 1);
    CHECK(pipe.dropped() <= 39);
}

TEST(video_pipe_never_hands_out_a_picture_for_another_key_or_size_or_after_cancel) {
    const int W = 200, H = 120;
    const VideoFrame f = test_frame(W, H, 9);
    VideoPipe pipe;
    VideoPicParams p = params(3, false);
    std::vector<uint8_t> rgb, hue, level;
    // another key: dropped
    CHECK(pipe.submit("a", f, 0, 0, W, H, nullptr, p));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!pipe.take("b", W, H, rgb, hue, level));
    CHECK(!pipe.take("a", W, H, rgb, hue, level));   // (and it was taken away by the try: a picture is handed out once)
    // another size: dropped
    CHECK(pipe.submit("a", f, 0, 0, W, H, nullptr, p));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!pipe.take("a", W + 2, H, rgb, hue, level));
    // cancelled: nothing comes, a picture that was still in the making is thrown away
    CHECK(pipe.submit("a", f, 0, 0, W, H, nullptr, p));
    pipe.cancel();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!pipe.take("a", W, H, rgb, hue, level));
    // and it works again afterwards
    CHECK(pipe.submit("a", f, 0, 0, W, H, nullptr, p));
    VideoPic got;
    CHECK(wait_take(pipe, "a", W, H, got));
}

TEST(video_pipe_switches_to_the_worker_for_heavy_pictures_and_back) {
    VideoPipe pipe;
    CHECK(!pipe.wants_thread());                 // (nothing is known yet: on the spot)
    for (int i = 0; i < 6; ++i) pipe.note_cost(1.0);
    CHECK(!pipe.wants_thread());
    for (int i = 0; i < 3; ++i) pipe.note_cost(20.0);
    CHECK(pipe.wants_thread());                  // (a heavy look: 20 ms a picture)
    for (int i = 0; i < 40; ++i) pipe.note_cost(20.0);
    CHECK(pipe.wants_thread());
    for (int i = 0; i < 40; ++i) pipe.note_cost(4.0);   // (between the two limits: it stays as it is)
    CHECK(pipe.wants_thread());
    for (int i = 0; i < 40; ++i) pipe.note_cost(1.0);
    CHECK(!pipe.wants_thread());
}
