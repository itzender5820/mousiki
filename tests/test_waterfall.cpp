// The waterfall view of the spectrogram: a stack of outlined graphs made from the same analysis.
#include "test_framework.h"
#include "spectrogram.h"
#include "scope_window.h"

#include <cmath>
#include <sstream>
#include <vector>

using namespace muisc;

namespace {
// feeds `seconds` of a 1 kHz tone (and a little of a 5 kHz one) through the analyzer
void feed_tone(SpectroAnalyzer& a, const SpectroSettings& s, double seconds) {
    const int rate = 44100;
    std::vector<float> lr;
    for (int i = 0; i < static_cast<int>(rate * seconds); ++i) {
        const float v = 0.4f * std::sin(2.0 * 3.14159265358979 * 1000.0 * i / rate) + 0.1f * std::sin(2.0 * 3.14159265358979 * 5000.0 * i / rate);
        lr.push_back(v);
        lr.push_back(v);
    }
    a.set_visible(true);
    a.update(s);                                // switches the feed on
    for (size_t o = 0; o < lr.size(); o += 2 * 4096) {
        a.push(lr.data() + o, std::min<size_t>(4096, (lr.size() - o) / 2), rate);
        a.update(s);
    }
}
}

TEST(waterfall_settings_rows_config_and_visibility) {
    SpectroSettings s;
    CHECK_EQ(s.view, 0);
    CHECK_EQ(spectro_row_value(s, kSrView), std::string("spectrogram"));
    spectro_adjust(s, kSrView, +1);
    CHECK_EQ(s.view, 1);
    CHECK_EQ(spectro_row_value(s, kSrView), std::string("waterfall"));
    CHECK(!spectro_row_visible(s, kSrMotion) && !spectro_row_visible(s, kSrChannels) && !spectro_row_visible(s, kSrSpan));
    CHECK(spectro_row_visible(s, kSrWfGraphs) && spectro_row_visible(s, kSrWfHeight));
    s.view = 0;
    CHECK(spectro_row_visible(s, kSrMotion) && !spectro_row_visible(s, kSrWfSlant));
    s.view = 1;
    for (int i = 0; i < 30; ++i) spectro_adjust(s, kSrWfGraphs, +1);
    CHECK_EQ(s.wf_graphs, 100);
    for (int i = 0; i < 30; ++i) spectro_adjust(s, kSrWfSpacing, -1);
    CHECK_EQ(s.wf_spacing, 20);
    for (int i = 0; i < 30; ++i) spectro_adjust(s, kSrWfSlant, +1);
    CHECK_EQ(s.wf_slant, 40);
    // config round trip
    std::ostringstream out;
    spectro_config_write(out, s, "##");
    SpectroSettings t;
    std::istringstream in(out.str());
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        spectro_config_key(t, line.substr(0, eq), line.substr(eq + 1));
    }
    CHECK_EQ(t.view, 1);
    CHECK_EQ(t.wf_graphs, 100);
    CHECK_EQ(t.wf_spacing, 20);
    CHECK_EQ(t.wf_slant, 40);
    CHECK_EQ(s.wf_detail, 0);
    spectro_adjust(s, kSrWfDetail, +1);
    CHECK_EQ(s.wf_detail, 1);
    CHECK_EQ(spectro_row_value(s, kSrWfDetail), std::string("high"));
    {
        std::ostringstream o2;
        spectro_config_write(o2, s, "##");
        SpectroSettings u;
        std::istringstream in2(o2.str());
        std::string l2;
        while (std::getline(in2, l2)) { const size_t e = l2.find('='); if (!l2.empty() && l2[0] != '#' && e != std::string::npos) spectro_config_key(u, l2.substr(0, e), l2.substr(e + 1)); }
        CHECK_EQ(u.wf_detail, 1);
    }
    spectro_adjust(s, kSrWfDetail, -1);
    spectro_reset(t);
    CHECK_EQ(t.view, 1);            // the reset keeps the view
    CHECK_EQ(t.wf_graphs, 40);
}

