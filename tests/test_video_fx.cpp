// The looks of the video view share the rows of a picture between threads: the result must be the same as with one thread, for every look.
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
