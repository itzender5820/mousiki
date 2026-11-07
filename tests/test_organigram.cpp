// The "<" menu organigram: the trees, the steering and the drawing.
#include "test_framework.h"
#include "organigram_trees.h"

#include <set>

using namespace muisc;

namespace {
int text_width(const std::string& s) {   // visible columns of a line with escape sequences
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b') { while (i < s.size() && s[i] != 'm') ++i; ++i; continue; }
        const unsigned char ch = static_cast<unsigned char>(s[i]);
        i += ch < 0x80 ? 1 : ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : 2;
        ++w;
    }
    return w;
}
std::string plain(const std::vector<std::string>& rows) {
    std::string o;
    for (const auto& r : rows) {
        for (size_t i = 0; i < r.size();) {
            if (r[i] == '\x1b') { while (i < r.size() && r[i] != 'm') ++i; ++i; continue; }
            o += r[i++];
        }
        o += '\n';
    }
    return o;
}
org::Theme theme() {
    org::Theme t;
    t.key_text = [](const org::Node& n) { return std::string(n.fallback ? n.fallback : ""); };
    return t;
}
}  // namespace

TEST(organigram_trees_are_well_formed) {
    for (const org::Tree& t : {org::player_tree(), org::book_tree(), org::radio_tree()}) {
        CHECK_EQ(t.n[0].depth, 0);
        for (int i = 1; i < t.count; ++i) {
            CHECK(t.n[i].depth >= 1);
            CHECK(t.n[i].depth <= t.n[i - 1].depth + 1);
            CHECK(org::parent_of(t, i) >= 0);
            if (!t.n[i].action) CHECK(!org::children_of(t, i).empty());   // a group has children
        }
    }
}

TEST(organigram_player_tree_has_the_asked_structure) {
    const org::Tree t = org::player_tree();
    const auto cats = org::children_of(t, 0);
    CHECK_EQ(cats.size(), static_cast<size_t>(7));
    const char* want[] = {"System", "Sound", "Library", "Other Lists", "Meta and Multi Media Data", "Visualizations", "Lyrics"};
    for (size_t i = 0; i < cats.size(); ++i) CHECK_EQ(std::string(t.n[cats[i]].title), std::string(want[i]));
    CHECK_EQ(org::children_of(t, cats[0]).size(), static_cast<size_t>(4));
    CHECK_EQ(org::children_of(t, cats[1]).size(), static_cast<size_t>(4));
    CHECK_EQ(org::children_of(t, cats[2]).size(), static_cast<size_t>(3));   // the three big overlays
    CHECK_EQ(org::children_of(t, cats[3]).size(), static_cast<size_t>(2));   // playlists, listening history
    CHECK_EQ(org::children_of(t, cats[5]).size(), static_cast<size_t>(5));   // Spectrum Bars, Spectrogram, Oscilloscope, Video, Creator
    CHECK_EQ(org::children_of(t, cats[6]).size(), static_cast<size_t>(4));
    // Meta Data Menu has two branches: fetch / convert and the looks
    const auto meta = org::children_of(t, cats[4]);
    CHECK_EQ(meta.size(), static_cast<size_t>(1));
    const auto branches = org::children_of(t, meta[0]);
    CHECK_EQ(branches.size(), static_cast<size_t>(2));
    CHECK_EQ(org::children_of(t, branches[0]).size(), static_cast<size_t>(3));
    CHECK_EQ(org::children_of(t, branches[1]).size(), static_cast<size_t>(1));   // the style options (the video overlays are under Visualizations)
    // every action is used once
    std::set<std::string> seen;
    for (int i = 0; i < t.count; ++i) if (t.n[i].action) CHECK(seen.insert(t.n[i].action).second);
}

