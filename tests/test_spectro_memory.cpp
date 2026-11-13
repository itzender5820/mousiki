// The memory of the spectrogram analysis (spectrogram.h): nothing is allocated before samples arrive, the history and the kept samples are sized to
// one page of the picture, a recompute from the kept samples is complete, and release() gives everything back.
#include <cmath>
#include <vector>

#include "spectrogram.h"
#include "test_framework.h"

using namespace muisc;

namespace {
constexpr int kRate = 44100;

// `seconds` of a tone that changes with time (so the columns differ and a missing one shows), through the analyzer in blocks like the audio's
void feed(SpectroAnalyzer& a, const SpectroSettings& s, double seconds, double t0 = 0.0, int rate = kRate) {
    std::vector<float> lr;
    const int n = static_cast<int>(rate * seconds);
    lr.reserve(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        const double t = t0 + static_cast<double>(i) / rate;
        const float v = static_cast<float>(0.4 * std::sin(2.0 * 3.14159265358979 * (300.0 + 40.0 * t) * t));
        lr.push_back(v);
        lr.push_back(0.5f * v);
    }
    a.set_visible(true);
    a.update(s);
    for (size_t o = 0; o < lr.size(); o += 2 * 4096) {
        a.push(lr.data() + o, std::min<size_t>(4096, (lr.size() - o) / 2), rate);
        a.update(s);
    }
}

std::vector<uint8_t> picture(SpectroAnalyzer& a, const SpectroSettings& s, int w = 200, int h = 40) {
    std::vector<uint8_t> idx;
    a.render(s, w, h, 0, w, idx, false);
    return idx;
}

// every picture column (of `w`) holds a pixel that is not black
int blank_columns(const std::vector<uint8_t>& idx, int w, int h) {
    int blank = 0;
    for (int x = 0; x < w; ++x) {
        bool any = false;
        for (int y = 0; y < h && !any; ++y) any = idx[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] != 0;
        if (!any) ++blank;
    }
    return blank;
}
}  // namespace

TEST(spectro_memory_nothing_is_allocated_until_samples_arrive) {
    SpectroAnalyzer a;
    SpectroSettings s;
    a.set_visible(true);
    for (int i = 0; i < 5; ++i) a.update(s);           // a spectrogram on the screen with no track
    CHECK_EQ(a.memory_bytes(), 0u);
    CHECK_EQ(a.columns(), 0u);
    const std::vector<uint8_t> blank = picture(a, s);   // drawing it is harmless
    CHECK_EQ(blank_columns(blank, 200, 40), 200);
    feed(a, s, 2.0);
    CHECK(a.memory_bytes() > 0u);
    CHECK(a.columns() > 0u);
}

TEST(spectro_memory_follows_the_time_span_and_is_far_below_a_minute_of_samples) {
    auto bytes_at = [&](int span) {
        SpectroAnalyzer a;
        SpectroSettings s;
        s.span = span;
        feed(a, s, 1.0);
        return a.memory_bytes();
    };
    const size_t b5 = bytes_at(5), b30 = bytes_at(30), b60 = bytes_at(60);
    CHECK(b5 < b30 && b30 < b60);
    // the default (30 s): about 5.7 MB of samples and 4.5 MB of history -- it was 10.8 + 8.7 MB with a fixed minute of samples and two pages
    CHECK(b30 < 12u * 1000 * 1000);
    CHECK(b30 > 8u * 1000 * 1000);
    CHECK(b5 < 6u * 1000 * 1000);
    // the waterfall looks back two pages of one channel
    SpectroAnalyzer w;
    SpectroSettings ws;
    ws.view = 1;
    feed(w, ws, 1.0);
    CHECK(w.memory_bytes() > 0u && w.memory_bytes() < 14u * 1000 * 1000);
}

TEST(spectro_memory_a_recompute_from_the_kept_samples_is_complete) {
    SpectroAnalyzer a;
    SpectroSettings s;
    s.motion = 1;    // scroll: the newest column on the right, a whole page of 30 s on screen
    feed(a, s, 40.0);
    CHECK_EQ(blank_columns(picture(a, s), 200, 40), 0);
    // another zero padding, then back: both times the page is computed again from the kept samples
    SpectroSettings t = s;
    t.zero_pad = 4;
    a.update(t);
    CHECK_EQ(blank_columns(picture(a, t), 200, 40), 0);
    a.update(s);
    CHECK_EQ(blank_columns(picture(a, s), 200, 40), 0);
    // the same picture as before (sweep: it depends on the columns only, scroll also on the clock that moves the picture on)
    SpectroSettings sw = s;
    sw.motion = 0;
    const std::vector<uint8_t> sweep_before = picture(a, sw);
    CHECK(blank_columns(sweep_before, 200, 40) <= 2);   // (the short dark gap ahead of the playhead)
    SpectroSettings t2 = sw;
    t2.window_log2 = 10;
    a.update(t2);
    CHECK(picture(a, t2) != sweep_before);   // (another window: another picture)
    a.update(sw);
    CHECK(picture(a, sw) == sweep_before);   // (the kept samples reach back over the whole page: nothing is missing, so the very same picture)
    SpectroSettings t3 = sw;
    t3.channels = 1;
    a.update(t3);
    CHECK(blank_columns(picture(a, t3), 200, 40) <= 2);
    a.update(sw);
    CHECK(picture(a, sw) == sweep_before);
}

