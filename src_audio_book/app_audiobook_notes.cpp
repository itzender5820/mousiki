// The audio book player, part 2: the bookmark (where one stopped listening, kept in the book's .cue), the questions around it
// (set a bookmark? / where to resume), the book notes (an editor in the visualization field, <folder>_notes.txt, the big notes
// overlay with export and import). Member functions of App (declared in app.h); the helpers shared with app.cpp are in
// app_internal.h.
#include "overlay_common.h"
#include "time_util.h"
#include "string_util.h"
#include "app.h"
#include "app_state_fo.h"
#include "book_state.h"
#include "text_edit.h"
#include "utf8_util.h"
#include "path_utf8.h"
#include "book_cue.h"
#include "book_notes.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "app_internal.h"

namespace muisc {

namespace {


} // namespace

// ---------------------------------------------------------------------
// The bookmark: REM BOOKMARK "<file name>" <seconds> in <book folder>/<folder>.cue, one per book (where one stopped listening).
// ---------------------------------------------------------------------
int App::book_chapter_of(const std::string& file) const {
    for (size_t i = 0; i < chapters_.size(); ++i)
        if (path_utf8(chapters_[i].path.filename()) == file) return static_cast<int>(i);
    return -1;
}

namespace {
// The newest bookmark: the latest `when`, among equal ones (old files have none) the one written last.
const BookMark* newest_of(const std::vector<BookMark>& v) {
    const BookMark* best = nullptr;
    for (const BookMark& m : v) if (!best || m.when >= best->when) best = &m;
    return best;
}

std::vector<BookMark> marks_of(const BookCue& cue) {
    std::vector<BookMark> v;
    for (const std::string& raw : cue.get_all_raw("REM BOOKMARK")) {
        BookMark m;
        if (parse_bookmark(raw, m)) v.push_back(m);
    }
    return v;
}
} // namespace

void App::book_bookmark_load() {
    book_->bookmark = BookBookmark();
    book_->automark = BookMark();
    book_->marks.clear();
    book_->notes.clear();
    if (book_->dir.empty()) return;
    const fs::path dir = path_from_utf8(book_->dir);
    BookCue cue;
    if (cue.load(BookCue::path_for(dir))) {
        book_->marks = marks_of(cue);
        if (const BookMark* n = newest_of(book_->marks)) book_->bookmark = *n;
        BookMark am;
        if (parse_bookmark(cue.get_raw("REM AUTOMARK"), am)) book_->automark = am;
    }
    book_notes::load(book_notes::path_for(dir), book_->notes);
}

// Writes the bookmarks of a book (its .cue keeps everything else as it is).
void App::book_marks_write(const std::string& dir, const std::vector<BookMark>& marks) {
    const fs::path cp = BookCue::path_for(path_from_utf8(dir));
    BookCue cue;
    cue.load(cp);
    std::vector<std::string> raw;
    for (const BookMark& m : marks) raw.push_back(format_bookmark(m));
    cue.set_all_raw("REM BOOKMARK", raw);
    if (!cue.save(cp)) { status_line_ = "could not write " + path_utf8(cp.filename()); return; }
    if (dir == book_->dir) book_bookmark_load();
}

// A new bookmark for the book that plays (which need not be the open one). A book may carry many; one that lies within a second of
// an old one in the same chapter replaces it.
void App::book_bookmark_store(const std::string& file, double sec) {
    const std::string dir = path_utf8(current_.path.parent_path());
    BookMark m;
    m.file = file;
    m.sec = std::max(0.0, sec);
    m.when = epoch_now();
    BookCue cue;
    std::vector<BookMark> v;
    if (cue.load(BookCue::path_for(path_from_utf8(dir)))) v = marks_of(cue);
    for (BookMark& o : v) if (o.file == m.file && std::fabs(o.sec - m.sec) < 1.0) { o = m; book_marks_write(dir, v); return; }
    v.push_back(m);
    book_marks_write(dir, v);
}

int App::book_bookmark_chapter() const {
    return book_->bookmark.valid() ? book_chapter_of(book_->bookmark.file) : -1;
}

int App::book_mark_in_chapter(int idx) const {
    if (idx < 0 || idx >= static_cast<int>(chapters_.size())) return -1;
    const std::string file = path_utf8(chapters_[static_cast<size_t>(idx)].path.filename());
    int best = -1;
    for (size_t i = 0; i < book_->marks.size(); ++i)
        if (book_->marks[i].file == file && (best < 0 || book_->marks[i].when >= book_->marks[static_cast<size_t>(best)].when)) best = static_cast<int>(i);
    return best;
}

// SHIFT+b: a bookmark right here.
void App::book_set_bookmark_now() {
    if (!has_track_ || !current_.is_local) { status_line_ = "play a chapter first: a bookmark marks the place where you stopped"; return; }
    const double el = player_.poll_elapsed();
    book_bookmark_store(path_utf8(current_.path.filename()), el);
    status_line_ = "bookmark " + std::to_string(book_->marks.size()) + " set: " + stem_of(path_utf8(current_.path.filename())) + " at " + clock_text(el);
}

// ---------------------------------------------------------------------
// The questions. Everything that leaves the place one is at (another chapter, another mode, quitting) asks first whether to set a
// bookmark; the audio is paused while the question is open.
// ---------------------------------------------------------------------
bool App::book_ask_due() const {
    return settings_.book_ask_bookmark && has_track_ && current_.is_local && player_.poll_elapsed() > 2.0;
}

void App::book_ask_then(const std::string& what, std::function<void()> then) {
    book_automark_tick(true);   // where one leaves is kept as the auto-mark whatever the answer is
    if (!book_ask_due()) { then(); return; }
    book_->then = std::move(then);
    book_->ask_what = what;
    book_->ask_file = path_utf8(current_.path.filename());
    book_->ask_sec = player_.poll_elapsed();
    book_->prompt_paused = !player_.is_paused();
    if (book_->prompt_paused) player_.pause();
    book_->ov_kind = BookOv::Ask;
    book_->ov_status.clear();
    text_disengage();
    mode_ = Mode::BookOverlay;
}

bool App::book_ask_key(int key) {
    const bool arrow = last_key_was_arrow();
    const bool yes = !arrow && (key == 'y' || key == 'Y' || key == '\r' || key == '\n');
    const bool no = !arrow && (key == 'n' || key == 'N');
    if (!yes && !no && key != 27) return true;
    mode_ = Mode::Browse;
    std::function<void()> then = std::move(book_->then);
    book_->then = nullptr;
    if (key == 27) {   // stay where one is: the audio goes on
        if (book_->prompt_paused) player_.resume();
        book_->prompt_paused = false;
        return true;
    }
    if (yes) {
        book_bookmark_store(book_->ask_file, book_->ask_sec);
        status_line_ = "bookmark set: " + stem_of(book_->ask_file) + " at " + clock_text(book_->ask_sec);
    }
    if (then) then();   // (the paused state stays until what follows plays something, or the resume choice is given up)
    return true;
}

std::vector<std::string> App::build_book_ask_panel(int panel_w) const {
    const int W = panel_w, inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const Framer framed{border + settings_.box_vertical + "\x1b[0m", inner};
    std::vector<std::string> lines;
    lines.push_back(box_top("Bookmark", W, border));
    auto centred = [&](const std::string& sgr, const std::string& plain) {
        const std::string t = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(t)) / 2);
        return framed(sgr + std::string(static_cast<size_t>(left), ' ') + t + "\x1b[0m", left + display_width(t));
    };
    lines.push_back(framed("", 0));
    lines.push_back(centred(header_sgr(settings_), "Set a bookmark here?"));
    lines.push_back(framed("", 0));
    lines.push_back(centred(ansi_for(settings_.list_color, false), stem_of(book_->ask_file) + "  at  " + clock_text(book_->ask_sec)));
    lines.push_back(centred(legend_sgr(settings_), book_->ask_what));
    lines.push_back(framed("", 0));
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return framed(border_bottom + std::string(static_cast<size_t>(left), ' ') + tx, left + display_width(tx));
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[y] / [ENTER] set the bookmark   [n] no bookmark", "[ESC] stay here (the audio goes on)"}, leg_in, leg_bot);
    return lines;
}

// [d] in the chapter pane on a chapter that carries the bookmark: a small question, then the bookmark is deleted from the .cue file.
void App::book_delmark_open(int idx) {
    const int mi = book_mark_in_chapter(idx);
    if (mi < 0) { status_line_ = "this chapter has no bookmark"; return; }
    book_->del_mark = book_->marks[static_cast<size_t>(mi)];
    book_->ov_kind = BookOv::DelMark;
    book_->ov_status.clear();
    text_disengage();
    mode_ = Mode::BookOverlay;
}

bool App::book_delmark_key(int key) {
    const bool arrow = last_key_was_arrow();
    const bool yes = !arrow && (key == 'y' || key == 'Y' || key == '\r' || key == '\n');
    const bool no = !arrow && (key == 'n' || key == 'N');
    if (!yes && !no && key != 27) return true;
    mode_ = Mode::Browse;
    if (!yes) return true;
    BookStamp st;
    st.kind = 1; st.dir = book_->dir; st.book = path_utf8(path_from_utf8(book_->dir).filename());
    st.file = book_->del_mark.file; st.sec = book_->del_mark.sec; st.when = book_->del_mark.when;
    if (book_stamp_remove(st))
        status_line_ = "bookmark deleted: " + stem_of(book_->del_mark.file) + " at " + clock_text(book_->del_mark.sec) + "  (CTRL+SHIFT+u brings it back)";
    return true;
}

std::vector<std::string> App::build_book_delmark_panel(int panel_w) const {
    const int W = panel_w, inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const Framer framed{border + settings_.box_vertical + "\x1b[0m", inner};
    std::vector<std::string> lines;
    lines.push_back(box_top("Bookmark", W, border));
    auto centred = [&](const std::string& sgr, const std::string& plain) {
        const std::string t = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(t)) / 2);
        return framed(sgr + std::string(static_cast<size_t>(left), ' ') + t + "\x1b[0m", left + display_width(t));
    };
    lines.push_back(framed("", 0));
    lines.push_back(centred(header_sgr(settings_), "Delete this bookmark?"));
    lines.push_back(framed("", 0));
    lines.push_back(centred(ansi_for(settings_.list_color, false), stem_of(book_->del_mark.file) + "  at  " + clock_text(book_->del_mark.sec)));
    lines.push_back(centred(legend_sgr(settings_), "It is removed from the book's .cue file for good."));
    lines.push_back(framed("", 0));
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return framed(border_bottom + std::string(static_cast<size_t>(left), ' ') + tx, left + display_width(tx));
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[y] / [ENTER] delete it   [n] keep it", "[ESC] keep it"}, leg_in, leg_bot);
    return lines;
}

