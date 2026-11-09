// Malformed UTF-8 and wide / combining characters (src/utf8_util.h, the width helpers of src/terminal_ui.cpp).
// What went wrong before: text that was measured wider than it was drawn moved everything right of it, the border
// included. The checks below do not depend on the platform's width table (wcwidth / win_codepoint_width): they test
// that what is measured and what is returned agree.
#include "test_framework.h"
#include "terminal_ui.h"
#include "utf8_util.h"

using namespace muisc;

namespace {
const std::string kFffd = "\xEF\xBF\xBD";   // U+FFFD
// Japanese: "レモンドロップ" (precomposed) and the same with decomposed voiced marks (ト + U+3099 = ド, フ + U+309A = プ)
const std::string kKana = "\xE3\x83\xAC\xE3\x83\xA2\xE3\x83\xB3\xE3\x83\x89\xE3\x83\xAD\xE3\x83\x83\xE3\x83\x97";
const std::string kKanaNfd = "\xE3\x83\xAC\xE3\x83\xA2\xE3\x83\xB3\xE3\x83\x88\xE3\x82\x99\xE3\x83\xAD\xE3\x83\x83\xE3\x83\x95\xE3\x82\x9A";
const std::string kBracketed = "\xE3\x80\x8C" + kKana + "\xE3\x80\x8D";   // 「…」

// Strings that are valid, cut short, or damaged in different ways.
std::vector<std::string> samples() {
    return {
        "", "plain ascii", "caf\xC3\xA9", kKana, kKanaNfd, kBracketed, "a" + kKana + "b" + kKanaNfd + "c",
        "\xE3\x83",                       // a 3-byte character cut after 2 bytes
        "abc\xE3",                        // ... after 1 byte, at the end
        "\xE3\x83\x28\x29",               // lead byte followed by non-continuation bytes
        "\x80\x81\xBF",                   // stray continuation bytes
        "\xC0\xAF\xC1\x80",               // overlong forms
        "\xED\xA0\x80",                   // a UTF-16 surrogate encoded as UTF-8
        "\xF4\x90\x80\x80",               // above U+10FFFF
        "\xFF\xFE title \xF0\x9F",        // bytes that never occur + a cut 4-byte character
        kKana.substr(0, 4) + "x" + kKana.substr(2),   // a character broken in the middle, twice
    };
}
} // namespace

TEST(utf8_valid_and_repair_basics) {
    CHECK(utf8_is_valid(""));
    CHECK(utf8_is_valid("abc"));
    CHECK(utf8_is_valid(kKana));
    CHECK(utf8_is_valid("\xF0\x9F\x8E\xB5"));   // an emoji (4 bytes)
    CHECK(!utf8_is_valid("\xE3\x83"));
    CHECK(!utf8_is_valid("\x80"));
    CHECK(!utf8_is_valid("\xC0\xAF"));          // overlong "/"
    CHECK(!utf8_is_valid("\xED\xA0\x80"));      // surrogate
    CHECK(!utf8_is_valid("\xF4\x90\x80\x80"));  // > U+10FFFF
    // valid text is returned unchanged
    CHECK_EQ(utf8_repair(kKana), kKana);
    CHECK_EQ(utf8_repair("caf\xC3\xA9"), std::string("caf\xC3\xA9"));
    // every byte that is not part of a valid sequence becomes one U+FFFD
    CHECK_EQ(utf8_repair("a\x80" "b"), "a" + kFffd + "b");
    CHECK_EQ(utf8_repair("\xE3\x83"), kFffd + kFffd);
    CHECK_EQ(utf8_repair("\xC0\xAF"), kFffd + kFffd);
}

TEST(utf8_repair_always_gives_valid_utf8) {
    for (const std::string& s : samples()) {
        const std::string r = utf8_repair(s);
        CHECK(utf8_is_valid(r));
        if (utf8_is_valid(s)) CHECK_EQ(r, s);
    }
    // every single byte value, alone and after a lead byte
    for (int b = 0; b < 256; ++b) {
        const std::string one(1, static_cast<char>(b));
        CHECK(utf8_is_valid(utf8_repair(one)));
        CHECK(utf8_is_valid(utf8_repair("\xE3" + one)));
        CHECK(utf8_is_valid(utf8_repair("\xE3\x83" + one)));
    }
}