TEST(organigram_steering_reaches_every_node) {
    for (const org::Tree& t : {org::player_tree(), org::book_tree(), org::radio_tree()}) {
        std::set<int> seen;
        std::vector<int> todo{1};
        while (!todo.empty()) {   // from the first category: right round the siblings, down into the children
            const int at = todo.back();
            todo.pop_back();
            if (!seen.insert(at).second) continue;
            todo.push_back(org::move(t, at, 3));
            todo.push_back(org::move(t, at, 2));
            todo.push_back(org::move(t, at, 1));
        }
        CHECK_EQ(static_cast<int>(seen.size()), t.count - 1);   // all but Main
        // up goes back to the parent, and never onto Main
        for (int i = 1; i < t.count; ++i) {
            const int p = org::parent_of(t, i);
            CHECK_EQ(org::move(t, i, 0), p > 0 ? p : i);
        }
    }
}

TEST(organigram_draws_exact_rows_at_many_sizes) {
    const org::Theme th = theme();
    for (const org::Tree& t : {org::player_tree(), org::book_tree(), org::radio_tree()})
        for (int sel = 1; sel < t.count; ++sel)
            for (int w : {40, 80, 120, 200})
                for (int h : {12, 24, 40}) {
                    const auto rows = org::render(t, sel, w, h, 1.5, th, "info");
                    CHECK_EQ(static_cast<int>(rows.size()), h);
                    for (const auto& r : rows) CHECK_EQ(text_width(r), w);
                }
}

TEST(organigram_shows_the_selection_its_path_and_its_children) {
    const org::Theme th = theme();
    const org::Tree t = org::player_tree();
    int fetch = -1;
    for (int i = 0; i < t.count; ++i) if (std::string(t.n[i].title) == "Fetch Cover") fetch = i;
    CHECK(fetch > 0);
    const std::string p = plain(org::render(t, fetch, 140, 40, 0.0, th, "Fetch Cover"));
    CHECK(p.find("Main") != std::string::npos);
    CHECK(p.find("Meta & Multimedia") != std::string::npos);
    CHECK(p.find("Meta Data Menu") != std::string::npos);
    CHECK(p.find("Fetch and Convert") != std::string::npos);
    CHECK(p.find("Cover and Video Look") != std::string::npos);
    CHECK(p.find("Fetch Cover") != std::string::npos);
    CHECK(p.find("File Conversion") != std::string::npos);
    CHECK(p.find("Style Options") == std::string::npos);   // the other branch is closed
    // a category shows its entries right below
    const std::string q = plain(org::render(t, 1, 140, 40, 0.0, th, "System"));
    CHECK(q.find("Cheat Sheet") != std::string::npos);
    CHECK(q.find("Equalizer") == std::string::npos);
}

TEST(organigram_book_tree_has_the_asked_structure) {
    const org::Tree t = org::book_tree();
    const auto cats = org::children_of(t, 0);
    const char* want[] = {"System", "Library and Meta Data", "Bookmarks and Notes", "Sound", "Visualizations"};
    CHECK_EQ(cats.size(), static_cast<size_t>(5));
    for (size_t i = 0; i < cats.size(); ++i) CHECK_EQ(std::string(t.n[cats[i]].title), std::string(want[i]));
    auto titles = [&](int cat) { std::string o; for (int k : org::children_of(t, cat)) o += std::string(t.n[k].title) + "|"; return o; };
    CHECK_EQ(titles(cats[0]), std::string("Settings|Console / logs / status|Help Wizard|Cheat Sheet|File Conversion|"));
    CHECK_EQ(titles(cats[1]), std::string("Library Overlay|Tag Editor|Reading History|Chapter Marks|"));
    CHECK_EQ(titles(cats[2]), std::string("Bookmark List|New Note|Set Bookmark|Notes Overlay|"));
}