TEST(spectro_memory_a_new_time_span_keeps_the_newest_samples) {
    SpectroAnalyzer a;
    SpectroSettings s;
    s.motion = 1;
    feed(a, s, 40.0);
    const size_t at30 = a.memory_bytes();
    SpectroSettings longer = s;
    longer.span = 60;
    a.update(longer);
    CHECK(a.memory_bytes() > at30);                       // the kept samples grew
    {
        const std::vector<uint8_t> p = picture(a, longer);
        // (a minute is on screen; the samples kept for a 30 s page -- about 32 s -- are the newest half of it: that half is computed again)
        int blank_right = 0;
        for (int x = 102; x < 200; ++x) {
            bool any = false;
            for (int y = 0; y < 40 && !any; ++y) any = p[static_cast<size_t>(y) * 200 + static_cast<size_t>(x)] != 0;
            if (!any) ++blank_right;
        }
        CHECK(blank_right <= 6);
    }
    SpectroSettings shorter = s;
    shorter.span = 10;
    a.update(shorter);
    CHECK(a.memory_bytes() < at30);                       // and shrank again: the newest samples stay
    CHECK_EQ(blank_columns(picture(a, shorter), 200, 40), 0);
    // playing on after the change fills the picture as usual
    feed(a, shorter, 3.0, 40.0);
    CHECK_EQ(blank_columns(picture(a, shorter), 200, 40), 0);
}

TEST(spectro_memory_release_gives_everything_back_and_the_next_samples_start_afresh) {
    SpectroAnalyzer a;
    SpectroSettings s;
    feed(a, s, 3.0);
    CHECK(a.memory_bytes() > 0u);
    CHECK(a.columns() > 0u);
    a.release();
    CHECK_EQ(a.memory_bytes(), 0u);
    CHECK_EQ(a.columns(), 0u);
    CHECK(!a.visible());
    CHECK_EQ(blank_columns(picture(a, s), 200, 40), 200);   // nothing to draw, nothing breaks
    a.release();                                              // a second call is harmless
    // the feed is off: samples pushed now are not kept
    std::vector<float> lr(8192, 0.3f);
    a.push(lr.data(), 4096, kRate);
    a.update(s);
    CHECK_EQ(a.memory_bytes(), 0u);
    // switched on again: the analysis starts from the samples that arrive from then on
    feed(a, s, 2.0, 5.0);
    CHECK(a.memory_bytes() > 0u);
    CHECK(a.columns() > 0u);
    // another rate (a hi-res track) after a release
    a.release();
    feed(a, s, 1.0, 0.0, 96000);
    CHECK_EQ(a.rate(), 96000);
    CHECK(a.columns() > 0u);
}

TEST(spectro_memory_policy_keeps_the_history_or_only_what_is_shown) {
    // history on (the default), a track playing: the analysis always runs, the memory is never given back
    for (double hidden : {0.0, 5.0, 11.0, 3600.0}) {
        for (bool shown : {false, true}) {
            const SpectroKeep k = spectro_keep_policy(true, true, shown, hidden);
            CHECK(k.analyse);
            CHECK(!k.release);
        }
    }
    // history off: it runs while a spectrogram is shown and for ten seconds after (a menu over it leaves no gap), then it stops and the memory goes back
    CHECK(spectro_keep_policy(true, false, true, 0.0).analyse);
    CHECK(spectro_keep_policy(true, false, false, 9.9).analyse);
    CHECK(!spectro_keep_policy(true, false, false, 9.9).release);
    const SpectroKeep late = spectro_keep_policy(true, false, false, 10.0);
    CHECK(!late.analyse);
    CHECK(late.release);
    CHECK(spectro_keep_policy(true, false, false, 600.0).release);
    // a spectrogram on screen is never released
    CHECK(!spectro_keep_policy(true, false, true, 600.0).release);
    CHECK(!spectro_keep_policy(false, false, true, 600.0).release);
    // no track (stopped / nothing loaded): nothing to analyse; the memory goes back once no spectrogram has been shown for ten seconds, whatever the setting
    for (bool history : {false, true}) {
        CHECK(!spectro_keep_policy(false, history, false, 0.0).analyse);
        CHECK(!spectro_keep_policy(false, history, false, 5.0).release);
        CHECK(spectro_keep_policy(false, history, false, 10.5).release);
    }
}