TEST(clean_tag_text_drops_controls_and_a_cut_tail) {
    CHECK_EQ(clean_tag_text("Title"), std::string("Title"));
    CHECK_EQ(clean_tag_text(kKana), kKana);
    CHECK_EQ(clean_tag_text("a\tb"), std::string("a b"));                  // tab / line breaks become one blank
    CHECK_EQ(clean_tag_text("a\r\n\r\nb"), std::string("a b"));
    CHECK_EQ(clean_tag_text("a\x1b[31mred"), std::string("a[31mred"));    // ESC is removed, it would start a sequence
    CHECK_EQ(clean_tag_text(std::string("a\0b", 3)), std::string("ab"));
    CHECK_EQ(clean_tag_text("a\xC2\x9B" "b"), std::string("ab"));          // U+009B (CSI) is a C1 control
    // a tag cut in the middle of its last character: the partial character is dropped, not shown as U+FFFD
    CHECK_EQ(clean_tag_text(kKana.substr(0, kKana.size() - 1)), kKana.substr(0, kKana.size() - 3));
    CHECK_EQ(clean_tag_text("abc\xE3"), std::string("abc"));
    // broken bytes elsewhere still become U+FFFD
    CHECK_EQ(clean_tag_text("a\x80" "b"), "a" + kFffd + "b");
    for (const std::string& s : samples()) CHECK(utf8_is_valid(clean_tag_text(s)));
}

TEST(width_helpers_agree_with_what_they_return) {
    // pad_right() and pad_left() always give exactly `width` columns, whatever the text holds
    for (const std::string& s : samples()) {
        for (int w : {1, 2, 3, 5, 8, 13, 30}) {
            CHECK_EQ(display_width(pad_right(s, w)), w);
            CHECK_EQ(display_width(pad_left(s, w)), w);
            // truncating and slicing never return more than asked, and always valid UTF-8
            const std::string t = truncate_str(s, w);
            CHECK(display_width(t) <= w);
            CHECK(utf8_is_valid(t));
            const std::string k = utf8_take(s, w);
            CHECK(display_width(k) <= w);
            CHECK(utf8_is_valid(k));
        }
    }
}

TEST(utf8_skip_take_window_has_the_asked_width) {
    // a marquee frame must keep its width while the text is long enough, wherever the window starts (a double-width
    // character cut by an edge is left out and its column made up with a blank)
    const std::string long_text = kBracketed + " " + kKanaNfd + " " + kKana + " - plain ascii";
    const int total = display_width(long_text);
    for (int skip = 0; skip + 12 < total; ++skip) {
        const std::string frame = utf8_skip_take(long_text, skip, 12);
        CHECK(utf8_is_valid(frame));
        CHECK_EQ(display_width(frame), 12);
    }
    // a window that starts behind the end is empty, never invalid
    CHECK(utf8_is_valid(utf8_skip_take(long_text, total + 5, 12)));
}

TEST(combining_marks_do_not_start_a_window) {
    // decomposed "ド" = "ト" + U+3099: a window that starts right behind the "ト" must not begin with the bare mark
    const std::string s = kKanaNfd + kKanaNfd;
    for (int skip = 0; skip < display_width(s); ++skip) {
        const std::string frame = utf8_skip_take(s, skip, 6);
        CHECK(frame.rfind("\xE3\x82\x99", 0) != 0);   // does not start with U+3099
        CHECK(frame.rfind("\xE3\x82\x9A", 0) != 0);   // ... or U+309A
    }
}

TEST(wrap_lines_chunks_long_cjk_words) {
    const std::string s = kBracketed + kBracketed + kBracketed;
    const auto lines = wrap_lines(s, 10, 6);
    CHECK(!lines.empty());
    CHECK(lines.size() <= 6);
    for (const std::string& l : lines) {
        CHECK(utf8_is_valid(l));
        CHECK(display_width(l) <= 10);
    }
}

