// The audio book player, part 3: the list of time stamps. SHIFT+k lists the auto-marks, bookmarks and notes of every book; ENTER goes to one (the resume choices), SHIFT+p plays from it, SHIFT+d discards it (after a warning). A big overlay over the whole
// screen, one more face of Mode::BookOverlay. Member functions of App (declared in app.h).
#include "app.h"
#include "book_state.h"
#include "utf8_util.h"
#include "path_utf8.h"
#include "book_cue.h"
#include "book_notes.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <set>
#include "app_internal.h"

namespace muisc {

namespace {

// "10-24 06:50" in local time ("-" when the bookmark has no date: it was written by an older version)
std::string when_text(long long when) {
    if (when <= 0) return "-";
    const std::time_t t = static_cast<std::time_t>(when);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%m-%d %H:%M", &tmv);
    return b;
}

} // namespace

// Opens the book of that folder in the chapter pane (the books list cursor goes to it). False when it is not in the library.
bool App::book_open_dir(const std::string& dir) {
    if (book_->dir == dir && !chapters_.empty()) return true;
    auto find = [&]() {
        for (size_t i = 0; i < folder_view_.size(); ++i) if (folder_view_[i].path == dir && !folder_view_[i].container && !folder_view_[i].up) return static_cast<int>(i);
        return -1;
    };
    int row = find();
    if (row < 0) {   // not on this level of the books pane: go to the level that shows it
        last_folder_query_.clear();
        book_->nav.clear();
        book_refresh_view();
        std::string parent = path_utf8(path_from_utf8(dir).parent_path());
        for (const FolderSummary& f : filter_folders("")) {   // the folder has books below it: its own files are its first row
            const std::string below = dir + "/";
            const std::string below_w = dir + "\\";
            if (f.path.compare(0, below.size(), below) == 0 || f.path.compare(0, below_w.size(), below_w) == 0) { parent = dir; break; }
        }
        std::error_code ec;
        const fs::path root = path_from_utf8(settings_.audiobook_path);
        if (fs::equivalent(path_from_utf8(parent), root, ec) && !ec) parent.clear();
        book_->nav = parent;
        book_refresh_view();
        row = find();
        if (row < 0) { book_->nav.clear(); book_refresh_view(); row = find(); }
    }
    if (row < 0) return false;
    selected_ = row;
    book_open(row);
    return true;
}

// The rows: every time stamp of every book - the auto-mark, the bookmarks and the notes - newest first (notes have no date: they
// come last, in the order of their book).
void App::book_marks_fill() {
    book_->mrows.clear();
    std::set<std::string> dirs;
    for (const auto& t : all_local_tracks_) dirs.insert(path_utf8(t.path.parent_path()));
    for (const std::string& d : dirs) {
        const fs::path dp = path_from_utf8(d);
        const std::string name = path_utf8(dp.filename());
        BookCue cue;
        if (cue.load(BookCue::path_for(dp))) {
            int num = 0;
            for (const std::string& raw : cue.get_all_raw("REM BOOKMARK")) {
                BookMark m;
                ++num;
                if (!parse_bookmark(raw, m)) continue;
                BookStamp r; r.kind = 1; r.dir = d; r.book = name; r.file = m.file; r.sec = m.sec; r.when = m.when; r.num = num;
                book_->mrows.push_back(r);
            }
            BookMark am;
            if (parse_bookmark(cue.get_raw("REM AUTOMARK"), am)) {
                BookStamp r; r.kind = 0; r.dir = d; r.book = name; r.file = am.file; r.sec = am.sec; r.when = am.when; r.num = 1;
                book_->mrows.push_back(r);
            }
        }
        std::vector<BookNote> notes;
        if (book_notes::load(book_notes::path_for(dp), notes)) {
            int num = 0;
            for (const BookNote& n : notes) {
                ++num;
                BookStamp r; r.kind = 2; r.dir = d; r.book = name; r.file = n.file; r.sec = n.sec; r.title = n.title; r.num = num; r.note = n;
                r.when = n.set;   // the date the note was written (older notes have none)
                book_->mrows.push_back(r);
            }
        }
    }
    // the file order breaks ties (old files have no dates): the later line is the newer one
    std::vector<size_t> ord(book_->mrows.size());
    for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
    if (book_->mfilter == 1) {   // SHIFT+t: by book (A-Z); in a book the highest chapter first, in a chapter the furthest mark first
        std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
            const BookStamp& x = book_->mrows[a];
            const BookStamp& y = book_->mrows[b];
            if (x.book != y.book) return natural_less(x.book, y.book);
            if (x.file != y.file) return natural_less(y.file, x.file);
            if (x.sec != y.sec) return x.sec > y.sec;
            return a > b;
        });
    } else {   // by the date the time stamp was set, the newest first (the file order breaks ties: old files have no dates)
        std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
            if (book_->mrows[a].when != book_->mrows[b].when) return book_->mrows[a].when > book_->mrows[b].when;
            return a > b;
        });
    }
    std::vector<BookStamp> sorted;
    for (size_t i : ord) sorted.push_back(book_->mrows[i]);
    book_->mrows = std::move(sorted);
    const int n = static_cast<int>(book_->mrows.size());
    book_->msel = n ? std::clamp(book_->msel, 0, n - 1) : 0;
}