TEST(organigram_video_sits_under_visualizations) {
    const org::Tree t = org::player_tree();
    int vis = -1, video = -1;
    for (int i = 0; i < t.count; ++i) { if (std::string(t.n[i].title) == "Visualizations") vis = i; if (std::string(t.n[i].title) == "Video") video = i; }
    CHECK(vis > 0 && video > 0);
    CHECK_EQ(org::parent_of(t, video), vis);
    const auto kids = org::children_of(t, video);
    CHECK_EQ(kids.size(), static_cast<size_t>(2));
    CHECK_EQ(std::string(t.n[kids[0]].action), std::string("HKeyVideo"));
    CHECK_EQ(std::string(t.n[kids[1]].action), std::string("HKeyVideoFull"));
    for (int i = 0; i < t.count; ++i) CHECK(std::string(t.n[i].title) != "Cover and Video Look" || org::children_of(t, i).size() == 1);   // only the style options stay
}

TEST(organigram_grows_a_wide_row_into_columns_instead_of_scrolling) {
    const org::Theme th = theme();
    const org::Tree t = org::book_tree();
    int cat = -1;
    for (int i = 0; i < t.count; ++i) if (std::string(t.n[i].title) == "Bookmarks and Notes") cat = i;
    for (int w : {30, 44, 60}) {   // too narrow for one row: the entries stand in columns, all visible, no scroll arrows
        const std::string p = plain(org::render(t, cat, w, 30, 0.0, th, "x"));
        for (const char* e : {"Bookmark List", "New Note", "Set Bookmark", "Notes Overlay"}) CHECK(p.find(e) != std::string::npos);
        CHECK(p.find("\xe2\x97\x80") == std::string::npos && p.find("\xe2\x96\xb6") == std::string::npos);
    }
    // the player's categories stay blocks in a row at a width of 100
    const org::Tree pt = org::player_tree();
    const std::string q = plain(org::render(pt, 1, 100, 30, 0.0, th, "System"));
    CHECK(q.find("Other Lists") != std::string::npos && q.find("Library") != std::string::npos && q.find("\xe2\x97\x80") == std::string::npos && q.find("\xe2\x96\xb6") == std::string::npos && q.find("Lyrics") != std::string::npos);
    // narrower: the seven categories hang in columns, every one still shown
    const std::string n = plain(org::render(pt, 1, 56, 30, 0.0, th, "System"));
    for (const char* e : {"System", "Sound", "Library", "Other Lists", "Meta & Multimedia", "Visualizations", "Lyrics"}) CHECK(n.find(e) != std::string::npos);
}

TEST(organigram_stays_on_its_row_when_it_grows_downwards) {
    const org::Theme th = theme();
    const org::Tree t = org::player_tree();
    auto row_of_main = [&](int sel, int w) {
        const auto rows = org::render(t, sel, w, 36, 0.0, th, "x");
        for (size_t i = 0; i < rows.size(); ++i) if (plain({rows[i]}).find("Main") != std::string::npos && plain({rows[i]}).find("\xe2\x80\xba") == std::string::npos) return static_cast<int>(i);
        return -1;
    };
    for (int w : {60, 100, 140}) {
        const int base = row_of_main(1, w);
        CHECK(base >= 0);
        for (int sel = 1; sel < t.count; ++sel) CHECK_EQ(row_of_main(sel, w), base);
    }
}

