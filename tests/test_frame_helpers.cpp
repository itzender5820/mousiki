// Small helpers every frame goes through: where the cursor is parked, and the colour of a gradient step.
#include "test_framework.h"
#include "terminal_ui.h"
#include "radio_settings.h"
#include "string_util.h"
#include "cover_colour.h"
#include <climits>
#include <cstdlib>

using namespace muisc;

// The first version: two rfind() calls over the whole frame.
static std::string park_cursor_rfind(const std::string& frame) {
    const size_t show = frame.rfind("\x1b[?25h"), hide = frame.rfind("\x1b[?25l");
    if (show != std::string::npos && (hide == std::string::npos || show > hide)) return frame;
    return frame + "\x1b[H";
}

TEST(park_cursor_decides_by_the_last_cursor_escape) {
    const std::string h = "\x1b[?25h", l = "\x1b[?25l";
    const std::vector<std::string> frames = {
        "", "x", "\x1b[?25", "\x1b[?25h", "\x1b[?25l", "text", "a" + h, h + "a", l + "a", "a" + l, "a" + h + "b" + l + "c", "a" + l + "b" + h + "c",
        h + l, l + h, h + h, l + l, "\x1b[1;1H\x1b[38;2;1;2;3mabc" + l + "\x1b[0m" + h + "\x1b[5;5H", "\x1b[?25x" + h, "\x1b[?26h", "\x1b[?25",
        std::string(50000, 'a') + h, h + std::string(50000, 'a'), std::string(100, '\x1b') + l};
    for (const std::string& f : frames) CHECK_EQ(park_cursor(f), park_cursor_rfind(f));
    CHECK_EQ(park_cursor("abc"), std::string("abc\x1b[H"));            // no caret escape: parked
    CHECK_EQ(park_cursor("abc\x1b[?25h"), std::string("abc\x1b[?25h"));  // the last one is "show": left as it is
}

TEST(a_gradient_step_is_the_same_whichever_pair_was_asked_before) {
    using namespace muisc::radio;
    const std::string red = "196", blue = "21";   // palette numbers: 196 = red (255,0,0), 21 = blue (0,0,255)
    const std::string a1 = gradient_fg(red, blue, 0.5f);
    const std::string b1 = gradient_fg(blue, red, 0.25f);
    const std::string a2 = gradient_fg(red, blue, 0.5f);   // the pair asked for again (and again after another pair)
    CHECK_EQ(a1, a2);
    CHECK(a1 != b1);
    CHECK_EQ(a1, std::string("\x1b[38;2;127;0;127m"));
    CHECK_EQ(b1, std::string("\x1b[38;2;63;0;191m"));
    CHECK_EQ(gradient_fg(red, "", 0.7f), ansi_fg(red));              // no end colour: just the first colour
    CHECK_EQ(gradient_fg(red, blue, 0.0f), std::string("\x1b[38;2;255;0;0m"));
    CHECK_EQ(gradient_fg(red, blue, 1.0f), std::string("\x1b[38;2;0;0;255m"));
}

TEST(gradient_steps_are_written_like_to_string_would_write_them) {
    using namespace muisc::radio;
    const std::vector<std::pair<std::string, std::string>> pairs = {{"196", "21"}, {"16", "231"}, {"46", "201"}, {"9", "255"}, {"232", "255"}, {"1", "2"}, {"226", "17"}};
    int n = 0;
    for (const auto& pr : pairs) {
        for (int i = 0; i <= 200; ++i) {
            const float t = static_cast<float>(i) / 200.0f;
            const std::string got = gradient_fg(pr.first, pr.second, t);
            int r = 0, g = 0, b = 0, r2 = 0, g2 = 0, b2 = 0;
            CHECK(color_rgb(pr.first, r, g, b) && color_rgb(pr.second, r2, g2, b2));
            const std::string want = "\x1b[38;2;" + std::to_string(static_cast<int>(r + (r2 - r) * t)) + ";" + std::to_string(static_cast<int>(g + (g2 - g) * t)) + ";"
                                   + std::to_string(static_cast<int>(b + (b2 - b) * t)) + "m";
            if (got != want) CHECK_EQ(got, want);
            ++n;
        }
    }
    CHECK(n > 1000);
}

TEST(atoi_safe_reads_like_atoi_and_never_overflows) {
    // The same answer as std::atoi wherever std::atoi is defined (the number fits an int).
    const std::vector<std::string> same = {"", "0", "7", "-7", "+7", "  42", "\t\n12", "12abc", "abc", "-", "+", "- 5", "3.9", "007", "-0",
                                           "2147483647", "-2147483647", "-2147483648", "48000KHz", "1,5", "--1", "0x10", "٣"};
    for (const std::string& v : same) CHECK_EQ(atoi_safe(v), std::atoi(v.c_str()));
    // Out of range: stops at the limits (std::atoi is undefined there).
    CHECK_EQ(atoi_safe("2147483648"), INT_MAX);
    CHECK_EQ(atoi_safe("99999999999999999999999999"), INT_MAX);
    CHECK_EQ(atoi_safe("-2147483649"), INT_MIN);
    CHECK_EQ(atoi_safe("-99999999999999999999999999"), INT_MIN);
    CHECK_EQ(atoi_safe(static_cast<const char*>(nullptr)), 0);
}

