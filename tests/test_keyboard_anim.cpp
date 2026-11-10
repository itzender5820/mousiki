// The keyboard of the "<" menus overlay: which keys a combination presses on a German (QWERTZ) and an English (QWERTY) keyboard,
// the rows are 60 columns wide, and the animation presses the modifiers before the key.
#include "test_framework.h"
#include "keyboard_anim.h"

using namespace muisc;

namespace {
std::string key_text(const kbd::Combo& c, bool z) {
    kbd::RowRef rows[kbd::kRows];
    kbd::rows_of(z, rows);
    return c.row < 0 ? std::string() : std::string(rows[c.row].k[c.col].label);
}
}  // namespace

TEST(keyboard_plain_and_shifted_letters) {
    for (bool z : {true, false}) {
        auto g = kbd::parse_combo("G", z);          // an upper case letter on its own = SHIFT + g
        CHECK(g.ok && g.shift && !g.ctrl && !g.alt && key_text(g, z) == "G");
        auto s = kbd::parse_combo("s", z);
        CHECK(s.ok && !s.shift && key_text(s, z) == "S");
        auto a = kbd::parse_combo("ALT+L", z);      // with ALT the letter is just the letter
        CHECK(a.ok && a.alt && !a.shift && key_text(a, z) == "L");
        auto c = kbd::parse_combo("CTRL+SHIFT+U", z);
        CHECK(c.ok && c.ctrl && c.shift && key_text(c, z) == "U");
    }
}

TEST(keyboard_layout_changes_the_keys) {
    auto lt_de = kbd::parse_combo("<", true);       // its own key on a German keyboard
    CHECK(lt_de.ok && !lt_de.shift && key_text(lt_de, true) == "<");
    auto lt_us = kbd::parse_combo("<", false);      // SHIFT+, on an English one
    CHECK(lt_us.ok && lt_us.shift && key_text(lt_us, false) == ",");
    auto star_de = kbd::parse_combo("*", true);     // SHIFT and +
    CHECK(star_de.ok && star_de.shift && key_text(star_de, true) == "+");
    auto star_us = kbd::parse_combo("*", false);    // SHIFT+8
    CHECK(star_us.ok && star_us.shift && key_text(star_us, false) == "8");
    auto brace = kbd::parse_combo("{", true);       // AltGr+7 on a German keyboard
    CHECK(brace.ok && brace.altgr && !brace.shift && key_text(brace, true) == "7");
    auto deg = kbd::parse_combo("\xc2\xb0", true);  // SHIFT+^
    CHECK(deg.ok && deg.shift && key_text(deg, true) == "^");
    auto y_de = kbd::parse_combo("y", true), y_us = kbd::parse_combo("y", false);   // Y and Z swap
    CHECK(y_de.row == 3 && y_us.row == 1);
}

TEST(keyboard_names_and_keys_that_are_not_drawn) {
    auto sp = kbd::parse_combo("SPACE", true);
    CHECK(sp.ok && key_text(sp, true) == "Space");
    auto en = kbd::parse_combo("SHIFT+ENTER", false);
    CHECK(en.ok && en.shift && key_text(en, false) == "Enter");
    auto esc = kbd::parse_combo("CTRL+ESC", true);   // not on the keyboard shown: only the modifier is lit
    CHECK(esc.ok && esc.ctrl && esc.row < 0);
    CHECK(!kbd::parse_combo("", true).ok);
    CHECK(!kbd::parse_combo("-", true).ok);
    CHECK(!kbd::parse_combo("ESC", false).ok);
}

TEST(keyboard_rows_are_60_columns_wide) {
    for (bool z : {true, false}) {
        auto p = kbd::plan(kbd::parse_combo("G", z), z, 1.0);
        CHECK_EQ(static_cast<int>(p.size()), kbd::kRows);
        for (const auto& row : p) {
            int end = 0;
            for (const auto& c : row) {
                int cols = 0;
                for (unsigned char ch : c.text) if ((ch & 0xC0) != 0x80) ++cols;
                CHECK_EQ(cols, c.w);
                CHECK(c.x >= end);
                end = c.x + c.w;
            }
            CHECK(end <= kbd::kWidth);
        }
    }
}

TEST(keyboard_animation_presses_modifiers_before_the_key) {
    const auto c = kbd::parse_combo("CTRL+SHIFT+U", true);
    auto count = [&](double t, int state) {
        int n = 0;
        for (const auto& row : kbd::plan(c, true, t)) for (const auto& k : row) if (k.state == state) ++n;
        return n;
    };
    CHECK_EQ(count(0.1, 1) + count(0.1, 2), 0);   // at rest
    CHECK_EQ(count(0.6, 1), 2);                   // CTRL and SHIFT are down ...
    CHECK_EQ(count(0.6, 2), 0);                   // ... before the key
    CHECK_EQ(count(1.2, 1), 2);
    CHECK_EQ(count(1.2, 2), 1);                   // then the key
    CHECK_EQ(count(2.0, 1) + count(2.0, 2), 0);   // and all let go; the next round starts again
    CHECK_EQ(count(0.6 + kbd::kCycle, 1), 2);
    // a key without modifiers goes down at once; the shift of a key on the right half is the left one
    const auto s = kbd::parse_combo("s", false);
    int down = 0;
    for (const auto& row : kbd::plan(s, false, 0.6)) for (const auto& k : row) if (k.state == 2) ++down;
    CHECK_EQ(down, 1);
    const auto p = kbd::parse_combo("P", false);   // SHIFT+p: p is on the right, the LEFT shift goes down
    const auto cells = kbd::plan(p, false, 0.6);
    CHECK(cells[3].front().state == 1 && cells[3].back().state == 0);
}
