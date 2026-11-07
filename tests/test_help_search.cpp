// The search inside the cheat sheet and the help wizard (SHIFT+s): src/help_search.h and wizard::search_key().
#include "test_framework.h"
#include "help_search.h"
#include "wizard.h"

using namespace muisc;

namespace {
helpsearch::Hit hit(const std::string& query, const std::string& text) {
    return helpsearch::match(helpsearch::split_words(query), text);
}
std::string marked(const std::string& text, const helpsearch::Hit& h) {
    std::string out;
    size_t pos = 0;
    for (const auto& r : h.ranges) {
        out += text.substr(pos, r.from - pos) + "[" + text.substr(r.from, r.to - r.from) + "]";
        pos = r.to;
    }
    return out + text.substr(pos);
}
}

TEST(help_search_finds_pieces_and_marks_them) {
    const std::string t = "Add hovering track to the end of the queue";
    const auto h = hit("queue end", t);
    CHECK(h.full());
    CHECK_EQ(marked(t, h), std::string("Add hovering track to the [end] of the [queue]"));
}

TEST(help_search_forgives_typos_and_knows_related_words) {
    CHECK(hit("quue", "Add to the queue").full());                  // one letter missing
    CHECK(hit("playlsit", "Open the playlist editor").full());       // two letters swapped
    CHECK(hit("remove", "Delete the hovering track").full());        // a related word
    CHECK(!hit("zebra", "Delete the hovering track").matched);
    CHECK(!hit("cat", "dog and cow").matched);                       // short words are not guessed
}

TEST(help_search_partial_hits_need_half_of_the_words) {
    const auto h = hit("queue zebra", "Add to the queue");
    CHECK(!h.full());
    CHECK_EQ(h.matched, 1);
    CHECK(helpsearch::worth_showing(h));
    CHECK(!helpsearch::worth_showing(hit("queue zebra giraffe", "Add to the queue")));
}

TEST(help_search_ranks_full_hits_first) {
    const auto full = hit("volume up", "Volume up");
    const auto part = hit("volume up", "Volume down");
    CHECK(helpsearch::better(full, part));
}

TEST(wizard_search_finds_chapters_and_opens_them) {
    wizard::State st;
    wizard::open(st, false);
    wizard::handle_key(st, wizard::Key::Search);
    CHECK(st.searching);
    for (char c : std::string("equalizer")) wizard::search_key(st, c, false, false);
    const std::vector<std::string> disk;
    const wizard::View v = wizard::render(st, 120, 26, disk);
    CHECK(v.search);
    CHECK(!st.results.empty());
    wizard::search_key(st, 27, false, false);          // ESC clears the query ...
    CHECK(st.searching);
    CHECK(st.query.empty());
    for (char c : std::string("equalizer")) wizard::search_key(st, c, false, false);
    wizard::render(st, 120, 26, disk);
    wizard::search_key(st, '\r', false, false);        // ... ENTER opens the chapter
    CHECK(!st.searching);
    wizard::search_key(st, 27, false, false);          // not searching: nothing happens
    wizard::render(st, 120, 26, disk);
}

namespace {
std::string wiz_text(const wizard::View& v) {
    std::string all;
    for (const auto& row : v.rows) { for (const auto& sp : row) all += sp.text; all += "\n"; }
    return all + v.title;
}
}

// Headings of the table of content ("2 MUSIC PLAYER MODE", "2.1 Main UI") cannot be selected: DOWN steps over them.
TEST(wizard_toc_headings_are_not_selectable) {
    wizard::State st;
    wizard::open(st, false);
    const std::vector<std::string> disk;
    wizard::handle_key(st, wizard::Key::Contents);
    wizard::render(st, 120, 40, disk);
    for (int i = 0; i < 4; ++i) wizard::handle_key(st, wizard::Key::Down);   // 1.0 -> 1.1 -> 1.2 -> 1.3 -> (headings 2, 2.1 skipped) 2.1.1
    wizard::handle_key(st, wizard::Key::Enter);
    const wizard::View v = wizard::render(st, 120, 26, disk);
    CHECK(v.title.find("2.1") != std::string::npos);
}