// The user wants chapter idx (ENTER, n, b): leave the current one with the question, arrive with the resume choices when it
// carries the bookmark.
void App::book_go(int idx) {
    if (idx < 0 || idx >= static_cast<int>(chapters_.size())) return;
    // A chapter that carries an auto-mark only: "continue where you left?". One bookmark: the resume choices. Several time stamps (a
    // bookmark and the auto-mark, or more bookmarks): the list of them. Nothing: it plays.
    auto arrive = [this, idx]() {
        std::vector<BookStamp> st = book_chapter_stamps(idx, idx != book_current_chapter());
        int nb = 0, na = 0;
        for (const BookStamp& b : st) { if (b.kind == 1) ++nb; else if (b.kind == 0) ++na; }
        if (nb == 0 && na == 1) book_continue_open(idx);
        else if (nb == 1 && na == 0) {
            for (const BookStamp& b : st)
                if (b.kind == 1) { BookMark m; m.file = b.file; m.sec = b.sec; m.when = b.when; book_resume_open(idx, m); break; }
        }
        else if (nb + na > 0) book_pick_open(idx, std::move(st));
        else { book_->prompt_paused = false; book_play_chapter(idx); }
    };
    if (idx == book_current_chapter()) arrive();
    else book_ask_then("Going on to " + stem_of(path_utf8(chapters_[static_cast<size_t>(idx)].path.filename())), arrive);
}

// Reading history ENTER: opens the book that holds `path` and goes to that chapter (false when the file is gone).
bool App::book_play_file(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !fs::exists(path_from_utf8(path), ec)) return false;
    if (!book_open_dir(path_utf8(path_from_utf8(path).parent_path()))) return false;
    for (size_t i = 0; i < chapters_.size(); ++i)
        if (path_utf8(chapters_[i].path) == path) { qst_.selected = static_cast<int>(i); book_fit_scroll(); book_go(static_cast<int>(i)); return true; }
    return false;
}

// Starts chapter idx and goes to `sec` in it (once it is loaded).
void App::book_play_at(int idx, double sec) {
    if (idx < 0 || idx >= static_cast<int>(chapters_.size())) return;
    book_->prompt_paused = false;
    if (idx == book_current_chapter()) {
        player_.seek_relative(sec - player_.poll_elapsed());
        if (player_.is_paused()) player_.resume();
        return;
    }
    book_play_chapter(idx);
    book_->seek_pending = true;
    book_->seek_file = path_utf8(chapters_[static_cast<size_t>(idx)].path);
    book_->seek_sec = std::max(0.0, sec);
    book_->seek_at = std::chrono::steady_clock::now();
}

void App::book_resume_open(int idx, const BookMark& mark, const std::string& label) {
    book_->resume_idx = idx;
    book_->resume_mark = mark;
    book_->resume_label = label;
    book_->resume_sel = 0;
    book_->ov_kind = BookOv::Resume;
    text_disengage();
    mode_ = Mode::BookOverlay;
}

namespace {
struct ResumeChoice { const char* label; double back; };   // back < 0: the start of the chapter
const ResumeChoice kResume[] = {{"right on the spot", 0}, {"10 seconds before", 10}, {"20 seconds before", 20},
                                {"30 seconds before", 30}, {"60 seconds before", 60}, {"the start of the chapter", -1}};
} // namespace

bool App::book_resume_key(int key) {
    const bool arrow = last_key_was_arrow();
    constexpr int n = 6;
    if (arrow && key == 'A') { book_->resume_sel = (book_->resume_sel + n - 1) % n; return true; }
    if (arrow && key == 'B') { book_->resume_sel = (book_->resume_sel + 1) % n; return true; }
    int pick = -1;
    if (!arrow && key >= '1' && key <= '6') pick = key - '1';
    if (!arrow && (key == '\r' || key == '\n')) pick = book_->resume_sel;
    if (pick >= 0) {
        mode_ = Mode::Browse;
        const double back = kResume[pick].back;
        const double at = back < 0 ? 0.0 : std::max(0.0, book_->resume_mark.sec - back);
        book_play_at(book_->resume_idx, at);
        return true;
    }
    if (key == 27 && !arrow) {   // give up: the chapter that was paused for the question goes on
        mode_ = Mode::Browse;
        if (book_->prompt_paused) player_.resume();
        book_->prompt_paused = false;
    }
    return true;
}

std::vector<std::string> App::build_book_resume_panel(int panel_w) const {
    const int W = panel_w, inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const Framer framed{border + settings_.box_vertical + "\x1b[0m", inner};
    std::vector<std::string> lines;
    lines.push_back(box_top("Resume", W, border));
    const std::string what = book_->resume_idx >= 0 && book_->resume_idx < static_cast<int>(chapters_.size())
        ? stem_of(path_utf8(chapters_[static_cast<size_t>(book_->resume_idx)].path.filename())) : std::string();
    const std::string head = truncate_str(what + "  -  " + book_->resume_label + " at " + clock_text(book_->resume_mark.sec), inner);
    lines.push_back(framed(header_sgr(settings_) + head, display_width(head)));
    lines.push_back(framed("", 0));
    const std::string plain_ink = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
    for (int i = 0; i < 6; ++i) {
        const bool sel = i == book_->resume_sel;
        std::string label = std::to_string(i + 1) + "  " + kResume[i].label;
        if (kResume[i].back >= 0) label += "  (" + clock_text(std::max(0.0, book_->resume_mark.sec - kResume[i].back)) + ")";
        else label += "  (00:00)";
        const std::string row = pad_right(truncate_str((sel ? "> " : "  ") + label, inner), inner);
        lines.push_back(framed((sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) : plain_ink) + row, inner));
    }
    lines.push_back(framed("", 0));
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return framed(border_bottom + std::string(static_cast<size_t>(left), ' ') + tx, left + display_width(tx));
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[UP/DOWN] choose  [ENTER] / [1-6] start", "[ESC] do not play it"}, leg_in, leg_bot);
    return lines;
}

// ---------------------------------------------------------------------
// Time stamps. A book has three kinds: the auto-mark (REM AUTOMARK, written by the program while one listens), regular bookmarks
// (REM BOOKMARK, as many as one likes) and notes (a bookmark with a text; <folder>_notes.txt).
// ---------------------------------------------------------------------
const char* App::book_stamp_type(const BookStamp& s) { return s.kind == 0 ? "Auto-mark" : s.kind == 1 ? "Bookmark" : "Note"; }

std::string App::book_stamp_name(const BookStamp& s) {
    if (s.kind == 0) return "Auto-mark";
    if (s.kind == 2 && !trim_ws(s.title).empty()) return s.title;
    return std::string(book_stamp_type(s)) + " " + std::to_string(std::max(1, s.num));
}

// The auto-mark of the chapter that plays: every few seconds, and when one leaves (book_ask_then). Nothing is written while the
// position does not move (paused), and not while "leave, but keep the time stamp" holds the old one back.
void App::book_automark_tick(bool force) {
    if (!has_track_ || !current_.is_local || book_->seek_pending || load_in_progress_.load()) return;
    const double el = player_.poll_elapsed();
    if (el <= 3.0) return;
    const std::string file = path_utf8(current_.path.filename());
    if (book_->am_hold) {
        if (file == book_->am_hold_file && el < book_->am_hold_sec) return;
        book_->am_hold = false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - book_->am_last < std::chrono::seconds(5)) return;
    if (file == book_->am_last_file && std::fabs(el - book_->am_last_sec) < (force ? 0.5 : 4.0)) { book_->am_last = now; return; }
    const std::string dir = path_utf8(current_.path.parent_path());
    BookMark m;
    m.file = file;
    m.sec = el;
    m.when = epoch_now();
    const fs::path cp = BookCue::path_for(path_from_utf8(dir));
    BookCue cue;
    cue.load(cp);
    cue.set_raw("REM AUTOMARK", format_bookmark(m));
    book_->am_last = now;
    book_->am_last_file = file;
    book_->am_last_sec = el;
    if (!cue.save(cp)) return;   // (no message: it would come again every few seconds)
    if (dir == book_->dir) book_->automark = m;
}

// The time stamps of chapter idx: the auto-mark (unless it lies within 5 seconds of a bookmark: that is the same place), the
// bookmarks and the notes.
std::vector<BookStamp> App::book_chapter_stamps(int idx, bool with_auto) const {
    std::vector<BookStamp> out;
    if (idx < 0 || idx >= static_cast<int>(chapters_.size())) return out;
    const std::string file = path_utf8(chapters_[static_cast<size_t>(idx)].path.filename());
    const std::string book = path_utf8(path_from_utf8(book_->dir).filename());
    std::vector<BookStamp> marks, notes;
    for (size_t i = 0; i < book_->marks.size(); ++i) {
        const BookMark& m = book_->marks[i];
        if (m.file != file || m.sec <= 3.0) continue;
        BookStamp b; b.kind = 1; b.dir = book_->dir; b.book = book; b.file = file; b.sec = m.sec; b.when = m.when; b.num = static_cast<int>(i) + 1;
        marks.push_back(b);
    }
    for (size_t i = 0; i < book_->notes.size(); ++i) {
        const BookNote& n = book_->notes[i];
        if (n.file != file) continue;
        BookStamp b; b.kind = 2; b.dir = book_->dir; b.book = book; b.file = file; b.sec = n.sec; b.title = n.title; b.num = static_cast<int>(i) + 1; b.note = n;
        notes.push_back(b);
    }
    auto by_sec = [](const BookStamp& a, const BookStamp& b) { return a.sec < b.sec; };
    std::stable_sort(marks.begin(), marks.end(), by_sec);
    std::stable_sort(notes.begin(), notes.end(), by_sec);
    if (with_auto && book_->automark.valid() && book_->automark.file == file && book_->automark.sec > 3.0) {
        bool same = false;
        for (const BookStamp& b : marks) if (std::fabs(b.sec - book_->automark.sec) < 5.0) same = true;
        if (!same) {
            BookStamp a; a.kind = 0; a.dir = book_->dir; a.book = book; a.file = file; a.sec = book_->automark.sec; a.when = book_->automark.when; a.num = 1;
            out.push_back(a);
        }
    }
    out.insert(out.end(), marks.begin(), marks.end());
    out.insert(out.end(), notes.begin(), notes.end());
    return out;
}