TEST(organigram_classic_view_is_a_panel_of_its_own_size) {
    const org::Theme th = theme();
    for (const org::Tree& t : {org::player_tree(), org::book_tree(), org::radio_tree()})
        for (int maxw : {40, 70, 120, 200})
            for (int maxr : {10, 24, 60}) {
                const org::ClassicPanel p = org::render_classic(t, 3, th, maxr, maxw);
                CHECK(p.inner_w <= std::max(24, maxw));
                CHECK(static_cast<int>(p.rows.size()) <= maxr);
                for (const auto& r : p.rows) CHECK_EQ(text_width(r), p.inner_w);
            }
    const org::Tree t = org::player_tree();
    const org::ClassicPanel big = org::render_classic(t, 3, th, 80, 200), small = org::render_classic(t, 3, th, 80, 70);
    CHECK(big.inner_w < 130);   // as wide as its columns need, not the window
    CHECK(small.inner_w <= 70);
    const std::string p = plain(big.rows);
    for (const char* e : {"SYSTEM", "Equalizer", "Playlists", "Fetch Cover", "Style Options", "Spectrum Bars", "Video", "Karaoke Mode", "[ALT+L]"}) CHECK(p.find(e) != std::string::npos);
    // the headers cannot be chosen: a category becomes its first entry, up / down skip them
    CHECK_EQ(org::classic_sel(t, 1), 2);
    for (int i = 1; i < t.count; ++i) {
        for (int dir : {0, 1, 2, 3}) CHECK(t.n[org::move_classic(t, i, dir)].depth >= 2);
    }
    std::set<int> seen;
    int at = org::classic_sel(t, 1);
    int entries = 0;
    for (int i = 1; i < t.count; ++i) if (t.n[i].depth >= 2) ++entries;
    for (int i = 0; i < entries; ++i, at = org::move_classic(t, at, 1)) seen.insert(at);
    CHECK_EQ(static_cast<int>(seen.size()), entries);
    // left / right change the column
    const int right = org::move_classic(t, 2, 3);
    CHECK(right > 2);
    // the Lyrics category stands in the left column of the player
    int lyr = -1;
    for (int i = 0; i < t.count; ++i) if (std::string(t.n[i].title) == "Refetch Lyrics") lyr = i;
    const auto cols = org::detail::classic_columns(t);
    bool left = false;
    for (const auto& c : cols[0]) if (c.node == lyr) left = true;
    CHECK(left);
}

TEST(organigram_keeps_the_keys_when_a_row_is_too_long) {
    org::Theme th;
    th.key_text = [](const org::Node& n) { return std::string(n.action && n.action[0] ? "SHIFT+p" : ""); };
    const org::Tree t = org::radio_tree();
    int cat = -1;
    for (int i = 0; i < t.count; ++i) if (std::string(t.n[i].title) == "Stations and Presets") cat = i;
    for (int w : {100, 120, 140, 70}) {   // a zigzag of two rows, or columns when narrower: the keys stay in every box
        const std::string p = plain(org::render(t, cat, w, 30, 0.0, th, "x"));
        size_t keys = 0;
        for (size_t at = p.find("[SHIFT+p]"); at != std::string::npos; at = p.find("[SHIFT+p]", at + 1)) ++keys;
        CHECK_EQ(keys, static_cast<size_t>(5));
        for (const char* e : {"Presets Menu", "Station Lists", "Radio Browser", "Big Stations Overlay", "Listening History"}) CHECK(p.find(e) != std::string::npos);
    }
}

TEST(organigram_visualizations_are_grouped) {
    const org::Tree t = org::player_tree();
    int vis = -1;
    for (int i = 0; i < t.count; ++i) if (std::string(t.n[i].title) == "Visualizations") vis = i;
    const auto kids = org::children_of(t, vis);
    CHECK_EQ(kids.size(), static_cast<size_t>(5));   // bars, Spectrogram, Oscilloscope, Video, Creator
    int groups = 0;
    for (int k : kids) if (!t.n[k].action) { ++groups; CHECK(org::children_of(t, k).size() >= 2); }   // options, overlay (, extra window)
    CHECK_EQ(groups, 3);
    const org::Theme th = theme();
    int spec = kids[1];
    const auto leaves = org::children_of(t, spec);
    const std::string p = plain(org::render(t, leaves[0], 90, 30, 0.0, th, "x"));
    CHECK(p.find("Options") != std::string::npos && p.find("Overlay") != std::string::npos && p.find("Extra Window") != std::string::npos);
    CHECK(p.find("Spectrogram") != std::string::npos && p.find("Oscilloscope") != std::string::npos);
}