// The intro pages of the radio and audio book are chapters of their own (3.0, 4.0).
TEST(wizard_part_intro_is_a_chapter_of_its_own) {
    wizard::State st;
    wizard::open(st, false);
    const std::vector<std::string> disk;
    wizard::handle_key(st, wizard::Key::Contents);
    bool found = false;
    for (int i = 0; i < 120 && !found; ++i) {
        const wizard::View v = wizard::render(st, 120, 80, disk);
        for (const auto& row : v.rows) {
            std::string line;
            bool cursor = false;
            for (const auto& sp : row) { line += sp.text; if (sp.role == wizard::Role::Cursor) cursor = true; }
            if (cursor && line.find("Radio Player Introduction") != std::string::npos) found = true;
        }
        if (!found) wizard::handle_key(st, wizard::Key::Down);
    }
    CHECK(found);
    wizard::handle_key(st, wizard::Key::Enter);
    const wizard::View v = wizard::render(st, 120, 26, disk);
    CHECK(v.title.find("3.0") != std::string::npos);
}

// The headings of the menu grid are in the header colour as a whole ("[MUSIC PLAYER: Main][[Header]]"), not only the capitalised word.
TEST(wizard_menu_grid_heading_is_header_coloured) {
    wizard::State st;
    wizard::open(st, false);
    wizard::handle_key(st, wizard::Key::Search);
    for (char c : std::string("grid of all three players")) wizard::search_key(st, c, false, false);
    const std::vector<std::string> disk;
    wizard::render(st, 120, 100, disk);
    wizard::search_key(st, '\r', false, false);
    const wizard::View v = wizard::render(st, 120, 100, disk);
    bool found = false, all_header = true;
    for (const auto& row : v.rows) {
        std::string line;
        for (const auto& sp : row) line += sp.text;
        if (line.find("MUSIC PLAYER: Main") == std::string::npos) continue;
        found = true;
        for (const auto& sp : row)
            if (sp.text.find_first_not_of(' ') != std::string::npos && sp.role != wizard::Role::Header) all_header = false;
    }
    CHECK(found);
    CHECK(all_header);
}

// GETTING STARTED follows the greeting of the very first start only; later it is a chapter (1.1) of the table of content.
TEST(wizard_getting_started_only_on_the_first_start) {
    const std::vector<std::string> disk(15, std::string(30, 'x'));
    {
        wizard::State st;
        wizard::open(st, true);
        wizard::render(st, 120, 40, disk);
        wizard::handle_key(st, wizard::Key::Right);
        CHECK(wiz_text(wizard::render(st, 120, 40, disk)).find("GETTING STARTED") != std::string::npos);
        wizard::handle_key(st, wizard::Key::Right);
        CHECK(wiz_text(wizard::render(st, 120, 40, disk)).find("TABLE OF CONTENT") != std::string::npos);
    }
    {
        wizard::State st;
        wizard::open(st, false);
        wizard::render(st, 120, 40, disk);
        wizard::handle_key(st, wizard::Key::Right);
        const std::string t = wiz_text(wizard::render(st, 120, 40, disk));
        CHECK(t.find("GETTING STARTED") == std::string::npos);
        CHECK(t.find("TABLE OF CONTENT") != std::string::npos);
    }
}

// [Text][[Legend]] in the help wizard: the text takes the legend colour, the square brackets are not shown.
TEST(wizard_frame_label_is_legend_coloured_without_brackets) {
    wizard::State st;
    wizard::open(st, false);
    wizard::handle_key(st, wizard::Key::Search);
    for (char c : std::string("audiobook set the mode")) wizard::search_key(st, c, false, false);
    const std::vector<std::string> disk;
    wizard::render(st, 120, 400, disk);
    wizard::search_key(st, '\r', false, false);
    const wizard::View v = wizard::render(st, 120, 400, disk);
    bool found = false, bracket = false;
    for (const auto& row : v.rows) {
        std::string line;
        for (const auto& sp : row) line += sp.text;
        if (line.find("Start Mode:") == std::string::npos) continue;
        for (const auto& sp : row) {
            if ((sp.text == "Start Mode" || sp.text == "Start Mode:") && sp.role == wizard::Role::Legend) found = true;
            if (sp.text.find('[') != std::string::npos || sp.text.find(']') != std::string::npos) bracket = true;
        }
    }
    CHECK(found);
    CHECK(!bracket);
}