// The auto-mark becomes a regular bookmark (its date stays) and the auto-mark line goes. Returns the number of the new bookmark, 0 on a
// write error.
int App::book_automark_convert(const std::string& dir, const BookMark& am) {
    const fs::path cp = BookCue::path_for(path_from_utf8(dir));
    BookCue cue;
    cue.load(cp);
    std::vector<BookMark> v = marks_of(cue);
    BookMark m = am;
    if (m.when <= 0) m.when = epoch_now();
    bool same = false;
    for (const BookMark& o : v) if (o.file == m.file && std::fabs(o.sec - m.sec) < 1.0) same = true;
    if (!same) v.push_back(m);
    std::vector<std::string> raw;
    for (const BookMark& x : v) raw.push_back(format_bookmark(x));
    cue.set_all_raw("REM BOOKMARK", raw);
    cue.set_raw("REM AUTOMARK", "");
    if (!cue.save(cp)) return 0;
    book_stamps_changed(dir);
    return static_cast<int>(v.size());
}

void App::book_stamps_changed(const std::string& dir) {
    if (dir == book_->dir) book_bookmark_load();
}

namespace {
bool same_mark(const BookMark& a, const BookMark& b) { return a.file == b.file && a.when == b.when && std::fabs(a.sec - b.sec) < 0.05; }
}

// Deletes a time stamp from its file and keeps it for the undo (CTRL+SHIFT+u). False when it is not there (any more).
bool App::book_stamp_remove(BookStamp s) {
    const fs::path book = path_from_utf8(s.dir);
    if (s.kind == 2) {
        std::vector<BookNote> v;
        book_notes::load(book_notes::path_for(book), v);
        size_t at = v.size();
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == s.note) { at = i; break; }
        if (at == v.size()) return false;
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(at));
        if (!book_notes::save(book_notes::path_for(book), v)) return false;
        s.num = static_cast<int>(at) + 1;
    } else {
        const fs::path cp = BookCue::path_for(book);
        BookCue cue;
        if (!cue.load(cp)) return false;
        if (s.kind == 0) {
            if (cue.get_raw("REM AUTOMARK").empty()) return false;
            cue.set_raw("REM AUTOMARK", "");
            if (!cue.save(cp)) return false;
        } else {
            std::vector<BookMark> v = marks_of(cue);
            BookMark key; key.file = s.file; key.sec = s.sec; key.when = s.when;
            size_t at = v.size();
            for (size_t i = 0; i < v.size(); ++i) if (same_mark(v[i], key)) { at = i; break; }
            if (at == v.size()) return false;
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(at));
            s.num = static_cast<int>(at) + 1;
            std::vector<std::string> raw;
            for (const BookMark& m : v) raw.push_back(format_bookmark(m));
            cue.set_all_raw("REM BOOKMARK", raw);
            if (!cue.save(cp)) return false;
        }
    }
    book_->undo.push_back(s);
    if (book_->undo.size() > 30) book_->undo.erase(book_->undo.begin());
    book_stamps_changed(s.dir);
    return true;
}

bool App::book_stamp_restore(const BookStamp& s) {
    const fs::path book = path_from_utf8(s.dir);
    if (s.kind == 2) {
        std::vector<BookNote> v;
        book_notes::load(book_notes::path_for(book), v);
        const size_t at = std::min(v.size(), static_cast<size_t>(std::max(1, s.num) - 1));
        v.insert(v.begin() + static_cast<std::ptrdiff_t>(at), s.note);
        if (!book_notes::save(book_notes::path_for(book), v)) return false;
    } else {
        const fs::path cp = BookCue::path_for(book);
        BookCue cue;
        cue.load(cp);
        BookMark m; m.file = s.file; m.sec = s.sec; m.when = s.when;
        if (s.kind == 0) cue.set_raw("REM AUTOMARK", format_bookmark(m));
        else {
            std::vector<BookMark> v = marks_of(cue);
            const size_t at = std::min(v.size(), static_cast<size_t>(std::max(1, s.num) - 1));
            v.insert(v.begin() + static_cast<std::ptrdiff_t>(at), m);
            std::vector<std::string> raw;
            for (const BookMark& x : v) raw.push_back(format_bookmark(x));
            cue.set_all_raw("REM BOOKMARK", raw);
        }
        if (!cue.save(cp)) return false;
    }
    book_stamps_changed(s.dir);
    return true;
}

// CTRL+SHIFT+u: the last deletion comes back (up to 30, newest first).
std::string App::book_undo_pop() {
    while (!book_->undo.empty()) {
        const BookStamp s = book_->undo.back();
        book_->undo.pop_back();
        if (book_stamp_restore(s)) {
            if (mode_ == Mode::BookOverlay && book_->ov_kind == BookOv::Marks) book_marks_fill();
            return std::string("brought back: ") + book_stamp_type(s) + " " + (s.kind == 1 ? std::to_string(s.num) + " " : std::string()) +
                   "in " + stem_of(s.file) + " at " + clock_text(s.sec);
        }
    }
    return "nothing to undo";
}

// ---------------------------------------------------------------------
// "Continue where you left?" and the list of the time stamps of a chapter
// ---------------------------------------------------------------------
void App::book_continue_open(int idx) {
    const std::vector<BookStamp> st = book_chapter_stamps(idx, true);
    if (st.empty()) { book_->prompt_paused = false; book_play_chapter(idx); return; }
    book_->cont_idx = idx;
    book_->cont_mark.file = st[0].file;
    book_->cont_mark.sec = st[0].sec;
    book_->cont_mark.when = st[0].when;
    book_->ov_kind = BookOv::Continue;
    text_disengage();
    mode_ = Mode::BookOverlay;
}

bool App::book_continue_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (arrow) return true;
    const bool yes = key == 'y' || key == 'Y' || key == '\r' || key == '\n';
    if (!yes && key != 'n' && key != 'N' && key != 'b' && key != 'B' && key != 27) return true;
    mode_ = Mode::Browse;
    if (yes) { book_play_at(book_->cont_idx, book_->cont_mark.sec); return true; }   // only "yes" starts the chapter
    // Everything else leaves: nothing is played (the chapter that was paused for the question goes on).
    if (book_->prompt_paused) player_.resume();
    book_->prompt_paused = false;
    if (key == 'b' || key == 'B') {   // the auto-mark becomes a regular bookmark (kept for good)
        const int num = book_automark_convert(book_->dir, book_->cont_mark);
        status_line_ = num > 0 ? "auto-mark converted to bookmark " + std::to_string(num) + ": " + stem_of(book_->cont_mark.file) + " at " + clock_text(book_->cont_mark.sec)
                               : "could not write the .cue file";
    } else if (key == 'n' || key == 'N') {
        status_line_ = "left; the auto-mark at " + clock_text(book_->cont_mark.sec) + " is kept";
    }
    return true;
}

std::vector<std::string> App::build_book_continue_panel(int panel_w) const {
    const int W = panel_w, inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const Framer framed{border + settings_.box_vertical + "\x1b[0m", inner};
    std::vector<std::string> lines;
    lines.push_back(box_top("Continue", W, border));
    auto centred = [&](const std::string& sgr, const std::string& plain) {
        const std::string t = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(t)) / 2);
        return framed(sgr + std::string(static_cast<size_t>(left), ' ') + t + "\x1b[0m", left + display_width(t));
    };
    lines.push_back(framed("", 0));
    lines.push_back(centred(header_sgr(settings_), inner >= 42 ? "Do you want to continue where you left?" : "Continue where you left?"));
    lines.push_back(framed("", 0));
    lines.push_back(centred(ansi_for(settings_.list_color, false), stem_of(book_->cont_mark.file) + "  at  " + clock_text(book_->cont_mark.sec)));
    lines.push_back(centred(legend_sgr(settings_), "(the auto-mark: nothing was bookmarked there)"));
    lines.push_back(framed("", 0));
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return framed(border_bottom + std::string(static_cast<size_t>(left), ' ') + tx, left + display_width(tx));
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[y] / [ENTER] Continue there", "[n] Leave, but keep the auto-mark",
                                                "[b] Convert auto-mark to bookmark and leave.", "[ESC] cancel"}, leg_in, leg_bot);
    return lines;
}

void App::book_pick_open(int idx, std::vector<BookStamp> rows) {
    book_->pick_idx = idx;
    book_->pick_rows = std::move(rows);
    book_->pick_sel = 0;
    book_->ov_kind = BookOv::Pick;
    text_disengage();
    mode_ = Mode::BookOverlay;
}

bool App::book_pick_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int n = static_cast<int>(book_->pick_rows.size()) + 1;   // (the last row: the start of the chapter)
    if (arrow && key == 'A') { book_->pick_sel = (book_->pick_sel + n - 1) % n; return true; }
    if (arrow && key == 'B') { book_->pick_sel = (book_->pick_sel + 1) % n; return true; }
    if (arrow) return true;
    int pick = -1;
    if (key >= '1' && key <= '9' && key - '1' < n) pick = key - '1';
    if (key == '\r' || key == '\n') pick = book_->pick_sel;
    if (pick >= 0) {
        mode_ = Mode::Browse;
        if (pick == n - 1) {   // the start of the chapter: the auto-mark of it stays until the playback passes it
            for (const BookStamp& b : book_->pick_rows)
                if (b.kind == 0) { book_->am_hold = true; book_->am_hold_file = b.file; book_->am_hold_sec = b.sec; }
            book_play_at(book_->pick_idx, 0.0);
            return true;
        }
        const BookStamp& b = book_->pick_rows[static_cast<size_t>(pick)];
        BookMark m; m.file = b.file; m.sec = b.sec; m.when = b.when;
        std::string label = b.kind == 0 ? "auto-mark" : b.kind == 1 ? "bookmark " + std::to_string(b.num) : "note " + book_stamp_name(b);
        book_resume_open(book_->pick_idx, m, label);
        return true;
    }
    if (key == 27) {
        mode_ = Mode::Browse;
        if (book_->prompt_paused) player_.resume();
        book_->prompt_paused = false;
    }
    return true;
}

std::vector<std::string> App::build_book_pick_panel(int panel_w) const {
    const int W = panel_w, inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const Framer framed{border + settings_.box_vertical + "\x1b[0m", inner};
    std::vector<std::string> lines;
    lines.push_back(box_top("Time stamps", W, border));
    const std::string what = book_->pick_idx >= 0 && book_->pick_idx < static_cast<int>(chapters_.size())
        ? stem_of(path_utf8(chapters_[static_cast<size_t>(book_->pick_idx)].path.filename())) : std::string();
    const std::string head = truncate_str(what + "  -  where do you want to go on?", inner);
    lines.push_back(framed(header_sgr(settings_) + head, display_width(head)));
    lines.push_back(framed("", 0));
    const std::string plain_ink = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
    const int n = static_cast<int>(book_->pick_rows.size()) + 1;
    const int body = std::clamp(term_rows_ - 14, 3, n);
    int first = std::clamp(book_->pick_sel - body / 2, 0, std::max(0, n - body));
    const int name_w = std::max(10, inner - 4 - 9 - 2 - 12);
    for (int i = first; i < first + body && i < n; ++i) {
        const bool sel = i == book_->pick_sel;
        std::string label;
        if (i == n - 1) label = std::string(i < 9 ? std::to_string(i + 1) : " ") + "  " + "the start of the chapter";
        else {
            const BookStamp& b = book_->pick_rows[static_cast<size_t>(i)];
            label = std::string(i < 9 ? std::to_string(i + 1) : " ") + "  " + pad_right(truncate_str(apply_font_map(book_stamp_name(b), settings_.font_map), name_w), name_w) +
                    "  " + pad_right(clock_text(b.sec), 9) + (b.kind == 2 ? "(note)" : "");
        }
        const std::string row = pad_right(truncate_str((sel ? "> " : "  ") + label, inner), inner);
        lines.push_back(framed((sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) : plain_ink) + row, inner));
    }
    lines.push_back(framed("", 0));
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return framed(border_bottom + std::string(static_cast<size_t>(left), ' ') + tx, left + display_width(tx));
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[UP/DOWN] choose  [ENTER] / [1-9] go there", "[ESC] do not play"}, leg_in, leg_bot);
    return lines;
}

