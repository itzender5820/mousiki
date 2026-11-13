// The help wizard (see wizard.cpp): the page layout, the keys it takes and the State that is kept
// while it is open.
#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The Mousiki Plus Help Wizard (SHIFT+w).
//
// One document for both modes. The music player (src/app.cpp) and the radio
// (src_radio/radio_ui.cpp, radio_main.cpp) only DRAW what this module hands
// them: the page text, its layout, the page / part / table-of-content state
// and the key handling all live here, so the two modes cannot drift apart.
//
// The text is a plain .txt file (Mousiki_Help_Wizard.txt in the repository,
// compiled in through src/wizard_text.inc by tools/wizard_to_inc.py). While
// editing it the environment variable MOUSIKI_WIZARD_FILE=/path/to/file.txt
// makes the program read that file instead, so no rebuild is needed.
//
// File format
//   [[New Page]]            starts a page (any case). Text before the first one is ignored,
//                           and so is every line that starts with =>>> (a note to the author).
//   page 1                  the greeting shown at the very first start after an installation (with the spinning disk)
//   page 2                  the greeting shown when the wizard is opened for the first time in a session
//   a page whose first line starts with TABLE OF CONTENT is a contents page: its numbered lines
//                           ("2 MUSIC PLAYER MODE", "2.1 Main UI", "2.1.2 Music File Paths") can be selected
//                           with UP / DOWN and opened with ENTER
//   a page whose first line starts with a number ("2.1.2 MUSIC FILE PATHS") belongs to that chapter; the
//                           chapter ("2.1") and its part ("2") are named in the top border
//   [[First Start]]         on a line of its own in a page: that page follows the first-start greeting once (the very first start after an
//                           installation); later it is only reached through the table of content
//   a contents line that ends with [[Header]] is a heading: it cannot be selected (a heading has no page of its own)
//   [[Header]] / [[Legend]] colours the capitalised words (or number) in front of it in the header / legend
//                           colour. At the end of a line that starts with => or NOTE: it colours the whole
//                           line, and on a contents page it always colours the whole line.
//   [Text][[Legend]]        the text in square brackets, followed directly by [[Legend]] (or [[Header]]), is drawn
//                           in that colour, whatever its case, and the brackets are not shown
//   [[Header_line]]         anywhere in a line: the whole line is drawn in the header colour
//   [anything]              in single square brackets is drawn in the legend colour
//   {VERSION}               the running version of Mousiki Plus
//
// Lines are never re-wrapped, they are shown exactly as written; only a line that is wider than the
// terminal allows is wrapped. Every page has an empty line at its beginning and its end. A page that is taller than
// the frame is scrollable: [UP] / [DOWN] move it one row, [SHIFT+UP] / [SHIFT+DOWN] a whole page (on a contents page
// they move the selection, and the page follows it). A scrollbar in the disk gradient colours shows it.
//
// The first two pages are the WELCOME pages (the first one only at the first start after an installation).
// They show the spinning disk to the left of the text: the disk starts kWelcomeLeft columns inside the frame, its
// vertical place is that of a 120x30 terminal, and the text is kWelcomeTextWidth columns wide at most. Every
// other page is not affected. The contents pages are centred (as one block, so all of them keep the same place).
//
// Moving with [->] / [<-] stays inside the main chapter of the page: from 3.1 only 3.x pages can be reached, the
// way to another chapter is the table of content. The welcome pages and the contents pages are one "chapter".
// ---------------------------------------------------------------------------

