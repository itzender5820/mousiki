// Small helpers every frame goes through: where the cursor is parked, and the colour of a gradient step.
#include "test_framework.h"
#include "terminal_ui.h"
#include "radio_settings.h"

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