// ---------------------------------------------------------------------
// The overlay of the book mode: tag editor, the two questions, the notes
// ---------------------------------------------------------------------
// SHIFT+s (library), SHIFT+k (time stamps), SHIFT+m (tag editor), SHIFT+l (notes): while one of them is open and nothing is being typed,
// the key of another one switches to it. Returns true when the key was used.
bool App::book_overlay_switch(int key) {
    if (!audiobook_ || last_key_was_arrow()) return false;
    enum Which { None, Library, Stamps, Tags, Notes };
    const int lookup = key == '\r' ? '\n' : key;
    const std::string action = resolve_hotkey_action(lookup);
    Which target = None;
    if (action == "HKeyMetaEditor") target = Tags;
    else if (action == "HKeyListOverlay") target = Notes;
    else if (action == "HKeyQueueOverlay") target = Stamps;
    else if (key == 'S') target = Library;
    if (target == None) return false;
    Which cur = None;
    if (folder_overlay_open_ && mode_ == Mode::Browse) cur = Library;
    else if (mode_ == Mode::BookOverlay) {
        if (book_->ov_kind == BookOv::Marks) cur = Stamps;
        else if (book_->ov_kind == BookOv::Notes) cur = Notes;
        else if (book_->ov_kind == BookOv::Tags) cur = Tags;
    }
    if (cur == None || cur == target) return false;   // (the same key closes the overlay, as before)
    // not while something is typed or asked
    if (cur == Library && fo_->search) return false;
    if (cur == Notes && book_->notes_mode != 0) return false;
    if (cur == Stamps && book_->mmode != 0) return false;
    if (cur == Tags) {
        if (book_->job.active || book_->ov_editing || book_->ov_rename_ask || book_->ov_confirm) return false;
        if (book_ov_dirty()) { book_->ov_status = "unsaved changes: [s] save, [d] discard them first"; return true; }
    }
    if (target == Tags && chapters_.empty()) { status_line_ = "open a book first (ENTER on a book), then SHIFT+m edits the tags of its chapters"; return true; }
    if ((target == Notes) && book_->dir.empty()) { status_line_ = "open a book first (ENTER on a book), then SHIFT+l lists its notes"; return true; }
    if (cur == Library) folder_overlay_close();
    book_->mfilter = 0; book_->notes_sort = 0;
    text_disengage();
    switch (target) {
        case Library: mode_ = Mode::Browse; folder_overlay_open(); break;
        case Stamps: book_marks_open(); break;
        case Tags: book_tags_open(); break;
        case Notes: book_notes_open(); break;
        default: break;
    }
    return true;
}

bool App::book_overlay_key(int key) {
    if (book_overlay_switch(key)) return true;
    switch (book_->ov_kind) {
        case BookOv::Ask: return book_ask_key(key);
        case BookOv::Resume: return book_resume_key(key);
        case BookOv::Notes: return book_notes_key(key);
        case BookOv::Marks: return book_marks_key(key);
        case BookOv::Chapters: return book_cm_key(key);
        case BookOv::DelMark: return book_delmark_key(key);
        case BookOv::Continue: return book_continue_key(key);
        case BookOv::Pick: return book_pick_key(key);
        default: return book_tags_key(key);
    }
}

std::vector<std::string> App::build_book_overlay_panel(int panel_w) const {
    switch (book_->ov_kind) {
        case BookOv::Ask: return build_book_ask_panel(panel_w);
        case BookOv::Resume: return build_book_resume_panel(panel_w);
        case BookOv::Notes: return build_book_notes_panel(panel_w, std::max(14, term_rows_ - 1));   // the whole screen (its last row stays untouched, like every full-screen view)
        case BookOv::Marks: return build_book_marks_panel(panel_w, std::max(14, term_rows_ - 1));
        case BookOv::Chapters: return build_book_cm_panel(panel_w, std::max(14, term_rows_ - 1));
        case BookOv::DelMark: return build_book_delmark_panel(panel_w);
        case BookOv::Continue: return build_book_continue_panel(panel_w);
        case BookOv::Pick: return build_book_pick_panel(panel_w);
        default: return build_book_tags_panel(panel_w);
    }
}

int App::book_ov_width(int term_w) const {
    switch (book_->ov_kind) {
        case BookOv::Ask: case BookOv::Resume: case BookOv::DelMark: case BookOv::Continue: case BookOv::Pick: return std::clamp(term_w - 2, 40, 62);
        case BookOv::Notes: case BookOv::Marks: case BookOv::Chapters: return std::max(40, term_w);   // full width
        default: return std::clamp(term_w - 2, 60, kBookOverlayWidth);
    }
}

// ---------------------------------------------------------------------
// The notes
// ---------------------------------------------------------------------
void App::book_viz_remember() {
    if (book_field_has_note() && book_->viz_before >= 0) return;   // (the note is already there: the view from before is kept)
    book_->viz_before = settings_.element_lyrics ? 3 : settings_.lyric_viz == 1 ? 1 : settings_.lyric_viz == 2 ? 2 : 0;
}

void App::book_viz_restore() {
    if (book_->viz_before < 0) return;
    if (book_->viz_before == 3) settings_.element_lyrics = true;
    else { settings_.element_lyrics = false; settings_.lyric_viz = book_->viz_before; }
    book_->viz_before = -1;
}

void App::book_note_new() {
    if (!has_track_ || !current_.is_local) { status_line_ = "play a chapter first: a note belongs to a point in a chapter"; return; }
    book_viz_remember();
    book_->note_edit = true;
    book_->note_typing = true;
    book_->note_discard_ask = false;
    book_->note_lines.assign(1, std::string());
    book_->note_row = 0;
    book_->note_replace = -1;
    book_->note_file = path_utf8(current_.path.filename());
    book_->note_dir = path_utf8(current_.path.parent_path());
    book_->note_sec = std::floor(player_.poll_elapsed());
    book_->note_status.clear();
    book_->note_shown = false;
    edit_owner_ = "booknote"; book_->note_arow = 0; book_->note_vscroll = 0;
    edit_caret_ = edit_anchor_ = 0;
}

void App::book_note_edit_existing(int idx) {
    if (idx < 0 || idx >= static_cast<int>(book_->notes.size())) return;
    const BookNote& n = book_->notes[static_cast<size_t>(idx)];
    book_viz_remember();
    book_->note_edit = true;
    book_->note_typing = true;
    book_->note_discard_ask = false;
    book_->note_lines.assign(1, n.title);
    for (const std::string& l : n.text) book_->note_lines.push_back(l);
    if (book_->note_lines.size() == 1) book_->note_lines.emplace_back();
    book_->note_row = 0;
    book_->note_replace = idx;
    book_->note_file = n.file;
    book_->note_dir = book_->dir;
    book_->note_sec = n.sec;
    book_->note_status.clear();
    book_->note_shown = false;
    edit_owner_ = "booknote"; book_->note_arow = 0; book_->note_vscroll = 0;
    edit_caret_ = edit_anchor_ = n.title.size();
}

// CTRL+n in the small field: the note that is written goes on in the big notes overlay (same text, same caret); one that is only shown
// is opened there for editing; with no note at all a new one is started there.
void App::book_note_to_overlay() {
    if (book_->dir.empty()) { status_line_ = "open a book first: the big notes overlay belongs to the open book"; return; }
    if (!book_->note_edit) {
        int idx = -1;
        if (book_->note_shown)
            for (size_t i = 0; i < book_->notes.size(); ++i) if (book_->notes[i] == book_->note_view) { idx = static_cast<int>(i); break; }
        if (idx >= 0) book_note_edit_existing(idx);
        else if (has_track_ && current_.is_local) book_note_new();
        else { status_line_ = "play a chapter first: a note belongs to a point in a chapter"; return; }
    } else if (book_->note_dir != book_->dir) {
        status_line_ = "this note belongs to the book that plays, not to the open one: save it first";
        return;
    }
    book_->ov_kind = BookOv::Notes;
    book_->notes_scroll = 0;
    book_->notes_sel = 0;
    if (book_->note_replace >= 0) {   // the note that is changed stays selected in the list
        const std::vector<int> order = book_notes_order();
        for (size_t i = 0; i < order.size(); ++i) if (order[i] == book_->note_replace) book_->notes_sel = static_cast<int>(i);
    }
    book_->notes_mode = 4;
    book_->ov_status.clear();
    mode_ = Mode::BookOverlay;   // (the text stays engaged: the caret is where it was)
}

void App::book_note_save() {
    BookNote n;
    n.file = book_->note_file;
    n.sec = book_->note_sec;
    n.title = trim_ws(book_->note_lines.empty() ? std::string() : book_->note_lines[0]);
    for (size_t i = 1; i < book_->note_lines.size(); ++i) n.text.push_back(book_->note_lines[i]);
    while (!n.text.empty() && trim_ws(n.text.back()).empty()) n.text.pop_back();
    if (n.title.empty() && n.text.empty()) { book_->note_status = "nothing to save yet"; return; }
    if (n.title.empty()) n.title = truncate_str(trim_ws(n.text.front()), 40);
    const bool open_book = book_->note_dir == book_->dir;
    const fs::path nf = book_notes::path_for(path_from_utf8(book_->note_dir));
    std::vector<BookNote> list;
    if (open_book) list = book_->notes; else book_notes::load(nf, list);
    const long long now_t = epoch_now();
    if (book_->note_replace >= 0 && open_book && book_->note_replace < static_cast<int>(list.size())) {
        const BookNote& old = list[static_cast<size_t>(book_->note_replace)];
        n.set = old.set > 0 ? old.set : now_t;
        n.changed = old == n && old.changed > 0 ? old.changed : now_t;   // (saved without a change: the date stays)
        list[static_cast<size_t>(book_->note_replace)] = n;
    } else { n.set = n.changed = now_t; list.push_back(n); }
    if (!book_notes::save(nf, list)) { book_->note_status = "could not write " + path_utf8(nf.filename()); return; }
    if (open_book) book_->notes = list;
    book_->note_edit = false;
    if (mode_ == Mode::BookOverlay && book_->ov_kind == BookOv::Notes && book_->notes_mode == 4) {   // written in the notes overlay: it stays, on that note
        text_disengage();
        book_->ov_status = "note saved: " + n.title + " (" + stem_of(n.file) + ", " + book_notes::time_text(n.sec) + ")";
        const std::vector<int> order = book_notes_order();
        for (size_t i = 0; i < order.size(); ++i) if (book_->notes[static_cast<size_t>(order[i])] == n) book_->notes_sel = static_cast<int>(i);
        return;
    }
    status_line_ = "note saved: " + n.title + " (" + stem_of(n.file) + ", " + book_notes::time_text(n.sec) + ") in " + path_utf8(nf.filename());
    book_note_show(n, false);
}