TEST(waterfall_picture_has_black_outlines_and_filled_graphs) {
    SpectroSettings s;
    s.view = 1;
    s.wf_graphs = 24;
    s.wf_spacing = 40;
    SpectroAnalyzer& a = spectro();
    a.reset();
    feed_tone(a, s, 3.0);
    const int w = 320, h = 160;
    std::vector<uint8_t> img;
    a.waterfall(s, w, h, false, img);
    CHECK_EQ(img.size(), static_cast<size_t>(w) * h);
    size_t outline = 0, fill = 0, bg = 0;
    for (uint8_t v : img) { if (v == 1) ++outline; else if (v >= 48) ++fill; else if (v == 0) ++bg; else CHECK(false); }
    CHECK(outline > 500);           // every graph has its black line
    CHECK(fill > 5000);             // and its coloured body
    CHECK(bg > 0);                  // the corners stay empty
    // the colours of the palette in use: background and outline are black, the fill is not
    const auto& pal = spectro_palette_view(s);
    CHECK(pal[0][0] == 0 && pal[0][1] == 0 && pal[0][2] == 0 && pal[1][0] == 0 && pal[1][1] == 0 && pal[1][2] == 0);
    CHECK(pal[200][0] + pal[200][1] + pal[200][2] > 100);
    // high resolution: indices are register * 4, black background and outline, a fill and some half dark outline pixels
    SpectroSettings hs = s;
    hs.wf_detail = 1;
    std::vector<uint8_t> hi;
    a.waterfall(hs, w, h, false, hi);
    CHECK_EQ(hi.size(), static_cast<size_t>(w) * h);
    size_t black = 0, bright = 0, half = 0;
    for (uint8_t v : hi) { CHECK((v & 3) == 0); const int r = v >> 2; if (r == 0) ++black; else if (r <= 40) ++bright; else ++half; }
    CHECK(black > 500 && bright > 5000 && half > 50);
    const auto& hp = spectro_palette_view(hs);
    CHECK(hp[0][0] == 0 && hp[2][1] == 0 && hp[4 * 20][0] + hp[4 * 20][1] + hp[4 * 20][2] > 100);
    CHECK(hp[4 * 55][0] + hp[4 * 55][1] + hp[4 * 55][2] > 0);
    // braille: only lines (no outline index, no fill)
    std::vector<uint8_t> lines;
    a.waterfall(s, w, h, true, lines);
    size_t lit = 0;
    for (uint8_t v : lines) { if (v != 0) { ++lit; CHECK(v >= 48); } }
    CHECK(lit > 300 && lit < static_cast<size_t>(w) * h / 3);
    // the tiles of a terminal picture are slices of that picture
    std::vector<uint8_t> tile;
    a.render(s, w, h, 100, 40, tile, false);
    CHECK_EQ(tile.size(), static_cast<size_t>(40) * h);
    bool same = true;
    for (int y = 0; y < h && same; ++y)
        for (int x = 0; x < 40; ++x)
            if (tile[static_cast<size_t>(y) * 40 + x] != img[static_cast<size_t>(y) * w + 100 + x]) { same = false; break; }
    CHECK(same);
    // the slant leaves room at the right: the graph is narrower than the picture, and the axis labels fit it
    CHECK(spectro_waterfall_plot_width(s, w) < w);
    const auto labels = spectro_axis_x(s, 44100, w, 8);
    CHECK(!labels.empty());
    // braille: lines made of dots, coloured
    const auto rows = a.braille(s, 60, 20);
    CHECK_EQ(rows.size(), static_cast<size_t>(20));
    size_t dots = 0;
    for (const std::string& r : rows) for (size_t i = 0; i + 2 < r.size(); ++i) if (static_cast<unsigned char>(r[i]) == 0xE2 && static_cast<unsigned char>(r[i + 1]) >= 0xA0 && static_cast<unsigned char>(r[i + 1]) <= 0xA3) ++dots;
    CHECK(dots > 100);
    a.reset();
}

TEST(waterfall_goes_to_the_spectrogram_window) {
    SpectroSettings s;
    s.view = 1; s.wf_graphs = 52; s.wf_spacing = 80; s.wf_slant = 15; s.wf_height = 45; s.wf_detail = 1; s.scale = 1;
    SpectroWinConfig c;
    spectro_win_pack(s, c);
    const SpectroSettings u = spectro_win_unpack(c);
    CHECK_EQ(u.view, 1);
    CHECK_EQ(u.wf_graphs, 52);
    CHECK_EQ(u.wf_spacing, 80);
    CHECK_EQ(u.wf_slant, 15);
    CHECK_EQ(u.wf_height, 45);
    CHECK_EQ(u.wf_detail, 1);
    CHECK_EQ(u.scale, 1);
    CHECK(u.wf_smooth);             // the window moves the graphs with the clock
    s.view = 0;
    spectro_win_pack(s, c);
    const SpectroSettings p = spectro_win_unpack(c);
    CHECK_EQ(p.view, 0);
    CHECK(!p.wf_smooth);
    SpectroSettings w = u;
    SpectroAnalyzer& a = spectro();
    a.reset();
    feed_tone(a, w, 2.0);
    std::vector<uint8_t> img;
    a.waterfall(w, 300, 150, false, img);
    CHECK_EQ(img.size(), static_cast<size_t>(300) * 150);
    for (uint8_t v : img) CHECK((v & 3) == 0);   // high resolution: register * 4
    a.reset();
}