namespace wizard {

// Master switch. false: the wizard never opens (not on the first start, not on SHIFT+w).
inline constexpr bool kEnabled = true;

constexpr int kBoxWidth = 120;   // the frame is never wider than this (and is centred on a wider terminal)
constexpr int kMargin = 5;       // blank columns between the frame and the text, left and right
constexpr int kDiskGap = 7;      // between the disk and the text of a welcome page (the disk's own empty cells come on top)
constexpr int kWelcomeLeft = 8;  // columns in front of the disk of a welcome page, inside the frame (the text starts at
                                 // column 45, whatever the split: 8 + 30 disk + 7 gap)
constexpr int kWelcomeTextWidth = 72;   // the text of a welcome page is at most this wide (a longer line is wrapped)

enum class Role { Text, Header, Legend, Cursor, Disk, Scroll, Match };   // Scroll: the scrollbar (t = 0..1 down the frame, drawn in the disk gradient)

// A piece of a row in one colour. `t` is 0..1 from the top of the disk to its bottom (Role::Disk only).
struct Span {
    Role role = Role::Text;
    std::string text;
    float t = 0.0f;
};
using Row = std::vector<Span>;

enum class Key { None, Left, Right, Up, Down, PageUp, PageDown, Enter, Back, Contents, Close, Search };

struct State {
    int page = 0;
    int scroll = 0;           // the first row of the page that is shown (a page taller than the frame scrolls)
    int max_scroll = 0;       // the largest useful `scroll` in the frame of the last render()
    int view_rows = 20;       // the rows a frame shows (a "page" for SHIFT+UP / SHIFT+DOWN)
    int toc = 0;              // the selected entry of the table of content
    int min_page = 0;         // 1 once the first-start greeting has been left: it is not reachable again
    bool started = false;     // opened at least once by this process
    bool first_flow = false;  // the first start after an installation: the greeting leads on to the GETTING STARTED page (marked [[First Start]]), then to the contents
    double angle = 0.0;       // the disk on the first page
    // the search key ("/", SHIFT+7 on a German keyboard): search the whole text (Key::Search). While `searching`, the frontends hand every key to search_key().
    bool searching = false;
    std::string query;
    size_t qcaret = 0, qanchor = 0;      // the caret and the other end of the marked range in `query` (byte offsets)
    int sel = 0;                         // the selected result
    int first = 0;                       // the first result shown
    int jump_line = -1;                  // set when a result is opened: the next frame scrolls this line of the page into view
    // the results of `cache_query`, best first: the page and the line of its best fragment (filled by render())
    std::string cache_query;
    std::vector<std::pair<int, int>> results;
};

// What the frontend has to draw for one frame.
struct View {
    int box_w = 0;            // width of the frame, borders included
    int left = 0;             // blank columns in front of the frame (centring)
    std::string title;        // the top border's title, to be drawn in the header colour
    std::vector<Row> rows;    // the rows between the borders (all exactly box_w - 2 columns wide)
    int page_no = 0;          // "page n/N" of the bottom border
    int page_total = 0;
    bool scrollable = false;  // the page is taller than the frame (a scrollbar is drawn in the last column of every row)
    bool can_back = false, can_next = false;
    bool toc_page = false;
    bool search = false;      // the search view is shown: the bottom border should list the search keys
};

// Opens the wizard. `first_launch` (the first start after an installation) shows the greeting with the disk;
// otherwise the first opening of a session shows the second greeting and later openings come back at the page
// the wizard was left at.
void open(State& st, bool first_launch);

// True for the pages that are drawn with the spinning disk beside them (the two welcome pages).
bool shows_disk(int page);

// Handles one key. Returns true when the wizard should close.
bool handle_key(State& st, Key key);

// While State::searching: one raw key of the search view (typing into the query, UP / DOWN choose a result, SHIFT+UP / DOWN
// jump five, ENTER opens the chapter of the result at the matching line, ESC clears the query and then leaves the search).
// `arrow`: the key is an arrow key (it arrives as the letters A-D); `shifted`: SHIFT was held with it.
void search_key(State& st, int key, bool arrow, bool shifted);

// The frame for a terminal of `cols` columns and `body_rows` rows between the top and the bottom border.
// `disk` is the current frame of the disk (muisc::DiskArt::frame), may be empty. Clamps st.scroll and sets st.max_scroll.
View render(State& st, int cols, int body_rows, const std::vector<std::string>& disk);

// For tests and tools: number of pages, and the rows a page takes in a frame of `cols` columns.
int page_count();
int page_rows(int page, int cols);
int page_title_width(int page);   // the width of the title shown in the top border (information only)
// Non-empty when the text could not be read or has a problem (shown on the first page).
const std::string& problems();

// The text of the wizard is read and split into pages when it is first needed, and each page is turned into coloured cells only when it is
// drawn. release_memory() gives all of it back (the frontends call it when the wizard is closed): the next opening reads it again, which takes
// a few milliseconds. memory_held(): the document is in memory (for tests).
void release_memory();
bool memory_held();

} // namespace wizard