void App::book_note_show(const BookNote& n, bool automatic) {
    book_viz_remember();
    book_->note_view = n;
    book_->note_shown = true;
    book_->note_auto = automatic;
    book_->note_auto_at = std::chrono::steady_clock::now();
}

// ---------------------------------------------------------------------
// The text of a note: lines (line 0 = the title, the rest paragraphs), a caret and the other end of the marked text, each as
// (line, byte in the line). The paragraphs are wrapped visually only: nothing in the text changes.
// ---------------------------------------------------------------------
namespace {
struct NoteCur { int row = 0; size_t col = 0, acol = 0; int arow = 0; };
typedef std::vector<std::pair<size_t, size_t>> NoteSegs;   // [start, end) in bytes of the visual rows of one paragraph

// The visual rows of a paragraph for `w` columns: break after the last blank that fits, else inside the word.
NoteSegs note_segments(const std::string& s, int w) {
    NoteSegs seg;
    w = std::max(4, w);
    const std::vector<size_t> cuts = utf8_cuts(s);
    size_t j = 0;   // index into cuts of the row's first character
    if (s.empty()) { seg.push_back({0, 0}); return seg; }
    while (j + 1 < cuts.size()) {
        int cols = 0;
        size_t k = j, last_blank = 0;   // last_blank: index after the last blank seen
        bool fits_all = true;
        for (; k + 1 < cuts.size(); ++k) {
            const int cw = display_width(s.substr(cuts[k], cuts[k + 1] - cuts[k]));
            if (cols + cw > w) { fits_all = false; break; }
            cols += cw;
            if (s[cuts[k]] == ' ') last_blank = k + 1;
        }
        size_t endk = k;
        if (!fits_all) {
            if (last_blank > j) endk = last_blank;
            else if (k == j) endk = j + 1;   // one character wider than the row
        }
        seg.push_back({cuts[j], cuts[endk]});
        j = endk;
    }
    return seg;
}
size_t note_seg_of(const NoteSegs& seg, size_t c) {
    size_t at = 0;
    for (size_t i = 0; i < seg.size(); ++i) if (seg[i].first <= c) at = i;
    return at;
}
bool note_has_sel(const NoteCur& c) { return c.row != c.arow || c.col != c.acol; }
void note_order(const NoteCur& c, int& r1, size_t& c1, int& r2, size_t& c2) {
    if (c.arow < c.row || (c.arow == c.row && c.acol <= c.col)) { r1 = c.arow; c1 = c.acol; r2 = c.row; c2 = c.col; }
    else { r1 = c.row; c1 = c.col; r2 = c.arow; c2 = c.acol; }
}
// the marked range inside line r: [a, b) (a == b: nothing of it is marked)
void note_sel_in_row(const NoteCur& c, int r, size_t len, size_t& a, size_t& b) {
    int r1, r2; size_t c1, c2;
    note_order(c, r1, c1, r2, c2);
    a = b = 0;
    if (r < r1 || r > r2 || (r1 == r2 && c1 == c2)) return;
    a = r == r1 ? std::min(c1, len) : 0;
    b = r == r2 ? std::min(c2, len) : len;
}
std::string note_sel_text(const std::vector<std::string>& L, const NoteCur& c) {
    int r1, r2; size_t c1, c2;
    note_order(c, r1, c1, r2, c2);
    std::string out;
    for (int r = r1; r <= r2 && r < static_cast<int>(L.size()); ++r) {
        const std::string& l = L[static_cast<size_t>(r)];
        const size_t a = r == r1 ? std::min(c1, l.size()) : 0, b = r == r2 ? std::min(c2, l.size()) : l.size();
        if (r > r1) out += '\n';
        out += l.substr(a, b - a);
    }
    return out;
}
void note_erase_sel(std::vector<std::string>& L, NoteCur& c) {
    if (!note_has_sel(c)) return;
    int r1, r2; size_t c1, c2;
    note_order(c, r1, c1, r2, c2);
    std::string& first = L[static_cast<size_t>(r1)];
    const std::string tail = L[static_cast<size_t>(r2)].substr(std::min(c2, L[static_cast<size_t>(r2)].size()));
    first.resize(std::min(c1, first.size()));
    first += tail;
    if (r1 == 0 && first.size() > 120) first.resize(utf8_prev_cut(first, 121));   // the title is short
    const size_t first_len = first.size();   // (taken before the erase: the rows after r1 go, `first` itself stays)
    L.erase(L.begin() + r1 + 1, L.begin() + r2 + 1);
    c.row = c.arow = r1;
    c.col = c.acol = std::min(c1, first_len);
}
void note_insert(std::vector<std::string>& L, NoteCur& c, const std::string& text) {
    note_erase_sel(L, c);
    std::vector<std::string> piece(1);
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == '\r') { if (i + 1 < text.size() && text[i + 1] == '\n') ++i; piece.emplace_back(); }
        else if (ch == '\n') piece.emplace_back();
        else if (ch == '\t') piece.back() += ' ';
        else if (ch >= 32 && ch != 127) piece.back().push_back(static_cast<char>(ch));
    }
    std::string& cur = L[static_cast<size_t>(c.row)];
    c.col = std::min(c.col, cur.size());
    if (c.row == 0) {   // the title is one line: the pieces are joined by blanks
        std::string one;
        for (size_t i = 0; i < piece.size(); ++i) one += (i ? " " : "") + piece[i];
        if (cur.size() + one.size() > 120) one.resize(std::min(one.size(), cur.size() >= 120 ? size_t(0) : 120 - cur.size()));
        while (!one.empty() && (static_cast<unsigned char>(one.back()) & 0xC0) == 0x80) one.pop_back();   // (not in the middle of a character)
        cur.insert(c.col, one);
        c.col += one.size();
    } else {
        const std::string post = cur.substr(c.col);
        cur.resize(c.col);
        cur += piece[0];
        if (piece.size() == 1) { c.col = cur.size(); cur += post; }
        else {
            int at = c.row;
            for (size_t i = 1; i < piece.size() && L.size() < 300; ++i) { L.insert(L.begin() + (++at), piece[i]); }
            c.row = at;
            c.col = L[static_cast<size_t>(at)].size();
            L[static_cast<size_t>(at)] += post;
        }
    }
    c.arow = c.row; c.acol = c.col;
}
}  // namespace