TEST(truecolour_becomes_256_colours_for_old_terminals) {
    // macOS Terminal before macOS 26 shows 24-bit colours wrong: they are turned into the nearest of the 256 fixed ones
    CHECK_EQ(nearest_256(0, 0, 0), 16);
    CHECK_EQ(nearest_256(255, 255, 255), 231);
    CHECK_EQ(nearest_256(255, 0, 0), 196);
    CHECK_EQ(nearest_256(128, 128, 128), 244);                       // a grey from the grey ramp
    CHECK_EQ(to_256_colours("a\x1b[38;2;255;0;0mred\x1b[0m"), std::string("a\x1b[38;5;196mred\x1b[0m"));
    CHECK_EQ(to_256_colours("\x1b[1;38;2;0;0;0;48;2;255;255;255mX"), std::string("\x1b[1;38;5;16;48;5;231mX"));
    CHECK_EQ(to_256_colours("\x1b[38;5;2;1mY\x1b[2J"), std::string("\x1b[38;5;2;1mY\x1b[2J"));   // 256 colours and other sequences stay
    CHECK_EQ(to_256_colours("no colour ;2; here"), std::string("no colour ;2; here"));
}

TEST(emoji_safe_mode_keeps_plain_emoji_and_replaces_risky_clusters) {
    const std::string note = "\xF0\x9F\x8E\xB5";                         // U+1F3B5, one codepoint
    const std::string cat = "\xF0\x9F\x90\x88\xE2\x80\x8D\xE2\xAC\x9B";   // black cat: U+1F408 ZWJ U+2B1B
    const std::string flag = "\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA";          // flag Germany
    set_emoji_mode(1);
    CHECK_EQ(display_width("a" + note + "b"), 4);   // the note is kept (2 cells)
    CHECK_EQ(display_width("a" + cat + "b"), 3);    // one "?"
    CHECK_EQ(display_width("a" + flag + "b"), 3);
    set_emoji_mode(2);
    CHECK_EQ(display_width("a" + note + "b"), 3);
    set_emoji_mode(0);
    CHECK(display_width("a" + cat + "b") != 3);
    set_emoji_mode(2);
}

// The fast path of the emoji replacement (the line has nothing that could be an emoji: it is not copied) must see every BMP emoji of
// the table, a VS16 and a keycap, and let braille / box drawing / block characters through untouched.
TEST(emoji_fast_path_sees_every_emoji_start) {
    set_emoji_mode(2);
    // every BMP emoji of the table is replaced by one "?" (width 1)
    const unsigned cps[] = {0x231A, 0x23E9, 0x23F0, 0x23F3, 0x25FD, 0x2614, 0x2648, 0x267F, 0x2693, 0x26A1, 0x26AA, 0x26BD, 0x26C4, 0x26CE,
                            0x26D4, 0x26EA, 0x26F2, 0x26F5, 0x26FA, 0x26FD, 0x2705, 0x270A, 0x2728, 0x274C, 0x274E, 0x2753, 0x2757, 0x2795,
                            0x27B0, 0x27BF, 0x2B1B, 0x2B50, 0x2B55};
    for (unsigned cp : cps) {
        std::string s = "a";
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
        CHECK(truncate_str(s, 10) == "a?");
    }
    CHECK(truncate_str("x\xEF\xB8\x8F", 10) == "?");                    // a plain character + VS16
    CHECK(truncate_str("1\xEF\xB8\x8F\xE2\x83\xA3", 10) == "?");        // a keycap
    CHECK(truncate_str("\xF0\x9F\x8E\xB5", 10) == "?");                 // a 4-byte emoji
    const std::string line = "\xE2\xA1\x80\xE2\xA3\xBF\xE2\x94\x82\xE2\x94\x80\xE2\x96\x88\xE2\x96\x91";   // braille, box drawing, blocks
    CHECK(truncate_str(line, 10) == line);
}
