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

// A chapter without a page of its own that opens a part (3.1 "The Radio Main UI", 4.1 "The Audio Book Player") leads to the part's intro
// page that stands right in front of its first page, not to 3.1.1 / 4.1.1 right away.
TEST(wizard_chapter_of_a_part_opens_the_part_page) {
    wizard::State st;
    wizard::open(st, false);
    const std::vector<std::string> disk;
    wizard::handle_key(st, wizard::Key::Contents);
    wizard::render(st, 120, 26, disk);
    for (int i = 0; i < 42; ++i) wizard::handle_key(st, wizard::Key::Down);   // entry 42 = "3.1 The Radio Main UI"
    wizard::handle_key(st, wizard::Key::Enter);
    const wizard::View v = wizard::render(st, 120, 26, disk);
    CHECK(v.title.find("3.1.1") == std::string::npos);
    CHECK(v.title.find("RADIO PLAYER MODE") != std::string::npos);
}

// [Text][[Legend]] in the help wizard: the text takes the legend colour, the square brackets are not shown.
TEST(wizard_frame_label_is_legend_coloured_without_brackets) {
    wizard::State st;
    wizard::open(st, false);
    wizard::handle_key(st, wizard::Key::Search);
    for (char c : std::string("Start Mode")) wizard::search_key(st, c, false, false);
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