// The note editor: every key belongs to it while it is open (the audio goes on).
bool App::book_note_key(int key) {
    const bool arrow = last_key_was_arrow();
    auto& L = book_->note_lines;
    auto close_editor = [&]() { book_->note_edit = false; book_->note_discard_ask = false; text_disengage(); if (!book_->note_shown) book_viz_restore(); };
    if (book_->note_discard_ask) {
        if (!arrow && (key == 'y' || key == 'Y')) close_editor();
        else if (!arrow && (key == 'n' || key == 'N' || key == 27 || key == '\r' || key == '\n')) book_->note_discard_ask = false;
        return true;
    }
    if (!book_->note_typing) {   // the keys do not type: SHIFT+s saves, ENTER types again, ESC lets go of the note
        if (!arrow && key == 'S') { book_note_save(); return true; }
        if (!arrow && (key == '\r' || key == '\n')) { book_->note_typing = true; return true; }
        if (!arrow && key == 27) {
            bool empty = true;
            for (const std::string& l : L) if (!trim_ws(l).empty()) empty = false;
            if (empty) close_editor(); else book_->note_discard_ask = true;
        }
        return true;
    }
    if (key == kKeyCtrlShiftS) { book_note_save(); return true; }
    if (!arrow && key == 27) { book_->note_typing = false; return true; }
    if (L.empty()) L.emplace_back();
    if (edit_owner_ != "booknote") {   // another field had the caret: start at the end of the line
        edit_owner_ = "booknote";
        edit_caret_ = edit_anchor_ = L[static_cast<size_t>(std::clamp(book_->note_row, 0, static_cast<int>(L.size()) - 1))].size();
        book_->note_arow = book_->note_row;
    }
    NoteCur c;
    c.row = std::clamp(book_->note_row, 0, static_cast<int>(L.size()) - 1);
    c.arow = std::clamp(book_->note_arow, 0, static_cast<int>(L.size()) - 1);
    c.col = std::min(edit_caret_, L[static_cast<size_t>(c.row)].size());
    c.acol = std::min(edit_anchor_, L[static_cast<size_t>(c.arow)].size());
    const int last = static_cast<int>(L.size()) - 1;
    auto line = [&](int r) -> std::string& { return L[static_cast<size_t>(r)]; };
    auto done = [&]() { book_->note_row = c.row; book_->note_arow = c.arow; edit_caret_ = c.col; edit_anchor_ = c.acol; return true; };
    auto collapse = [&](bool shift) { if (!shift) { c.arow = c.row; c.acol = c.col; } };
    auto left = [&]() {
        if (c.col > 0) c.col = utf8_prev_cut(line(c.row), c.col);
        else if (c.row > 0) { --c.row; c.col = line(c.row).size(); }
    };
    auto right = [&]() {
        if (c.col < line(c.row).size()) c.col = utf8_next_cut(line(c.row), c.col);
        else if (c.row < last) { ++c.row; c.col = 0; }
    };
    auto word_left = [&]() {
        if (c.col > 0) c.col = le_word_left(line(c.row), c.col);
        else if (c.row > 0) { --c.row; c.col = line(c.row).size(); }
    };
    auto word_right = [&]() {
        if (c.col < line(c.row).size()) c.col = le_word_right(line(c.row), c.col);
        else if (c.row < last) { ++c.row; c.col = 0; }
    };
    // Up / Down: one visual row (the wrapped rows of a paragraph count), the column (in screen cells) kept as well as it goes
    auto vertical = [&](int dir) {
        const int w = std::max(4, book_->note_wrap_w);
        const NoteSegs seg = c.row == 0 ? NoteSegs{{0, line(0).size()}} : note_segments(line(c.row), w);
        const size_t si = note_seg_of(seg, c.col);
        const int want = display_width(line(c.row).substr(seg[si].first, c.col - seg[si].first));
        int row = c.row;
        size_t to = si;
        NoteSegs target = seg;
        if (dir < 0) {
            if (si > 0) to = si - 1;
            else if (row > 0) { --row; target = row == 0 ? NoteSegs{{0, line(0).size()}} : note_segments(line(row), w); to = target.size() - 1; }
            else { c.col = 0; return; }
        } else {
            if (si + 1 < seg.size()) to = si + 1;
            else if (row < last) { ++row; target = note_segments(line(row), w); to = 0; }
            else { c.col = line(row).size(); return; }
        }
        const size_t a = target[to].first, b = target[to].second;
        const std::string& l = line(row);
        size_t off = a;
        int cols = 0;
        for (const size_t cut : utf8_cuts(l.substr(a, b - a))) {
            const int cw = cut == 0 ? 0 : display_width(l.substr(a, cut));
            if (cw > want) break;
            cols = cw; off = a + cut;
        }
        (void)cols;
        // a wrapped row that ends at a blank: the caret stays before the next row's first character
        if (to + 1 < target.size() && off >= b) off = utf8_prev_cut(l, b);
        c.row = row; c.col = off;
    };
    if (arrow && last_key_was_page() && (key == 'A' || key == 'B')) {   // PgUp / PgDn: a page of rows (SHIFT+arrow marks instead)
        for (int i = 0; i < 10; ++i) vertical(key == 'A' ? -1 : +1);
        collapse(false);
        return done();
    }
    const bool shift_arrow = arrow && last_key_was_shifted();
    // the keys that move the caret (SHIFT keeps the other end where it was: that marks)
    if (arrow && key == 'A') { vertical(-1); collapse(shift_arrow); return done(); }
    if (arrow && key == 'B') { vertical(+1); collapse(shift_arrow); return done(); }
    if (arrow && key == 'D') {
        if (note_has_sel(c)) { int r1, r2; size_t c1, c2; note_order(c, r1, c1, r2, c2); c.row = r1; c.col = c1; }
        else left();
        collapse(false); return done();
    }
    if (arrow && key == 'C') {
        if (note_has_sel(c)) { int r1, r2; size_t c1, c2; note_order(c, r1, c1, r2, c2); c.row = r2; c.col = c2; }
        else right();
        collapse(false); return done();
    }
    if (key == kKeyShiftLeft) { left(); return done(); }
    if (key == kKeyShiftRight) { right(); return done(); }
    if (key == kKeyCtrlLeft || key == kKeyAltLeft) { word_left(); collapse(false); return done(); }
    if (key == kKeyCtrlRight || key == kKeyAltRight) { word_right(); collapse(false); return done(); }
    if (key == kKeyCtrlShiftLeft) { word_left(); return done(); }
    if (key == kKeyCtrlShiftRight) { word_right(); return done(); }
    if (key == kKeyHome) { c.col = 0; collapse(last_key_was_shifted()); return done(); }
    if (key == kKeyEnd) { c.col = line(c.row).size(); collapse(last_key_was_shifted()); return done(); }
    if (key == 1) { c.arow = 0; c.acol = 0; c.row = last; c.col = line(last).size(); return done(); }   // CTRL+a: mark everything
    // copy / cut / paste
    if (key == kKeyCtrlC) {
        std::string t;
        if (note_has_sel(c)) t = note_sel_text(L, c);
        else for (size_t i = 1; i < L.size(); ++i) t += (i > 1 ? "\n" : "") + L[i];   // nothing marked: the whole text
        clipboard_set(t);
        book_->note_status = t.empty() ? "nothing to copy" : (note_has_sel(c) ? "COPIED" : "COPIED the whole text");
        return done();
    }
    if (key == kKeyCtrlX) {
        if (!note_has_sel(c)) { book_->note_status = "nothing marked to cut"; return done(); }
        clipboard_set(note_sel_text(L, c));
        note_erase_sel(L, c);
        book_->note_status = "CUT";
        return done();
    }
    if (key == kKeyCtrlV) { note_insert(L, c, clipboard_get()); book_->note_status = "PASTED"; return done(); }
    book_->note_status.clear();
    if (key == kKeyDelete) {
        if (note_has_sel(c)) note_erase_sel(L, c);
        else if (c.col < line(c.row).size()) { const size_t n = utf8_next_cut(line(c.row), c.col); line(c.row).erase(c.col, n - c.col); c.acol = c.col; }
        else if (c.row >= 1 && c.row < last) { line(c.row) += line(c.row + 1); L.erase(L.begin() + c.row + 1); }
        return done();
    }
    if (key == 127 || key == 8) {
        if (note_has_sel(c)) note_erase_sel(L, c);
        else if (c.col > 0) { const size_t p = utf8_prev_cut(line(c.row), c.col); line(c.row).erase(p, c.col - p); c.col = c.acol = p; }
        else if (c.row >= 2) {   // joins the line with the one above
            const size_t at = line(c.row - 1).size();
            line(c.row - 1) += line(c.row);
            L.erase(L.begin() + c.row);
            --c.row; c.col = at; c.arow = c.row; c.acol = c.col;
        }
        return done();
    }
    if (!arrow && (key == '\r' || key == '\n')) {
        note_erase_sel(L, c);
        if (c.row == 0) {   // the title is one line: on to the text
            if (L.size() == 1) L.emplace_back();
            c.row = 1; c.col = 0;
        } else if (L.size() < 300) {   // a new line, with what stood behind the caret
            const size_t at = std::min(c.col, line(c.row).size());
            const std::string rest = line(c.row).substr(at);
            line(c.row).resize(at);
            L.insert(L.begin() + c.row + 1, rest);
            ++c.row; c.col = 0;
        }
        c.arow = c.row; c.acol = c.col;
        return done();
    }
    if (is_text_key(key)) {
        if (note_has_sel(c)) note_erase_sel(L, c);
        std::string& cur = line(c.row);
        c.col = std::min(c.col, cur.size());
        if (cur.size() < (c.row == 0 ? 120u : 4000u)) { cur.insert(c.col, 1, static_cast<char>(key)); ++c.col; }
        c.arow = c.row; c.acol = c.col;
        return done();
    }
    return done();
}

// "chapter  @ 0:00:05" for `width` columns: the time stamp is always whole, the chapter name gives way (cut with "...", or left out
// when even a short piece does not fit).
std::string App::book_note_where(const std::string& file, double sec, int width) const {
    const std::string at = "@ " + book_notes::time_text(sec);
    const std::string stem = stem_of(file);
    if (display_width(stem) + 2 + display_width(at) <= width) return stem + "  " + at;
    const int room = width - 2 - display_width(at);
    if (room >= 4) return truncate_str(stem, room) + "  " + at;
    return at;
}

