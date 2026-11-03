// The Hi-Fi spectrum bars: where a tone shows up, the stereo halves, the braille drawing.
#include "test_framework.h"
#include "hifi_bars.h"

#include <chrono>
#include <cmath>
#include <thread>

using namespace muisc;

static int argmax(const std::vector<float>& v) {
    int b = 0;
    for (size_t i = 1; i < v.size(); ++i) if (v[i] > v[static_cast<size_t>(b)]) b = static_cast<int>(i);
    return b;
}

static void feed_tones(HifiBars& h, double fl, double fr) {
    h.bands(2, 8, 0.03, 1, 10, 0);   // marks the bars as in use: only then the feed is taken
    std::vector<float> lr(2 * 4096);
    for (int i = 0; i < 4096; ++i) {
        lr[2 * static_cast<size_t>(i)] = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * fl * i / 48000.0));
        lr[2 * static_cast<size_t>(i) + 1] = fr > 0 ? 0.5f * static_cast<float>(std::sin(2.0 * M_PI * fr * i / 48000.0)) : 0.0f;
    }
    h.push(lr.data(), 4096, 48000);
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
}

TEST(hifi_bands_follow_the_frequency_of_each_channel) {
    HifiBars h;
    feed_tones(h, 220.0, 3000.0);
    const auto l = h.bands(0, 200, 0.5, 1, 10, 0);
    const int pl = argmax(l);
    const auto r = h.bands(1, 200, 0.5, 1, 10, 0);
    const int pr = argmax(r);
    CHECK(pl >= 8 && pl <= 22);      // 220 Hz: low on the mel scale (about 14 of 200)
    CHECK(pr >= 90 && pr <= 110);    // 3000 Hz: about the middle (100 of 200)
    CHECK(l[static_cast<size_t>(pl)] > 0.5f);
    CHECK(r[static_cast<size_t>(pl)] < 0.3f);   // the right channel has nothing at 220 Hz
}

TEST(hifi_rows_draw_braille_columns_with_the_bass_in_the_middle) {
    HifiBars h;
    feed_tones(h, 220.0, 0.0);   // only the left channel
    HifiView v;
    v.cells = 40; v.rows = 4; v.resolution = 0; v.channels = 0; v.fluidity = 1; v.degradation = 10; v.viscosity = 0; v.dt = 0.5;
    const auto grad = [](float) { return std::string("\x1b[38;2;1;2;3m"); };
    const auto hei = [](float) { return std::string("\x1b[38;2;9;9;9m"); };
    auto rows = hifi_rows(h, v, grad, hei);
    CHECK_EQ(static_cast<int>(rows.size()), 4);
    // the lit cells of the bottom row: only in the left half, near its right end (the bass is at the middle)
    const std::string& bottom = rows[3];
    int cell = 0, first = -1, last = -1;
    for (size_t i = 0; i < bottom.size();) {
        const unsigned char c = static_cast<unsigned char>(bottom[i]);
        if (c == 0x1b) { while (i < bottom.size() && bottom[i] != 'm' && bottom[i] != '[') ++i; if (bottom[i] == '[') while (i < bottom.size() && bottom[i] != 'm') ++i; ++i; continue; }
        if (c == ' ') { ++cell; ++i; continue; }
        if (c >= 0xE0) { if (first < 0) first = cell; last = cell; ++cell; i += 3; continue; }
        ++i;
    }
    CHECK(first >= 0);
    CHECK(last < 20);      // nothing in the right half
    CHECK(last >= 12);     // the tone sits towards the middle
    // mono: one mix across the width, the low tone at the left
    v.channels = 1;
    HifiBars h2;
    feed_tones(h2, 220.0, 0.0);
    rows = hifi_rows(h2, v, grad, hei);
    CHECK_EQ(static_cast<int>(rows.size()), 4);
}

#include "settings.h"

TEST(bars_behavior_presets_and_the_values_of_each_style) {
    CHECK_EQ(bars_preset_of(10, 8, 3), 1);                 // the old defaults are "default"
    CHECK_EQ(bars_preset_of(2, 9, 1), 0);
    CHECK_EQ(bars_preset_of(10, 4, 6), 2);
    CHECK_EQ(bars_preset_of(10, 2, 8), 3);
    CHECK_EQ(bars_preset_of(5, 5, 5), kBarsCustom);
    Settings s;
    s.bars_style = 0;
    s.bar_fluidity() = 2; s.bar_degradation() = 9; s.bar_viscosity() = 1;   // Classic: lively
    CHECK_EQ(s.visualizer_fluidity, 2);
    CHECK_EQ(s.hifi_fluidity, 10);                          // Hi-Fi keeps its own values
    s.bars_style = 1;
    CHECK_EQ(bars_preset_of(s.bar_fluidity(), s.bar_degradation(), s.bar_viscosity()), 1);
    s.bar_degradation() = 2; s.bar_viscosity() = 8;         // Hi-Fi: slow
    CHECK_EQ(s.hifi_degradation, 2);
    CHECK_EQ(s.visualizer_degradation_speed, 9);            // Classic is untouched
}
