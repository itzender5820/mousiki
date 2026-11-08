// The lyric block with a translation (video views) and the 4-way "Use Lyrics" value.
#include "test_framework.h"
#include "app_internal.h"
#include "settings.h"

using namespace muisc;

namespace {
std::string plain(const std::vector<std::string>& rows) {
    std::string out;
    for (const std::string& r : rows) { out += strip_csi(r); out += '\n'; }
    return out;
}
std::vector<LyricLine> two_lines() {
    LyricLine a; a.start_time = 1.0; a.full_text = "Hello big world"; a.words = {{1.0, "Hello"}, {2.0, "big"}, {3.0, "world"}};
    LyricLine b; b.start_time = 5.0; b.full_text = "Next line";
    return {a, b};
}
}

TEST(lyric_mode_names_and_old_values) {
    CHECK_EQ(lyr_mode_parse("true"), 1);
    CHECK_EQ(lyr_mode_parse("false"), 0);
    CHECK_EQ(lyr_mode_parse("both"), 2);
    CHECK_EQ(lyr_mode_parse("Translation"), 3);
    CHECK_EQ(std::string(lyr_mode_name(2)), std::string("lyrics+transl."));
    CHECK_EQ(std::string(lyr_mode_name(3)), std::string("transl."));
    CHECK_EQ(std::string(lyr_mode_name(0)), std::string("none"));
}

TEST(translation_is_in_brackets_and_not_highlighted) {
    Settings s;
    s.lyrics_animation = 1;   // word by word: what has not been sung yet is blank in the lyric line
    const auto lines = two_lines();
    const std::vector<std::string> tr = {"Hallo grosse Welt", "Naechste Zeile"};
    const std::string early = plain(lyric_translation_rows(lines, &tr, 0, 1.0, 40, 3, s));
    CHECK(early.find("(Hallo grosse Welt)") != std::string::npos);   // all of it from the start: no reveal of its own
    CHECK(early.find("Hello") == std::string::npos);                 // translation only: the lyric line is not shown
    const std::string both = plain(lyric_translation_rows(lines, &tr, 0, 100.0, 40, 2, s));
    CHECK(both.find("Hello big world") != std::string::npos);
    CHECK(both.find("Hello") < both.find("(Hallo"));                 // the lyric line first, the translation under it
}

TEST(translation_modes_both_and_missing) {
    Settings s;
    const auto lines = two_lines();
    const std::vector<std::string> tr = {"Hallo grosse Welt", ""};
    const std::string none_for_line = plain(lyric_translation_rows(lines, &tr, 1, 100.0, 40, 3, s));
    CHECK(none_for_line.find("Next line") != std::string::npos);   // no translation for this line: the lyric line shows
    const std::string lyr = plain(lyric_translation_rows(lines, &tr, 0, 100.0, 40, 1, s));
    CHECK(lyr.find("Hallo") == std::string::npos);
}