// The field: the editor, or the note that is shown. `w` x `h` cells, every row exactly w columns.
std::vector<std::string> App::book_note_field_rows(int w, int h) const {
    const std::string R = "\x1b[0m";
    std::vector<std::string> rows;
    auto put = [&](const std::string& sgr, const std::string& plain) {
        const std::string t = truncate_str(plain, w);
        rows.push_back(sgr + t + std::string(static_cast<size_t>(std::max(0, w - display_width(t))), ' ') + R);
    };
    const std::string head = header_sgr(settings_), leg = legend_sgr(settings_), ink = ansi_for(settings_.list_color, false);
    const std::string wfile = book_->note_edit ? book_->note_file : book_->note_view.file;
    const double wsec = book_->note_edit ? book_->note_sec : book_->note_view.sec;
    if (book_->note_edit) {
        {   // the header: what it is + chapter and time (the time always whole); the hint about the time only where there is room
            std::string kind = book_->note_replace >= 0 ? "EDIT NOTE  " : "NEW NOTE  ";
            if (w < display_width(kind) + 14) kind = book_->note_replace >= 0 ? "EDIT  " : "NEW  ";
            const std::string hint = book_->note_replace >= 0 ? "" : "  (time set when SHIFT+n was pressed)";
            std::string where = book_note_where(wfile, wsec, w - display_width(kind));
            if (!hint.empty() && display_width(kind) + display_width(where) + display_width(hint) <= w && where.find("...") == std::string::npos) where += hint;
            put(head, kind + where);
        }
        const int body = std::max(1, h - 5);   // header, title, and three rows for the legend / status (the legend wraps)
        // the caret and the marked text, as the keys see them
        NoteCur cur;
        const bool engaged = book_->note_typing && !book_->note_discard_ask;
        const int nl = static_cast<int>(book_->note_lines.size());
        cur.row = std::clamp(book_->note_row, 0, nl - 1);
        cur.arow = std::clamp(book_->note_arow, 0, nl - 1);
        cur.col = std::min(edit_owner_ == "booknote" ? edit_caret_ : book_->note_lines[static_cast<size_t>(cur.row)].size(), book_->note_lines[static_cast<size_t>(cur.row)].size());
        cur.acol = std::min(edit_owner_ == "booknote" ? edit_anchor_ : cur.col, book_->note_lines[static_cast<size_t>(cur.arow)].size());
        if (edit_owner_ != "booknote") { cur.arow = cur.row; cur.acol = cur.col; }
        // the title: one line that scrolls sideways
        {
            const std::string& text = book_->note_lines[0];
            const std::string prefix = "Title: ";
            size_t sa = 0, sb = 0;
            note_sel_in_row(cur, 0, text.size(), sa, sb);
            const int pw = display_width(prefix);
            size_t cc = cur.row == 0 ? cur.col : sb, aa = cur.row == 0 ? (cur.arow == 0 ? cur.acol : text.size()) : sa;
            if (cur.row != 0 && sa == sb) cc = aa = 0;
            const bool here = engaged && cur.row == 0;
            const EditPaint pt = paint_edit_field(text, cc, aa, std::max(1, w - pw), change_sgr(settings_), here);
            const std::string base = ink + "\x1b[1m";
            if (!engaged) put(base, prefix + text);
            else rows.push_back(base + prefix + (here ? change_sgr(settings_) : std::string()) + pt.s + R + std::string(static_cast<size_t>(std::max(0, w - pw - pt.cols)), ' '));
        }
        // the text: every paragraph wrapped to the width (nothing is changed in it), scrolled so that the caret row is visible
        book_->note_wrap_w = std::max(4, w - 2);   // (one column is left for the caret at the end of a row, one for the scroll bar)
        struct VRow { int row; size_t a, b, si; };
        std::vector<VRow> vrows;
        int caret_v = 0;
        for (int r = 1; r < nl; ++r) {
            const NoteSegs seg = note_segments(book_->note_lines[static_cast<size_t>(r)], book_->note_wrap_w);
            if (r == cur.row) caret_v = static_cast<int>(vrows.size()) + static_cast<int>(note_seg_of(seg, cur.col));
            for (size_t q = 0; q < seg.size(); ++q) vrows.push_back({r, seg[q].first, seg[q].second, q});
        }
        if (cur.row == 0) caret_v = 0;
        if (caret_v < book_->note_vscroll) book_->note_vscroll = caret_v;
        if (caret_v >= book_->note_vscroll + body) book_->note_vscroll = caret_v - body + 1;
        book_->note_vscroll = std::clamp(book_->note_vscroll, 0, std::max(0, static_cast<int>(vrows.size()) - 1));
        // the scroll bar (last column) when the text is longer than the rows there are
        overlay::Thumb thumb;
        const bool bar = overlay::scroll_thumb(book_->note_vscroll, body, static_cast<int>(vrows.size()), thumb);
        auto bar_cell = [&](int i) -> std::string {   // the last column of row i of the text (empty: no bar)
            if (!bar) return std::string();
            return gradient_ansi(settings_.disk_color, settings_.disk_color_end, body > 1 ? static_cast<float>(i) / static_cast<float>(body - 1) : 0.0f)
                   + (i >= thumb.at && i < thumb.at + thumb.len ? "\u2588" : "\u2591") + R;
        };
        const int fill = w - (bar ? 1 : 0);   // the text rows are this wide, the bar takes the rest
        for (int i = 0; i < body; ++i) {
            const int vi = book_->note_vscroll + i;
            if (vi >= static_cast<int>(vrows.size())) { rows.push_back(ink + std::string(static_cast<size_t>(fill), ' ') + R + bar_cell(i)); continue; }
            const VRow& v = vrows[static_cast<size_t>(vi)];
            const std::string& text = book_->note_lines[static_cast<size_t>(v.row)];
            size_t sa = 0, sb = 0;
            note_sel_in_row(cur, v.row, text.size(), sa, sb);
            const NoteSegs full = note_segments(text, book_->note_wrap_w);
            const bool owns_caret = engaged && v.row == cur.row && note_seg_of(full, cur.col) == v.si;
            std::string out;
            int cols = 0;
            const std::vector<size_t> cuts = utf8_cuts(text);
            for (size_t k = 0; k + 1 < cuts.size(); ++k) {
                const size_t off = cuts[k];
                if (off < v.a) continue;
                if (off >= v.b) break;
                const std::string ch = text.substr(off, cuts[k + 1] - off);
                const int cw = std::max(1, display_width(ch));
                if (owns_caret && off == cur.col) { std::string blk; for (int q = 0; q < cw; ++q) blk += "\u2588"; out += change_sgr(settings_) + blk + R + ink; }
                else if (off >= sa && off < sb) out += "\x1b[7m" + ch + "\x1b[0m" + ink;
                else out += ch;
                cols += cw;
            }
            if (owns_caret && cur.col >= v.b && v.b >= text.size()) { out += change_sgr(settings_) + "\u2588" + R + ink; cols += 1; }
            rows.push_back(ink + out + R + std::string(static_cast<size_t>(std::max(0, fill - cols)), ' ') + bar_cell(i));
        }
        // the legend (wrapped to the width of the field, at most three rows; a status or the question takes the first row)
        std::vector<std::pair<std::string, std::string>> leg_rows;   // (colour, text)
        if (book_->note_discard_ask) {
            leg_rows.push_back({head, "Throw the note away?"});
            leg_rows.push_back({leg, "[y] yes   [n] keep it"});
        }
        else {
            if (!book_->note_status.empty()) leg_rows.push_back({head, book_->note_status});
            const std::string text = book_->note_typing ? "[ENTER] new line  [ESC] stop typing  [CTRL+SHIFT+s] save  [CTRL+n] big editor"
                                                       : "typing is off:  [SHIFT+s] save  [ENTER] type on  [ESC] discard  [CTRL+n] big editor";
            for (const std::string& piece : wrap_words(text, std::max(4, w))) leg_rows.push_back({leg, piece});
        }
        if (leg_rows.size() > 3) leg_rows.resize(3);
        for (size_t i = leg_rows.size(); i < 3; ++i) leg_rows.push_back({leg, std::string()});
        for (const auto& lr : leg_rows) put(lr.first, lr.second);
    } else {
        put(head + "\x1b[1m", book_->note_view.title.empty() ? std::string("(no title)") : book_->note_view.title);
        put(leg, book_note_where(wfile, wsec, w));
        put(ink, "");
        for (const std::string& l : book_->note_view.text) {
            for (const std::string& piece : wrap_words(l.empty() ? std::string(" ") : l, std::max(4, w))) {
                if (static_cast<int>(rows.size()) >= h - 1) break;
                put(ink, piece);
            }
        }
    }
    while (static_cast<int>(rows.size()) < h) rows.push_back(std::string(static_cast<size_t>(w), ' '));
    if (!book_->note_edit) {   // the way out, on the last row
        {
            const std::string out_t = truncate_str(w >= 62 ? "[.] put the note away  [,] write it again  [CTRL+n] big editor" : w >= 44 ? "[.] away  [,] edit again  [CTRL+n] big editor" : "[.] away [,] edit [CTRL+n] big", w);
            rows[static_cast<size_t>(h - 1)] = leg + out_t + std::string(static_cast<size_t>(std::max(0, w - display_width(out_t))), ' ') + R;
        }
    }
    rows.resize(static_cast<size_t>(h));
    return rows;
}

// Render loop: playback went past the time of a note -> the field shows it (it goes away after 20 seconds, or with the chapter).
void App::book_notes_follow() {
    if (!has_track_ || !current_.is_local) return;
    const std::string file = path_utf8(current_.path.filename());
    if (!player_.is_paused()) {
        const double el = player_.poll_elapsed();
        const double prev = book_->prev_elapsed;
        book_->prev_elapsed = el;
        if (el >= prev && el - prev < 3.0 && !book_->note_edit) {   // playing through, not a seek
            for (const BookNote& n : book_->notes)
                if (n.file == file && n.sec > prev && n.sec <= el) { book_note_show(n, true); break; }
        }
    }
    if (book_->note_shown && book_->note_auto && !load_in_progress_.load() && !book_->seek_pending && book_->note_view.file != file) book_->note_shown = false;
    if (book_->note_shown && book_->note_auto &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - book_->note_auto_at).count() > 20.0) book_->note_shown = false;
}

// ---------------------------------------------------------------------
// The notes overlay (SHIFT+l)
// ---------------------------------------------------------------------
std::vector<int> App::book_notes_order() const {
    std::vector<int> idx;
    for (size_t i = 0; i < book_->notes.size(); ++i) idx.push_back(static_cast<int>(i));
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        const BookNote& x = book_->notes[static_cast<size_t>(a)];
        const BookNote& y = book_->notes[static_cast<size_t>(b)];
        int cx = book_chapter_of(x.file), cy = book_chapter_of(y.file);
        if (cx < 0) cx = 1 << 20;
        if (cy < 0) cy = 1 << 20;
        if (book_->notes_sort == 1) {   // SHIFT+t: the recently edited first (notes without a date last, in file order)
            const long long ex = x.changed > 0 ? x.changed : x.set, ey = y.changed > 0 ? y.changed : y.set;
            if (ex != ey) return ex > ey;
            return a > b;
        }
        if (cx != cy) return cx > cy;   // the chapter order, the highest chapter on top, in a chapter the furthest time on top
        return x.sec > y.sec;
    });
    return idx;   // (the bookmark has its own list: SHIFT+k)
}

void App::book_notes_open() {
    if (book_->dir.empty()) { status_line_ = "open a book first (ENTER on a book), then SHIFT+l lists its notes"; return; }
    book_bookmark_load();   // (the files may have been edited meanwhile)
    book_->ov_kind = BookOv::Notes;
    book_->notes_sel = 0;
    book_->notes_scroll = 0;
    book_->notes_mode = 0;
    book_->notes_sort = 0;
    book_->ov_status.clear();
    text_disengage();
    mode_ = Mode::BookOverlay;
}

// The legend of the notes overlay (and of the other book overlays): a small tree table like the legends of the meta editor, with
// the group names in the header colour, the keys in the legend colour and their descriptions in the command colour.
std::vector<LegendRow> App::book_overlay_legend(int panel_w, const std::vector<LegendGroup>& groups, int max_rows) const {
    return layout_legend_tree(groups, std::max(0, panel_w), max_rows);
}

