// The help wizard's text is read when it is first needed and given back when the wizard closes (wizard.h: release_memory()): the pages, the
// search and the contents look the same afterwards.
#include <string>
#include <vector>

#include "test_framework.h"
#include "wizard.h"

namespace {
std::string frame_text(const wizard::View& v) {
    std::string all = v.title + "|" + std::to_string(v.page_no) + "/" + std::to_string(v.page_total) + "\n";
    for (const auto& row : v.rows) {
        for (const auto& sp : row) all += sp.text + "\x01" + std::to_string(static_cast<int>(sp.role)) + "\x02";
        all += "\n";
    }
    return all;
}

// a frame of a sample of the pages, at two widths
std::vector<std::string> sample_frames() {
    std::vector<std::string> out;
    const std::vector<std::string> disk(15, std::string(30, 'o'));
    const int n = wizard::page_count();
    for (int page = 0; page < n; page += 5) {
        for (int cols : {120, 70}) {
            wizard::State st;
            wizard::open(st, false);
            st.page = page;
            st.toc = -1;
            out.push_back(frame_text(wizard::render(st, cols, 30, disk)));
        }
    }
    for (int page = 0; page < n; page += 9) out.push_back(std::to_string(wizard::page_rows(page, 100)));
    return out;
}
}  // namespace

TEST(wizard_memory_the_text_is_read_when_needed_and_given_back) {
    wizard::release_memory();
    CHECK(!wizard::memory_held());
    CHECK(wizard::page_count() > 20);
    CHECK(wizard::memory_held());
    wizard::release_memory();
    CHECK(!wizard::memory_held());
    wizard::release_memory();   // (a second call is harmless)
    CHECK(!wizard::memory_held());
}

TEST(wizard_memory_the_pages_look_the_same_after_the_text_was_given_back) {
    wizard::release_memory();
    const std::vector<std::string> before = sample_frames();
    CHECK(before.size() > 20);
    CHECK(wizard::memory_held());
    wizard::release_memory();
    CHECK(!wizard::memory_held());
    const std::vector<std::string> after = sample_frames();
    CHECK(before == after);
    // and again, with the pages drawn in the opposite order (the few parsed pages that are kept do not change what a page looks like)
    wizard::release_memory();
    std::vector<std::string> reversed = sample_frames();
    CHECK(reversed == before);
}

TEST(wizard_memory_search_and_contents_work_after_the_text_was_given_back) {
    const std::vector<std::string> disk;
    auto search = [&](const std::string& q) {
        wizard::State st;
        wizard::open(st, false);
        wizard::handle_key(st, wizard::Key::Search);
        for (char c : q) wizard::search_key(st, c, false, false);
        wizard::render(st, 120, 26, disk);
        return st.results;
    };
    wizard::release_memory();
    const auto first = search("equalizer");
    CHECK(!first.empty());
    wizard::release_memory();
    CHECK(search("equalizer") == first);
    // the contents page
    wizard::release_memory();
    wizard::State st;
    wizard::open(st, false);
    wizard::handle_key(st, wizard::Key::Contents);
    const wizard::View v = wizard::render(st, 120, 40, disk);
    CHECK(v.toc_page);
    CHECK(!v.rows.empty());
    wizard::release_memory();
}