void App::book_marks_open() {
    book_->ov_kind = BookOv::Marks;
    book_->msel = book_->mscroll = 0;
    book_->mmode = 0;
    book_->mfilter = 0;
    book_->ov_status.clear();
    book_marks_fill();
    text_disengage();
    mode_ = Mode::BookOverlay;
}

bool App::book_marks_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int n = static_cast<int>(book_->mrows.size());
    const int body = std::max(3, term_rows_ - 7);
    auto say = [&](const std::string& s) { book_->ov_status = s; };
    if (book_->mmode == 1) {   // discard?
        if (!arrow && (key == 'y' || key == 'Y') && book_->msel >= 0 && book_->msel < n) {
            const BookStamp row = book_->mrows[static_cast<size_t>(book_->msel)];
            const bool ok = book_stamp_remove(row);
            book_marks_fill();
            say(ok ? std::string(book_stamp_type(row)) + " discarded  (CTRL+SHIFT+u brings it back)" : "could not discard it");
        } else say("nothing discarded");
        book_->mmode = 0;
        return true;
    }
    if (arrow && (key == 'A' || key == 'B')) {
        if (n) {
            const int step = last_key_was_shifted() ? body : 1;
            book_->msel = std::clamp(book_->msel + (key == 'A' ? -step : step), 0, n - 1);
        }
        return true;
    }
    if (arrow) return true;
    if (key == kKeyCtrlShiftU) { say(book_undo_pop()); book_marks_fill(); return true; }
    if (key == 27 || key == 'q' || key == 'K') { book_->mfilter = 0; mode_ = Mode::Browse; return true; }   // (the note sorting is cleared on the way out)
    if (key == 'T') {   // SHIFT+t: sorted by the date they were set <-> by book, chapter and progress
        book_->mfilter = (book_->mfilter + 1) % 2;
        book_->msel = book_->mscroll = 0;
        book_marks_fill();
        say(book_->mfilter == 0 ? "sorted by the date the time stamp was set, the newest first" : "sorted by book; in a book the highest chapter and the furthest mark first");
        return true;
    }
    if (n == 0 || book_->msel < 0 || book_->msel >= n) return true;
    const BookStamp row = book_->mrows[static_cast<size_t>(book_->msel)];
    if (key == 'D' || key == kKeyDelete) { book_->mmode = 1; return true; }
    if (key == '\r' || key == '\n') {   // the resume choices of that bookmark (after the question about the bookmark here)
        mode_ = Mode::Browse;
        const BookMark mk = [&]() { BookMark m; m.file = row.file; m.sec = row.sec; m.when = row.when; return m; }();
        const std::string label = row.kind == 0 ? "auto-mark" : row.kind == 1 ? "bookmark " + std::to_string(row.num) : "note";
        book_ask_then(std::string("Going to the ") + (row.kind == 0 ? "auto-mark" : row.kind == 1 ? "bookmark" : "note") + " in " + row.book, [this, row, mk, label]() {
            if (!book_open_dir(row.dir)) { status_line_ = "that book is no longer in the library"; return; }
            const int c = book_chapter_of(row.file);
            if (c < 0) { status_line_ = "that chapter is no longer in the book"; return; }
            queue_selected_ = c;
            book_fit_scroll();
            book_resume_open(c, mk, label);
        });
        return true;
    }
    if (key == 'B') {   // SHIFT+b: an auto-mark becomes a regular bookmark
        if (row.kind != 0) { say("SHIFT+b converts an auto-mark (the type column says Auto-mark) to a bookmark"); return true; }
        BookMark am; am.file = row.file; am.sec = row.sec; am.when = row.when;
        const int num = book_automark_convert(row.dir, am);
        book_marks_fill();
        say(num > 0 ? "auto-mark converted to bookmark " + std::to_string(num) : "could not write the .cue file");
        return true;
    }
    if (key == 'P') {   // plays from there; the list stays
        if (!book_open_dir(row.dir)) { say("that book is no longer in the library"); return true; }
        const int c = book_chapter_of(row.file);
        if (c < 0) { say("that chapter is no longer in the book"); return true; }
        queue_selected_ = c;
        book_fit_scroll();
        book_play_at(c, row.sec);
        say("playing " + stem_of(row.file) + " from " + book_notes::time_text(row.sec));
        return true;
    }
    return true;
}

