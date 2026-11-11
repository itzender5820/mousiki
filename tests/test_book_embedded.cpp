// The chapters a file carries itself (an .m4b audio book): ffprobe's JSON turned into a clean list.
#include "test_framework.h"
#include "book_embedded.h"
#include <cmath>

using namespace muisc;

namespace {
std::string one(const char* start, const char* end, const char* title) {
    std::string s = std::string("{\"id\":1,\"time_base\":\"1/1000\",\"start_time\":\"") + start + "\",\"end_time\":\"" + end + "\",\"tags\":{";
    if (title) s += std::string("\"title\":\"") + title + "\"";
    return s + "}}";
}
}

TEST(embedded_chapters_are_read_in_time_order_and_close_gaps) {
    const std::string j = "{\"chapters\":[" + one("600.000000", "1200.000000", "Two") + "," + one("0.000000", "590.000000", "One") + "," + one("1200.000000", "1500.5", "Three") + "]}";
    const auto c = parse_embedded_chapters(j, 1800.0);
    CHECK_EQ(c.size(), static_cast<size_t>(3));
    CHECK_EQ(c[0].title, std::string("One"));
    CHECK(std::fabs(c[0].end - 600.0) < 1e-6);        // the gap 590 .. 600 is closed: a chapter ends where the next begins
    CHECK(std::fabs(c[1].start - 600.0) < 1e-6 && std::fabs(c[1].end - 1200.0) < 1e-6);
    CHECK(std::fabs(c[2].end - 1800.0) < 1e-6);        // the last one runs to the end of the file
}

TEST(embedded_chapters_name_untitled_ones_and_ignore_a_single_chapter) {
    const std::string j = "{\"chapters\":[" + one("0", "100", nullptr) + "," + one("100", "200", "") + "]}";
    const auto c = parse_embedded_chapters(j, 200.0);
    CHECK_EQ(c.size(), static_cast<size_t>(2));
    CHECK_EQ(c[0].title, std::string("Chapter 1"));
    CHECK_EQ(c[1].title, std::string("Chapter 2"));
    CHECK(parse_embedded_chapters("{\"chapters\":[" + one("0", "100", "Only") + "]}", 100.0).empty());   // one chapter is just the file
    CHECK(parse_embedded_chapters("{\"chapters\":[]}", 100.0).empty());
    CHECK(parse_embedded_chapters("not json", 100.0).empty());
    CHECK(parse_embedded_chapters("{}", 100.0).empty());
}

TEST(embedded_chapters_too_short_ones_are_absorbed_and_title_case_does_not_matter) {
    std::string j = "{\"chapters\":[" + one("0", "50", "A") + "," + one("50", "50.4", "blip") + "," + one("50.4", "120", "B") + "]}";
    auto c = parse_embedded_chapters(j, 120.0);
    CHECK_EQ(c.size(), static_cast<size_t>(2));         // the 0.4 s one is gone, A takes its time
    CHECK(std::fabs(c[0].end - 50.4) < 1e-6);
    CHECK_EQ(c[1].title, std::string("B"));
    j = "{\"chapters\":[{\"start_time\":\"0\",\"end_time\":\"10\",\"tags\":{\"TITLE\":\"Upper\"}},{\"start_time\":\"10\",\"end_time\":\"20\",\"tags\":{\"title\":\"  Lower  \"}}]}";
    c = parse_embedded_chapters(j, 20.0);
    CHECK_EQ(c.size(), static_cast<size_t>(2));
    CHECK_EQ(c[0].title, std::string("Upper"));
    CHECK_EQ(c[1].title, std::string("Lower"));         // (trimmed)
}

TEST(embedded_chapters_are_only_asked_for_where_they_are_likely) {
    CHECK(embedded_chapters_worth_asking("/b/book.m4b", 40));    // always for an .m4b
    CHECK(embedded_chapters_worth_asking("/b/book.MKA", 40));
    CHECK(!embedded_chapters_worth_asking("/b/01.mp3", 40));     // a folder of many files is not probed one by one
    CHECK(embedded_chapters_worth_asking("/b/book.mp3", 1));     // a book of one file: it may carry its chapters
}