bool App::book_notes_key(int key) {
    const bool arrow = last_key_was_arrow();
    const std::vector<int> order = book_notes_order();
    const int n = static_cast<int>(order.size());
    const fs::path book = path_from_utf8(book_->dir);
    auto say = [&](const std::string& s) { book_->ov_status = s; };
    if (book_->notes_mode == 4) {   // a note is written or changed here, in the right pane
        if (key == kKeyCtrlN && !book_->note_discard_ask) {   // CTRL+n: back to the small field, the note goes on there
            book_->notes_mode = 0;
            mode_ = Mode::Browse;
            return true;
        }
        book_note_key(key);
        if (!book_->note_edit) {   // saved, or thrown away
            book_->notes_mode = 0;
            if (book_->ov_status.empty()) say("");
        }
        return true;
    }
    if (book_->notes_mode == 1 || book_->notes_mode == 2) {   // a path is typed
        if (!arrow && key == 27) { book_->notes_mode = 0; say("cancelled"); return true; }
        if (!arrow && (key == '\r' || key == '\n')) {
            const fs::path p = path_from_utf8(trim_ws(book_->notes_path));
            if (book_->notes_mode == 1) {
                std::error_code ec;
                if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
                say(book_notes::save(p, book_->notes) ? "exported " + std::to_string(book_->notes.size()) + " note" + (book_->notes.size() == 1 ? "" : "s") + " to " + path_utf8(p)
                                                     : "could not write " + path_utf8(p));
            } else {
                std::vector<BookNote> in;
                if (!book_notes::load(p, in)) say("no such file: " + path_utf8(p));
                else {
                    const int added = book_notes::merge(book_->notes, in);
                    if (added > 0 && !book_notes::save(book_notes::path_for(book), book_->notes)) say("could not write the notes file");
                    else say("imported " + std::to_string(added) + " new note" + (added == 1 ? "" : "s") + " (" + std::to_string(static_cast<int>(in.size()) - added) + " already there)");
                }
            }
            book_->notes_mode = 0;
            return true;
        }
        edit_focus("booknotespath", book_->notes_path);
        edit_text_key(book_->notes_path, edit_caret_, edit_anchor_, key, 400, &book_->ov_status);
        return true;
    }
    if (book_->notes_mode == 3) {   // delete?
        if (!arrow && (key == 'y' || key == 'Y')) {
            const int e = book_->notes_sel >= 0 && book_->notes_sel < n ? order[static_cast<size_t>(book_->notes_sel)] : -1;
            if (e >= 0) {
                BookStamp st;
                const BookNote& bn = book_->notes[static_cast<size_t>(e)];
                st.kind = 2; st.dir = book_->dir; st.book = path_utf8(book.filename());
                st.file = bn.file; st.sec = bn.sec; st.title = bn.title; st.note = bn;
                if (book_stamp_remove(st)) say("note deleted  (CTRL+SHIFT+u brings it back)");
                else say("could not delete the note");
                book_->notes_sel = std::max(0, book_->notes_sel - 1);
            }
        }
        book_->notes_mode = 0;
        return true;
    }
    if (!arrow && key == kKeyCtrlShiftU) { say(book_undo_pop()); return true; }
    if (key == kKeyCtrlC) {   // CTRL+c copies the chosen note (title and text)
        if (n > 0 && book_->notes_sel >= 0 && book_->notes_sel < n) {
            const BookNote& bn = book_->notes[static_cast<size_t>(order[static_cast<size_t>(book_->notes_sel)])];
            std::string t = bn.title;
            for (const std::string& l : bn.text) t += "\n" + l;
            clipboard_set(t);
            say("COPIED the note");
        }
        return true;
    }
    if (arrow && key == 'A') { if (n) book_->notes_sel = (book_->notes_sel + n - 1) % n; return true; }
    if (arrow && key == 'B') { if (n) book_->notes_sel = (book_->notes_sel + 1) % n; return true; }
    const int e = (n > 0 && book_->notes_sel >= 0 && book_->notes_sel < n) ? order[static_cast<size_t>(book_->notes_sel)] : -1;   // -1: no note
    if (!arrow && (key == 27 || key == 'q' || key == 'L')) { book_->notes_sort = 0; mode_ = Mode::Browse; return true; }
    if (!arrow && key == 'T') {   // SHIFT+t: chapter order, furthest on top <-> recently edited first
        book_->notes_sort = (book_->notes_sort + 1) % 2;
        if (e >= 0) {   // the cursor stays on the same note
            const std::vector<int> neu = book_notes_order();
            for (size_t i = 0; i < neu.size(); ++i) if (neu[i] == e) { book_->notes_sel = static_cast<int>(i); break; }
        }
        say(book_->notes_sort == 0 ? "sorted by chapter, the highest chapter and the furthest time on top" : "sorted by the last edit, the most recent on top");
        return true;
    }
    if (!arrow && key == 'f') {   // [f]: shows it in the field of the main screen (the overlay closes)
        if (e >= 0) { book_note_show(book_->notes[static_cast<size_t>(e)], false); mode_ = Mode::Browse; }
        return true;
    }
    if (!arrow && key == 'P') {   // plays from there; the overlay stays, the note is in the field once it is closed
        if (e < 0) return true;
        const std::string file = book_->notes[static_cast<size_t>(e)].file;
        const double sec = book_->notes[static_cast<size_t>(e)].sec;
        const int c = book_chapter_of(file);
        if (c < 0) { say("that chapter is not in the book any more"); return true; }
        book_play_at(c, sec);
        book_note_show(book_->notes[static_cast<size_t>(e)], false);
        say("playing " + stem_of(file) + " from " + book_notes::time_text(sec));
        return true;
    }
    if (!arrow && (key == 't' || key == '\r' || key == '\n') && e >= 0) {   // ENTER (or t): edit it here, in the right pane
        book_note_edit_existing(e);
        book_->notes_mode = 4;
        book_->ov_status.clear();
        return true;
    }
    if (!arrow && key == 'n') {   // a new note at the point that plays
        if (!has_track_ || !current_.is_local) { say("play a chapter first: a note belongs to a point in a chapter"); return true; }
        book_note_new();
        book_->notes_mode = 4;
        book_->ov_status.clear();
        return true;
    }
    if (!arrow && (key == 'x' || key == kKeyDelete || key == 127) && e >= 0) { book_->notes_mode = 3; return true; }
    if (!arrow && (key == 'E' || key == 'I')) {
        std::error_code ec;
        const std::string folder = path_utf8(book.filename());
        const fs::path dir = book / "notes";   // an extra folder inside the book's folder (made when the export is written)
        book_->notes_path = path_utf8(dir / path_from_utf8(folder + "_notes_export.txt"));
        book_->notes_mode = key == 'E' ? 1 : 2;
        edit_owner_ = "booknotespath";
        edit_caret_ = edit_anchor_ = book_->notes_path.size();
        say("");
        return true;
    }
    return true;
}

std::vector<std::string> App::build_book_notes_panel(int panel_w, int panel_h) const {
    const int W = panel_w, inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const Framer framed{bar, inner};
    const int left_w = std::clamp(inner * 45 / 100, 30, 60);
    const int right_w = std::max(10, inner - left_w - 3);
    const std::string folder = path_utf8(path_from_utf8(book_->dir).filename());
    std::vector<std::string> lines;
    lines.push_back(box_top("Book notes - " + truncate_str(folder, std::max(8, W - 24)), W, border));
    const std::vector<int> order = book_notes_order();
    const int n = static_cast<int>(order.size());
    const bool editing = book_->notes_mode == 4;
    // the legend: three groups, below the window (the keys that work now)
    std::vector<LegendGroup> groups;
    if (editing) {
        groups = {{"Write", {"[ENTER] New line", "[↑↓] Line", "[CTRL+SHIFT+s] Save", "[CTRL+n] Back to the small field"}},
                  {"Leave", {"[ESC] Stop typing, then [SHIFT+s] Save / [ESC] Discard\001[ESC] Stop typing"}}};
    } else {
        groups = {{"List", {"[↑↓] Select", "[ENTER] Edit", "[SHIFT+t] Sort: chapter / last edit\001Sort", "[ESC] Close"}},
                  {"Note", {"[n] New note", "[f] Show in the field", "[x] Delete", "[SHIFT+p] Play from there\001Play"}},
                  {"File", {"[SHIFT+e] Export all notes\001Export", "[SHIFT+i] Import (only new ones)\001Import"}}};
    }
    const std::vector<LegendRow> lrows = book_overlay_legend(W, groups, 3);
    const int body = std::max(3, panel_h - 4 - static_cast<int>(lrows.size()));   // top, header, status, bottom border, the legend
    int first = std::clamp(book_->notes_scroll, 0, std::max(0, n - body));
    if (book_->notes_sel < first) first = book_->notes_sel;
    if (book_->notes_sel >= first + body) first = book_->notes_sel - body + 1;
    first = std::max(0, first);
    const std::string head = header_sgr(settings_), leg = legend_sgr(settings_);
    const std::string plain_ink = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
    auto row_line = [&](const std::string& left, int left_used, const std::string& right, int right_used) {
        return bar + " " + left + std::string(static_cast<size_t>(std::max(0, left_w - left_used)), ' ') + R + " " + border + settings_.box_vertical + R + " " +
               right + std::string(static_cast<size_t>(std::max(0, right_w - right_used)), ' ') + R + " " + bar;
    };
    {   // header of the two panes
        const std::string l = truncate_str(std::to_string(n) + " note" + (n == 1 ? "" : "s") + "  (chapter, time, title)", left_w);
        lines.push_back(row_line(head + l, display_width(l), leg + (editing ? "THE NOTE (typing)" : "THE NOTE"), editing ? 17 : 8));
    }
    // the right pane: the selected note, or the editor
    std::vector<std::string> rp;   // plain lines (the first is the title)
    std::string rtitle;
    std::vector<std::string> editor;
    if (editing) {
        editor = book_note_field_rows(right_w, body);
    } else if (n > 0 && book_->notes_sel >= 0 && book_->notes_sel < n) {
        const BookNote& nt = book_->notes[static_cast<size_t>(order[static_cast<size_t>(book_->notes_sel)])];
        rtitle = nt.title.empty() ? std::string("(no title)") : nt.title;
        rp.push_back(book_note_where(nt.file, nt.sec, right_w));
        rp.push_back("");
        for (const std::string& l : nt.text) for (const std::string& piece : wrap_words(l.empty() ? std::string(" ") : l, right_w)) rp.push_back(piece);
    } else {
        rtitle = "No notes yet";
        rp.push_back("[n] writes one at the point you are listening to.");
    }
    for (int i = 0; i < body; ++i) {
        const int idx = first + i;
        std::string lcell, rcell;
        int lu = 0, ru = 0;
        if (idx < n) {
            const BookNote& nt = book_->notes[static_cast<size_t>(order[static_cast<size_t>(idx)])];
            const int c = book_chapter_of(nt.file);
            std::string plain = "  " + std::string(c >= 0 ? std::to_string(c + 1) : "?") + "  " + truncate_str(stem_of(nt.file), 16) + "  " + book_notes::time_text(nt.sec) + "  " + nt.title;
            plain = pad_right(truncate_str(plain, left_w), left_w);
            const bool sel = idx == book_->notes_sel;
            lcell = (sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) : plain_ink) + plain;
            lu = left_w;
        }
        if (editing) {
            if (i < static_cast<int>(editor.size())) { rcell = editor[static_cast<size_t>(i)]; ru = right_w; }
        } else if (i == 0 && !rtitle.empty()) { const std::string t = truncate_str(rtitle, right_w); rcell = head + "\x1b[1m" + t; ru = display_width(t); }
        else if (static_cast<size_t>(i) <= rp.size() && i >= 1) { const std::string t = truncate_str(rp[static_cast<size_t>(i - 1)], right_w); rcell = (i == 1 ? leg : ansi_for(settings_.list_color, false)) + t; ru = display_width(t); }
        lines.push_back(row_line(lcell, lu, rcell, ru));
    }
    // the status / question row
    {
        std::string body_s;
        int used = 0;
        if (book_->notes_mode == 1 || book_->notes_mode == 2) {
            const std::string label = book_->notes_mode == 1 ? "Export to: " : "Import from: ";
            size_t c = book_->notes_path.size();
            size_t a = c;
            if (edit_owner_ == "booknotespath") { c = edit_caret_; a = edit_anchor_; }
            const EditPaint p = paint_edit_field(book_->notes_path, c, a, std::max(4, inner - display_width(label)), change_sgr(settings_), true);
            body_s = head + label + change_sgr(settings_) + p.s;
            used = display_width(label) + p.cols;
        } else if (book_->notes_mode == 3) {
            const std::string t = "Delete this note?   [y] yes   [n] no";
            const std::string hb = bg_ansi_for(settings_.header_color);
            body_s = (hb.empty() ? std::string("\x1b[7m") : hb + "\x1b[30m") + pad_right(t, inner);
            used = inner;
        } else {
            const std::string t = truncate_str(book_->ov_status, inner);
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