TEST(parse_int_and_parse_double_follow_stoi_and_stod_without_throwing) {
    const std::vector<std::string> texts = {"", "0", "12", "-12", "+3", " 7", "12abc", "abc", "-", "1.5", ".5", "1e3", "1,5", "0x10", "2147483647", "2147483648",
                                            "-2147483648", "-2147483649", "99999999999999999999", "1e999", "nan", "inf", "-0.25", "  3.5xyz"};
    for (const std::string& t : texts) {
        int want_i = 0; bool ok_i = true;
        try { want_i = std::stoi(t); } catch (...) { ok_i = false; }
        const auto got_i = parse_int(t);
        CHECK_EQ(got_i.has_value(), ok_i);
        if (ok_i && got_i) CHECK_EQ(*got_i, want_i);
        double want_d = 0; bool ok_d = true;
        try { want_d = std::stod(t); } catch (...) { ok_d = false; }
        const auto got_d = parse_double(t);
        CHECK_EQ(got_d.has_value(), ok_d);
        if (ok_d && got_d) CHECK(*got_d == want_d || (*got_d != *got_d && want_d != want_d));
    }
}

TEST(the_cover_gradient_keeps_its_colour_between_the_two_ends) {
    // the default disk colours: xterm 170 (215, 95, 215) to xterm 30 (0, 135, 135)
    const auto sat = [](const std::array<uint8_t, 3>& c) { const int mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]}); return mx ? double(mx - mn) / mx : 0.0; };
    const auto at = [](double u) { return gradient_step(215, 95, 215, 0, 135, 135, u); };
    CHECK(at(0.0) == (std::array<uint8_t, 3>{215, 95, 215}));   // the ends are exactly the chosen colours
    CHECK(at(1.0) == (std::array<uint8_t, 3>{0, 135, 135}));
    double worst_plain = 1.0, worst_now = 1.0;
    for (int i = 0; i <= 100; ++i) {
        const double u = i / 100.0;
        const std::array<uint8_t, 3> plain = {static_cast<uint8_t>(std::lround(215 + (0 - 215) * u)), static_cast<uint8_t>(std::lround(95 + (135 - 95) * u)), static_cast<uint8_t>(std::lround(215 + (135 - 215) * u))};
        const auto now = at(u);
        worst_plain = std::min(worst_plain, sat(plain)); worst_now = std::min(worst_now, sat(now));
        const auto luma = [](const std::array<uint8_t, 3>& c) { return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]; };
        CHECK(luma(now) > 0.75 * luma(plain));   // as bright to the eye as the plain blend (never much darker)
    }
    CHECK(worst_plain < 0.45);        // the plain blend goes dull ...
    CHECK(worst_now > 0.5);           // ... this one keeps more of the colour (the ends' saturations are 0.56 and 1.0)
    // without a colour in it nothing changes: grey to grey, and a solid colour
    CHECK(gradient_step(200, 200, 200, 50, 50, 50, 0.5) == (std::array<uint8_t, 3>{125, 125, 125}));
    CHECK(gradient_step(10, 200, 30, 10, 200, 30, 0.37) == (std::array<uint8_t, 3>{10, 200, 30}));
}

TEST(the_cover_gradient_strength_runs_from_the_plain_blend_to_the_full_saturation) {
    const auto plain = [](double u) { return std::array<uint8_t, 3>{static_cast<uint8_t>(std::lround(215 + (0 - 215) * u)), static_cast<uint8_t>(std::lround(95 + (135 - 95) * u)), static_cast<uint8_t>(std::lround(215 + (135 - 215) * u))}; };
    const auto sat = [](const std::array<uint8_t, 3>& c) { const int mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]}); return mx ? double(mx - mn) / mx : 0.0; };
    for (int i = 0; i <= 20; ++i) CHECK(gradient_step(215, 95, 215, 0, 135, 135, i / 20.0, 0.0) == plain(i / 20.0));   // strength 0: exactly the old blend
    double last = 0;
    for (double k : {0.0, 0.2, 0.4, 0.6, 0.8, 1.0}) {   // more strength, more saturation halfway (never less)
        const double s = sat(gradient_step(215, 95, 215, 0, 135, 135, 0.5, k));
        CHECK(s + 1e-9 >= last);
        last = s;
    }
    CHECK(gradient_step(215, 95, 215, 0, 135, 135, 0.5) == gradient_step(215, 95, 215, 0, 135, 135, 0.5, 0.6));   // the default is 0.6
    CHECK(gradient_step(215, 95, 215, 0, 135, 135, 0.0, 1.0) == (std::array<uint8_t, 3>{215, 95, 215}));       // the ends stay the chosen colours at every strength
    CHECK(gradient_step(215, 95, 215, 0, 135, 135, 1.0, 1.0) == (std::array<uint8_t, 3>{0, 135, 135}));
}