std::vector<std::string> App::build_book_marks_panel(int panel_w, int panel_h) const {
    const int W = panel_w, inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const Framer framed{border + settings_.box_vertical + R, inner};
    const std::string head = header_sgr(settings_), leg = legend_sgr(settings_);
    const std::string plain_ink = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
    std::vector<std::string> lines;
    lines.push_back(box_top("Time stamps - auto-marks, bookmarks, notes", W, border));

    const int n = static_cast<int>(book_->mrows.size());
    const std::vector<LegendGroup> groups = {
        {"List", {"[\u2191\u2193] Select", "[SHIFT+\u2191\u2193] Page", "[ENTER] Resume choices\001Resume", "[ESC] Close (resets the sorting)\001Close"}},
        {"Time stamp", {"[SHIFT+p] Play from here\001Play", "[SHIFT+d] Discard (asks first)\001Discard", "[CTRL+SHIFT+u] Undo discard\001Undo",
                      "[SHIFT+b] Auto-mark to bookmark\001To bookmark", "[SHIFT+t] Sort: date / book\001Sort"}}};
    const std::vector<LegendRow> lrows = book_overlay_legend(W, groups, 3);
    const int body = std::max(3, panel_h - 4 - static_cast<int>(lrows.size()));   // top, header, status, bottom border, the legend
    int first = std::clamp(book_->mscroll, 0, std::max(0, n - body));
    if (book_->msel < first) first = book_->msel;
    if (book_->msel >= first + body) first = book_->msel - body + 1;
    first = std::max(0, first);

    // columns: WHEN, TYPE, BOOK, CHAPTER (a bookmark's number or a note's title after it), TIME
    const int when_w = 11, type_w = 9, time_w = 9;
    const int book_w = std::clamp(inner * 28 / 100, 14, 44);
    const int chap_w = std::clamp(inner - 2 - when_w - 2 - type_w - 2 - (book_w + 2) - time_w, 8, 60);
    auto cols = [&](const std::string& w, const std::string& ty, const std::string& b, const std::string& c, const std::string& t) {
        std::string s = "  " + pad_right(truncate_str(w, when_w), when_w) + "  ";
        s += pad_right(truncate_str(ty, type_w), type_w) + "  ";
        s += pad_right(truncate_str(apply_font_map(b, settings_.font_map), book_w), book_w) + "  ";
        s += pad_right(truncate_str(apply_font_map(c, settings_.font_map), chap_w), chap_w);
        s += pad_right(t, time_w);
        return pad_right(truncate_str(s, inner), inner);
    };
    {
        const std::string h = cols("WHEN", "TYPE", "BOOK", "CHAPTER", "TIME");
        lines.push_back(framed(head + h, inner));
    }
    for (int i = 0; i < body; ++i) {
        const int idx = first + i;
        if (idx >= n) { lines.push_back(framed("", 0)); continue; }
        const BookStamp& r = book_->mrows[static_cast<size_t>(idx)];
        std::string chap = stem_of(r.file);
        if (r.kind == 1) chap += "  #" + std::to_string(r.num);
        else if (r.kind == 2) chap += "  \u201c" + book_stamp_name(r) + "\u201d";
        const std::string plain = cols(when_text(r.when), book_stamp_type(r), r.book, chap, book_notes::time_text(r.sec));
        const bool sel = idx == book_->msel;
        const std::string sgr = sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color)
                                    : (r.kind == 0 ? head + bg_ansi_for(settings_.list_inactive_bg_color) : plain_ink);
        lines.push_back(framed(sgr + plain, inner));
    }
    if (n == 0) {   // the empty list says how to get a bookmark
        const std::string t = "Nothing yet. The auto-mark appears by itself while you listen; SHIFT+b sets a bookmark, SHIFT+n a note.";
        lines[2] = framed(leg + truncate_str(t, inner), display_width(truncate_str(t, inner)));
    }
    {   // the status / question row
        std::string body_s;
        int used = 0;
        if (book_->mmode == 1) {
            const std::string t = "Discard this time stamp? CTRL+SHIFT+u brings it back.   [y] yes   [n] no";
            const std::string hb = bg_ansi_for(settings_.header_color);
            body_s = (hb.empty() ? std::string("\x1b[7m") : hb + "\x1b[30m") + pad_right(truncate_str(t, inner), inner);
            used = inner;
        } else {
            const std::string t = truncate_str(book_->ov_status.empty() ? std::to_string(n) + (n == 1 ? " time stamp" : " time stamps") + std::string("  (auto-marks are shown bright; sorted by ") + (book_->mfilter == 1 ? "book, SHIFT+t: by date)" : "date, SHIFT+t: by book)") : book_->ov_status, inner);
            body_s = leg + t;
            used = display_width(t);
        }
        lines.push_back(framed(body_s, used));
    }
    lines.push_back(box_bottom(W, "", border_bottom));
    for (size_t i = 0; i < lrows.size(); ++i) lines.push_back(paint_legend(settings_, lrows, i, border, W, false));
    return lines;
}

} // namespace muisc
