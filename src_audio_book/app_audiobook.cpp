// The audio book player: the same engine as the music player (App), with the AUDIO BOOK PATH as its library. Its left pane
// lists the books (folders), the right pane lists the chapters of the open book instead of the queue; next / previous and
// the end of a chapter walk the chapters. Split out of app.cpp; the shared helpers are in app_internal.h.
#include "app.h"
#include "book_state.h"
#include "string_util.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "utf8_util.h"
#include "path_utf8.h"
#include "book_art.h"
#include "picture_formats.h"
#include "miniz.h"
#include "book_cue.h"
#include "process_util.h"
#include "console_log.h"
#include "cover_art.h"   // SizeCache, cover_scaled_rgb()
#include "cover_colour.h"
#include <memory>
#include <algorithm>
#include <set>
#include <thread>
#include <cctype>
#include <chrono>
#include <cmath>
#include <array>
#include <fstream>
#include <future>
#include <map>
#include <sstream>
#include "app_internal.h"

namespace muisc {

namespace {

} // namespace

// "Chapter 2" before "Chapter 10": digit runs compare as numbers, everything else case-insensitively.
bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            size_t si = i, sj = j;
            while (si < a.size() && a[si] == '0') ++si;
            while (sj < b.size() && b[sj] == '0') ++sj;
            size_t ei = si, ej = sj;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
            if (ei - si != ej - sj) return ei - si < ej - sj;
            const int c = a.compare(si, ei - si, b, sj, ej - sj);
            if (c != 0) return c < 0;
            i = ei; j = ej;
            continue;
        }
        const int ca = std::tolower(static_cast<unsigned char>(a[i])), cb = std::tolower(static_cast<unsigned char>(b[j]));
        if (ca != cb) return ca < cb;
        ++i; ++j;
    }
    return (a.size() - i) < (b.size() - j);
}

namespace {



// "Penguin, 2019" -> publisher "Penguin", year "2019". A last part that is not a 4-digit year stays in the publisher.
void split_publisher_year(const std::string& v, std::string& publisher, std::string& year) {
    publisher = trim_ws(v);
    year.clear();
    const size_t c = publisher.rfind(',');
    const std::string tail = trim_ws(c == std::string::npos ? publisher : publisher.substr(c + 1));
    const bool is_year = tail.size() == 4 && std::all_of(tail.begin(), tail.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; });
    if (is_year) { year = tail; publisher = c == std::string::npos ? std::string() : trim_ws(publisher.substr(0, c)); }
}

// A typed file name (without its ending) that will also open on Windows: no path parts or reserved characters. The file ending is
// never part of what is typed: it stays as it is.
std::string clean_file_stem(std::string name) {
    name = trim_ws(name);
    for (char& c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || u < 32) c = '_';
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    return name;
}

// ---- the chapter numbers of the file names ("01 Title" in front, "Title 01" at the end) ----
bool is_sep(char c) { return c == ' ' || c == '.' || c == '_' || c == '-'; }
bool is_dig(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

// "03 Title" -> "03" (the digits, a separator and something after them); "" when the name does not start with a number
std::string front_number(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && is_dig(s[i])) ++i;
    if (i == 0 || i >= s.size() || !is_sep(s[i])) return {};
    size_t j = i;
    while (j < s.size() && is_sep(s[j])) ++j;
    return j < s.size() ? s.substr(0, i) : std::string();
}
std::string strip_front_number(const std::string& s) {
    const std::string n = front_number(s);
    if (n.empty()) return s;
    size_t j = n.size();
    while (j < s.size() && is_sep(s[j])) ++j;
    return s.substr(j);
}
// "Title 03" -> "03"; "" when the name does not end with a number that follows a separator
std::string end_number(const std::string& s) {
    size_t i = s.size();
    while (i > 0 && is_dig(s[i - 1])) --i;
    if (i == s.size() || i == 0 || !is_sep(s[i - 1])) return {};
    size_t j = i;
    while (j > 0 && is_sep(s[j - 1])) --j;
    return j > 0 ? s.substr(i) : std::string();
}
std::string strip_end_number(const std::string& s) {
    const std::string n = end_number(s);
    if (n.empty()) return s;
    size_t j = s.size() - n.size();
    while (j > 0 && is_sep(s[j - 1])) --j;
    return s.substr(0, j);
}
// 0 none, 1 every name starts with its own number, 2 every name ends with its own number (the numbers differ from file to file)
int detect_numbering(const std::vector<std::string>& stems) {
    if (stems.size() < 2) return 0;
    for (int kind = 1; kind <= 2; ++kind) {
        std::set<std::string> seen;
        bool all = true;
        for (const std::string& st : stems) {
            const std::string n = kind == 1 ? front_number(st) : end_number(st);
            if (n.empty() || !seen.insert(n).second) { all = false; break; }
        }
        if (all) return kind;
    }
    return 0;
}

// How two paths are compared for "is this file one of those": Windows does not tell A.mp3 from a.mp3.
std::string path_key(const fs::path& p) {
    std::string k = path_utf8(p);
#ifdef _WIN32
    k = ascii_lower_str(k);
#endif
    return k;
}

} // namespace

// The books: every folder of the library that directly holds audio files (the folder list of the music player's "/f:", kept
// as the left pane for good). Keeps the cursor on the same book across a rescan.
// The books pane walks through the folders like a tree: the level of book_->nav shows its books (a folder with audio files) and its
// containers (a folder that holds other books: ENTER goes in, ".." comes back). A folder that has audio files AND books below
// shows as a container whose first row is its own audio files. While a search is typed the list is flat: every matching book.
void App::book_build_view() {
    const std::vector<FolderSummary> flat = filter_folders(last_folder_query_);
    folder_view_.clear();
    if (!last_folder_query_.empty()) { folder_view_ = flat; return; }
    auto trimmed = [](fs::path p) { p = p.lexically_normal(); if (p.filename().empty()) p = p.parent_path(); return p; };
    const fs::path root = trimmed(path_from_utf8(settings_.audiobook_path));
    const fs::path base = book_->nav.empty() ? root : trimmed(path_from_utf8(book_->nav));
    std::map<std::string, FolderSummary> kids;   // by the first folder name below `base`
    std::vector<FolderSummary> self_rows;
    bool any = false;
    for (const FolderSummary& f : flat) {
        const fs::path rel = trimmed(path_from_utf8(f.path)).lexically_relative(base);
        if (rel.empty() || *rel.begin() == "..") continue;
        any = true;
        if (rel == ".") {
            FolderSummary s = f;
            s.self = true;
            s.parent = "this folder";
            self_rows.push_back(std::move(s));
            continue;
        }
        const fs::path first = *rel.begin();
        FolderSummary& k = kids[path_utf8(first)];
        const bool deeper = std::distance(rel.begin(), rel.end()) > 1;
        if (k.path.empty()) { k.path = deeper ? path_utf8(base / first) : f.path; k.name = path_utf8(first); k.parent = path_utf8(base.filename()); }
        ++k.sub_count;
        if (deeper) { k.container = true; k.path = path_utf8(base / first); }
        else k.track_count = f.track_count;
    }
    if (!any) {   // the library is not below the AUDIO BOOK PATH (or it is not set): the flat list as ever
        book_->nav.clear();
        folder_view_ = flat;
        return;
    }
    if (!book_->nav.empty()) {
        FolderSummary up;
        const fs::path parent = trimmed(base.parent_path());
        up.path = parent == root ? std::string() : path_utf8(parent);
        up.name = "..";
        up.parent = "back";
        up.up = true;
        folder_view_.push_back(std::move(up));
    }
    for (FolderSummary& s : self_rows) folder_view_.push_back(std::move(s));
    std::vector<FolderSummary> rest;
    for (auto& kv : kids) rest.push_back(std::move(kv.second));
    std::stable_sort(rest.begin(), rest.end(), [](const FolderSummary& a, const FolderSummary& b) { return ascii_lower_str(a.name) < ascii_lower_str(b.name); });
    for (FolderSummary& k : rest) folder_view_.push_back(std::move(k));
}

void App::book_refresh_view() {
    std::string keep;
    if (selected_ >= 0 && selected_ < static_cast<int>(folder_view_.size())) keep = folder_view_[static_cast<size_t>(selected_)].path;
    list_source_ = ListSource::Folder;
    book_build_view();
    selected_ = 0;
    for (size_t i = 0; i < folder_view_.size(); ++i) if (folder_view_[i].path == keep) { selected_ = static_cast<int>(i); break; }
    scroll_ = std::max(0, std::min(scroll_, selected_));
    if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
    // the open book may be gone after a rescan
    if (!book_->dir.empty()) {
        bool still = false;
        for (const auto& f : filter_folders(last_folder_query_)) if (f.path == book_->dir) still = true;
        if (!still && last_folder_query_.empty()) { book_->dir.clear(); chapters_.clear(); }
        else if (still) book_load_chapters(book_->dir);
    }
}

// Fills chapters_ with the audio files of the folder, in natural file name order.
void App::book_load_chapters(const std::string& dir) {
    chapters_.clear();
    for (const auto& t : all_local_tracks_) {
        if (path_utf8(t.path.parent_path()) != dir) continue;
        BookChapter c;
        c.path = t.path;
        c.title = t.title;
        chapters_.push_back(std::move(c));
    }
    std::stable_sort(chapters_.begin(), chapters_.end(), [](const BookChapter& a, const BookChapter& b) {
        return natural_less(path_utf8(a.path.filename()), path_utf8(b.path.filename()));
    });
}

int App::book_current_chapter() const {
    if (!has_track_ || !current_.is_local) return -1;
    for (size_t i = 0; i < chapters_.size(); ++i) if (chapters_[i].path == current_.path) return static_cast<int>(i);
    return -1;
}

// ENTER on a book: its chapters fill the right pane and take the focus (the cursor on the chapter that plays, else the first).
void App::book_open(int row) {
    if (row < 0 || row >= static_cast<int>(folder_view_.size())) return;
    const FolderSummary pick = folder_view_[static_cast<size_t>(row)];
    if (pick.up || pick.container) {   // walking through the folders: one level up / down, no book opens
        const std::string from = book_->nav;
        book_->nav = pick.up ? pick.path : pick.path;
        selected_ = 0;
        folder_view_.clear();
        book_build_view();
        selected_ = 0;
        if (pick.up) { for (size_t i = 0; i < folder_view_.size(); ++i) if (folder_view_[i].path == from) { selected_ = static_cast<int>(i); break; } }
        else if (folder_view_.size() > 1 && folder_view_[0].up) selected_ = 1;   // (below the "..")
        scroll_ = std::max(0, selected_ - list_nav_rows() + 1);
        status_line_ = pick.up ? "back" : "folder: " + pick.name + " (" + std::to_string(pick.sub_count) + (pick.sub_count == 1 ? " book)" : " books)");
        return;
    }
    book_->dir = folder_view_[static_cast<size_t>(row)].path;
    book_load_chapters(book_->dir);
    book_bookmark_load();   // the bookmark and the notes of this book
    book_->note_shown = false;
    qst_.focus = true;
    const int cur = book_current_chapter();
    const int mark = book_bookmark_chapter();
    qst_.selected = cur >= 0 ? cur : mark >= 0 ? mark : 0;   // the cursor: the chapter that plays, else the one with the bookmark
    qst_.scroll = 0;
    book_fit_scroll();
    status_line_ = "book: " + path_utf8(path_from_utf8(book_->dir).filename()) + " (" + std::to_string(chapters_.size()) +
                   (chapters_.size() == 1 ? " chapter)" : " chapters)");
}

void App::book_fit_scroll() {
    const int n = static_cast<int>(chapters_.size());
    qst_.selected = n == 0 ? 0 : std::clamp(qst_.selected, 0, n - 1);
    const int rows = std::max(1, list_nav_rows());
    if (qst_.selected < qst_.scroll) qst_.scroll = qst_.selected;
    if (qst_.selected >= qst_.scroll + rows) qst_.scroll = qst_.selected - rows + 1;
    qst_.scroll = std::clamp(qst_.scroll, 0, std::max(0, n - rows));
}

void App::book_play_chapter(int idx) {
    if (idx < 0 || idx >= static_cast<int>(chapters_.size())) return;
    const BookChapter& c = chapters_[static_cast<size_t>(idx)];
    LocalTrack t;
    t.title = c.title;
    t.path = c.path;
    qst_.selected = idx;
    book_fit_scroll();
    start_local_track(t);
}

// n / b: the next / previous chapter of the book that plays (or is open).
void App::book_step(int delta, bool direct) {
    if (chapters_.empty()) { status_line_ = "no book open (ENTER on a book opens its chapters)"; return; }
    int base = book_current_chapter();
    if (base < 0) base = std::clamp(qst_.selected, 0, static_cast<int>(chapters_.size()) - 1);
    else base += delta;
    if (base < 0 || base >= static_cast<int>(chapters_.size())) {
        status_line_ = delta > 0 ? "that was the last chapter" : "that is the first chapter";
        return;
    }
    if (direct) {   // the remote app cannot answer questions on the computer: the auto-mark is kept, the chapter plays from its start
        book_automark_tick(true);
        book_->prompt_paused = false;
        book_play_chapter(base);
        return;
    }
    book_go(base);   // (asks about a bookmark first, and offers the resume points when the chapter carries it)
}

// The chapter that played ended: on to the next one, or the end of the book.
void App::book_advance() {
    const int cur = book_current_chapter();
    if (cur < 0 || cur + 1 >= static_cast<int>(chapters_.size())) {
        has_track_ = false;
        advancing_ = false;
        status_line_ = cur >= 0 ? "end of the book" : "";
        return;
    }
    advancing_ = true;   // (until poll_pending_load() reports back)
    book_play_chapter(cur + 1);
    if (!load_in_progress_.load()) { advancing_ = false; has_track_ = false; }
}

// Seeking: a single press jumps 5 s; held down (the terminal repeats the key) the jumps get longer and longer, up to minutes.
void App::book_speed_set(double speed, bool save) {
    const double r = std::clamp(std::round(speed / Settings::kBookSpeedStep) * Settings::kBookSpeedStep, TimeStretch::kMinSpeed, TimeStretch::kMaxSpeed);
    settings_.book_speed = r;
    player_.set_speed(r);
    status_line_ = "playback speed: " + speed_label(r) + (std::fabs(r - 1.0) < 1e-6 ? " (normal)" : " (the pitch stays)");
    if (save) save_settings(settings_);
    force_redraw_ = true;
}

void App::book_speed_step(int dir) { book_speed_set(settings_.book_speed + dir * Settings::kBookSpeedStep, true); }

void App::book_seek(int dir) {
    if (!has_track_) return;
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    const double gap = std::chrono::duration<double>(now - book_->seek_last).count();
    if (gap > 0.4 || dir != book_->seek_dir) { book_->seek_start = now; book_->seek_dir = dir; book_->seek_run = 0; }
    ++book_->seek_run;
    double step = 5.0;
    if (book_->seek_run > 1) {
        const double held = std::chrono::duration<double>(now - book_->seek_start).count();
        const double speed = 20.0 * (1.0 + 2.0 * held) * (1.0 + 2.0 * held);   // seconds of audio per second, growing with the time held
        step = std::clamp(speed * std::min(gap, 0.2), 5.0, 900.0);
    }
    book_->seek_last = now;
    player_.seek_relative(dir * step);
    seek_flash(dir);   // the << / >> labels light up in BORDER_COLOR BOTTOM (build_progress_panel)
}

// Keys of the audio book player. Called for every Browse key before the music player's own handling; returns true when the key
// was used (or is not available in this mode).
bool App::book_intercept(const std::string& action, int key) {
    // [<-] in the library pane inside a folder: one step up (the ".." row)
    if (last_key_was_arrow() && key == 'D' && !qst_.focus && last_folder_query_.empty() && !book_->nav.empty() && !folder_view_.empty() && folder_view_[0].up) {
        book_open(0);
        return true;
    }
    if (action == "HKeyPlay") {
        if (qst_.focus) { if (!chapters_.empty()) book_go(std::clamp(qst_.selected, 0, static_cast<int>(chapters_.size()) - 1)); }
        else book_open(selected_);
        return true;
    }
    if (action == "HKeyNavigateUp" || action == "HKeyNavigateDown") {
        if (!qst_.focus) return false;   // the books list: the normal folder list handling
        qst_.selected += action == "HKeyNavigateUp" ? -1 : 1;
        book_fit_scroll();
        return true;
    }
    if (action == "HKeyMetaEditor") { book_tags_open(); return true; }   // the tag editor of the audio book player (no fetching)
    if (action == "HKeySwitchBetweenCards") { qst_.focus = !qst_.focus; return true; }
    if (action == "HKeyHistory") { history_open(); return true; }              // the reading history
    if (action == "HKeyRemoveHoveringSongFromQueue") {   // [d] in the chapter pane: delete the bookmark of that chapter (asks first)
        if (qst_.focus && !chapters_.empty()) book_delmark_open(std::clamp(qst_.selected, 0, static_cast<int>(chapters_.size()) - 1));
        else status_line_ = "[d] deletes the bookmark of the chapter chosen in the chapter pane";
        return true;
    }
    if (key == kKeyCtrlC && book_->note_shown && !book_->note_edit) {   // CTRL+c copies the note that is shown (it must not end the program)
        std::string t = book_->note_view.title;
        for (const std::string& l : book_->note_view.text) t += "\n" + l;
        clipboard_set(t);
        status_line_ = "COPIED the note";
        return true;
    }
    if (key == kKeyCtrlN) { book_note_to_overlay(); return true; }               // CTRL+n: the note of the field in the big notes overlay
    if (action == "HKeyBookmark") { book_set_bookmark_now(); return true; }   // SHIFT+b
    if (action == "HKeyToggleMetaOnly") { book_note_new(); return true; }      // SHIFT+n
    if (action == "HKeyListOverlay") { book_notes_open(); return true; }       // SHIFT+l
    if (action == "HKeyQueueOverlay") { book_marks_open(); return true; }   // SHIFT+k: the bookmark of every book
    if (key == 'S') { folder_overlay_open(); return true; }                    // SHIFT+s: the library overlay (search + books with their chapters)
    if (action == "HKeyLyricsTiming") { book_cm_open(); return true; }         // ALT+L: chapter marks and cutting
    if (action == "HKeyQueueToPlaylist") { status_line_ = book_undo_pop(); return true; }   // CTRL+SHIFT+u: bring back a deleted time stamp
    if (action == "HKeyQuit") {   // the question about a bookmark comes first
        book_automark_tick(true);
        auto qk = settings_.hotkeys.find("HKeyQuit");
        const int lookup = key == '\r' ? '\n' : key;
        if (qk == settings_.hotkeys.end() || hotkey_string_to_key(qk->second) != lookup || !book_ask_due()) return false;
        book_ask_then("Quitting Mousiki", [this]() { quit_ = true; });
        return true;
    }
    if (action == "HKeyToggleLyricsBack" && book_->note_shown && !book_->note_edit) {   // "," on a note that is shown: write it again
        for (size_t i = 0; i < book_->notes.size(); ++i)
            if (book_->notes[i] == book_->note_view) { book_note_edit_existing(static_cast<int>(i)); return true; }
        book_->note_shown = false;   // (a note that is not in this book's list any more: nothing to edit)
        book_viz_restore();
        return true;
    }
    if ((action == "HKeyToggleLyrics" || action == "HKeyToggleLyricsBack") && book_field_has_note()) {   // "." puts the note away: the view from before comes back
        book_->note_shown = false;
        book_viz_restore();
        return true;
    }
    if (action == "HKeyPlayNextSong") { book_step(1); return true; }
    if (action == "HKeyPlayPreviousSong") { book_step(-1); return true; }
    if (key == kKeyShiftLeft) { book_speed_step(-1); return true; }    // SHIFT+LEFT / SHIFT+RIGHT: slower / faster (fixed keys)
    if (key == kKeyShiftRight) { book_speed_step(1); return true; }
    if (action == "HKeySeekForward") { book_seek(1); return true; }
    if (action == "HKeySeekBackward") { book_seek(-1); return true; }
    if (action == "HKeyCyclePlayMode" || action == "HKeyCyclePlayModeBack") {   // an audio book only has: on to the next chapter / repeat the chapter / stop after it (j: the other way round)
        const bool back = action == "HKeyCyclePlayModeBack";
        settings_.play_mode = back ? (settings_.play_mode == 0 ? 3 : settings_.play_mode == 1 ? 0 : 1)
                                   : (settings_.play_mode == 0 ? 1 : settings_.play_mode == 1 ? 3 : 0);
        status_line_ = settings_.play_mode == 0 ? "play mode: on to the next chapter" : settings_.play_mode == 1 ? "play mode: repeat the chapter" : "play mode: stop after the chapter";
        return true;
    }
    if (key == 27 && qst_.focus) { qst_.focus = false; return true; }   // ESC in the chapter pane: the focus goes back to the library pane
    if (key == 27 && last_folder_query_.empty() && !book_->nav.empty() && !folder_view_.empty() && folder_view_[0].up) { book_open(0); return true; }   // ESC inside a folder: one step up
    if (key == 27) {   // ESC (library pane): back to the whole list of books
        last_folder_query_.clear();
        book_refresh_view();
        status_line_.clear();
        return true;
    }
    static const char* const blocked[] = {
        "HKeyAddHoveringSongToQueue", "HKeyRemoveHoveringSongFromQueue", "HKeyQueueMoveUp", "HKeyQueueMoveDown", "HKeyQueueMoveTop",
        "HKeyQueueMoveBottom", "HKeyQueueLock", "HKeyQueueAddEnd", "HKeyClearQueue", "HKeyQueueUndoClear",
        "HKeyAddToPlaylist", "HKeyPlaylist", "HKeyBulkAdd", "HKeyDownloadStream",
        "HKeyKaraoke", "HKeyRetryLyrics", "HKeyReadLyrics", "HKeyCycleSortMode",
        "HKeyFilterForFolder", "HKeyClearFilter", "HKeyCreator", "HKeyShuffleNext", "HKeyToggleWaveform",
    };
    for (const char* b : blocked)
        if (action == b) { status_line_ = "not available in the audio book player"; return true; }
    return false;
}

// ---- the cover image of a book: found in the book's folder, decoded by ffmpeg in the background -----------------------------
namespace {
struct CoverData {
    std::string dir;                      // the book folder this belongs to
    int w = 0, h = 0;                     // size of `level`
    std::vector<unsigned char> level;     // grey level 0..255 (never 0 where the cover is: 0 would be transparent)
    std::vector<unsigned char> rgb, alpha;   // the colours (w * h * 3) and the opacity (w * h)
    bool ok = false;
};

// A cover scaled to a size, for the wave (grey) or in its own colours (240 colours, median cut), kept until the size changes.
struct BookCoverScaled {
    std::string key;
    int w = 0, h = 0;
    std::vector<unsigned char> grey;      // wave mode
    std::vector<unsigned char> level, hue;   // colour mode: opacity, palette index
    std::array<std::array<uint8_t, 3>, 256> pal{};
};

// Median cut over a 5-bit-per-channel histogram: the palette (at most `n` colours) and the palette index of every pixel.
void quantize_colours(const std::vector<unsigned char>& rgb, const std::vector<unsigned char>& alpha, int n,
                      std::array<std::array<uint8_t, 3>, 256>& pal, std::vector<unsigned char>& hue) {
    const size_t px = alpha.size();
    std::vector<unsigned int> hist(32768, 0);
    for (size_t i = 0; i < px; ++i)
        if (alpha[i] >= 3) ++hist[(static_cast<size_t>(rgb[i * 3] >> 3) << 10) | (static_cast<size_t>(rgb[i * 3 + 1] >> 3) << 5) | (rgb[i * 3 + 2] >> 3)];
    std::vector<int> bins;
    for (int b = 0; b < 32768; ++b) if (hist[static_cast<size_t>(b)]) bins.push_back(b);
    auto chan = [](int b, int c) { return c == 0 ? (b >> 10) & 31 : c == 1 ? (b >> 5) & 31 : b & 31; };
    std::vector<std::vector<int>> boxes;
    if (!bins.empty()) boxes.push_back(bins);
    while (static_cast<int>(boxes.size()) < n) {
        int best = -1, best_c = 0, best_range = 0;
        for (size_t k = 0; k < boxes.size(); ++k) {
            if (boxes[k].size() < 2) continue;
            for (int c = 0; c < 3; ++c) {
                int lo = 31, hi = 0;
                for (int b : boxes[k]) { lo = std::min(lo, chan(b, c)); hi = std::max(hi, chan(b, c)); }
                if (hi - lo > best_range) { best_range = hi - lo; best = static_cast<int>(k); best_c = c; }
            }
        }
        if (best < 0) break;
        std::vector<int> v = std::move(boxes[static_cast<size_t>(best)]);
        std::sort(v.begin(), v.end(), [&](int a, int b) { return chan(a, best_c) < chan(b, best_c); });
        unsigned long long total = 0, acc = 0;
        for (int b : v) total += hist[static_cast<size_t>(b)];
        size_t cut = 1;
        for (size_t i = 0; i + 1 < v.size(); ++i) { acc += hist[static_cast<size_t>(v[i])]; cut = i + 1; if (acc * 2 >= total) break; }
        boxes[static_cast<size_t>(best)] = std::vector<int>(v.begin(), v.begin() + static_cast<long>(cut));
        boxes.emplace_back(v.begin() + static_cast<long>(cut), v.end());
    }
    std::vector<unsigned char> of_bin(32768, 0);
    for (size_t k = 0; k < boxes.size(); ++k) {
        double sum[3] = {0, 0, 0}, cnt = 0;
        for (int b : boxes[k]) {
            const double c = hist[static_cast<size_t>(b)];
            for (int ch = 0; ch < 3; ++ch) sum[ch] += c * ((chan(b, ch) << 3) | (chan(b, ch) >> 2));
            cnt += c;
            of_bin[static_cast<size_t>(b)] = static_cast<unsigned char>(k);
        }
        for (int ch = 0; ch < 3; ++ch) pal[k][static_cast<size_t>(ch)] = static_cast<uint8_t>(cnt > 0 ? std::lround(sum[ch] / cnt) : 0);
    }
    hue.assign(px, 0);
    for (size_t i = 0; i < px; ++i)
        if (alpha[i] >= 3) hue[i] = of_bin[(static_cast<size_t>(rgb[i * 3] >> 3) << 10) | (static_cast<size_t>(rgb[i * 3 + 1] >> 3) << 5) | (rgb[i * 3 + 2] >> 3)];
}

// The book's cover as the music player's CoverImage (the same pixels), so it can use cover_scaled_rgb() for Cover Colors. Made
// once per cover (two remembered: the open book and the one before).
const CoverImage& book_cover_image(const CoverData& c) {
    static std::array<CoverImage, 2> slot;
    static size_t next = 0;
    const std::string key = "book|" + c.dir + "|" + std::to_string(c.w) + "x" + std::to_string(c.h);
    for (CoverImage& ci : slot) if (ci.key == key) return ci;
    CoverImage& ci = slot[next];
    next = (next + 1) % slot.size();
    ci = CoverImage();
    ci.key = key; ci.dir = c.dir; ci.w = c.w; ci.h = c.h; ci.level = c.level; ci.rgb = c.rgb; ci.alpha = c.alpha; ci.ok = c.ok;
    return ci;
}

// Cover Brightness for the book cover with the gradient wave: a brightness curve, 50 % = the picture as it is, 100 % = much brighter
// (dark parts lifted), 0 % = darker. (The music player's Cover behind Disk looks use it differently: there it lifts the dots.)
std::array<unsigned char, 256> book_brightness_curve(int brightness) {
    std::array<unsigned char, 256> lut{};
    const double gamma = std::pow(2.0, (50 - std::clamp(brightness, 0, 100)) / 50.0);   // 0 % -> 2, 50 % -> 1, 100 % -> 0.5
    for (int v = 0; v < 256; ++v)
        lut[static_cast<size_t>(v)] = v == 0 ? 0 : static_cast<unsigned char>(std::clamp(static_cast<int>(std::lround(255.0 * std::pow(v / 255.0, gamma))), 1, 255));
    return lut;
}

const BookCoverScaled& scaled_cover(const CoverData& c, int max_w, int max_h, bool colour, int contrast) {
    static SizeCache<BookCoverScaled, 4> cache;   // (the terminal's and the phone's picture are different sizes: both stay)
    contrast = std::clamp(contrast, 0, 150);
    const std::string key = c.dir + "|" + std::to_string(max_w) + "x" + std::to_string(max_h) + (colour ? "c" : "g") + std::to_string(contrast);
    // more contrast: values move away from the middle (0 % = unchanged, 100 % = 2.5 times the distance from the middle grey, 150 % = 3.25 times)
    const double factor = 1.0 + contrast / 100.0 * 1.5;
    auto punch = [&](unsigned char v) { return static_cast<unsigned char>(std::clamp(static_cast<int>(std::lround((v - 127.5) * factor + 127.5)), 0, 255)); };
    if (const BookCoverScaled* hit = cache.find(key)) return *hit;
    BookCoverScaled& cs = cache.fresh();
    cs.key = key;
    if (!colour) {
        cs.grey = BookArt::scale_grey(c.level, c.w, c.h, max_w, max_h, cs.w, cs.h);
        if (contrast > 0)
            for (unsigned char& v : cs.grey) if (v) v = std::max<unsigned char>(1, punch(v));
        return cs;
    }
    const size_t n = static_cast<size_t>(c.w) * c.h;
    std::vector<unsigned char> chn[4];
    int ow = 0, oh = 0;
    for (int k = 0; k < 4; ++k) {
        std::vector<unsigned char> plane(n);
        for (size_t i = 0; i < n; ++i) plane[i] = k < 3 ? c.rgb[i * 3 + static_cast<size_t>(k)] : c.alpha[i];
        chn[k] = BookArt::scale_grey(plane, c.w, c.h, max_w, max_h, ow, oh);
    }
    cs.w = ow; cs.h = oh;
    const size_t m = static_cast<size_t>(ow) * oh;
    std::vector<unsigned char> rgb(m * 3);
    for (size_t i = 0; i < m; ++i) for (int k = 0; k < 3; ++k) rgb[i * 3 + static_cast<size_t>(k)] = contrast > 0 ? punch(chn[k][i]) : chn[k][i];
    cs.level = chn[3];
    quantize_colours(rgb, cs.level, 240, cs.pal, cs.hue);
    return cs;
}

// The picture file of a book folder that is the cover: a file named like a cover (cover, folder, front, albumart ...) first, else
// the first picture in the folder (by name). Empty path = none.
fs::path find_cover_file(const fs::path& dir) {
    static const char* const kNames[] = {"cover", "folder", "front", "albumart", "album", "book", "thumb"};
    std::error_code ec;
    auto is_picture = [&](const fs::path& f) {
        std::string ext = path_utf8(f.extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* e : kCoverPictureExtensions) if (ext == e) return true;
        return false;
    };
    std::vector<fs::path> pics;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && is_picture(it->path())) pics.push_back(it->path());
    if (pics.empty()) {   // none beside the audio files: the first picture in a folder below (by path)
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && is_picture(it->path())) pics.push_back(it->path());
        if (pics.empty()) return {};
        std::sort(pics.begin(), pics.end());
        return pics.front();
    }
    std::sort(pics.begin(), pics.end());
    std::string folder = path_utf8(dir.filename());
    for (char& c : folder) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* n : kNames)
        for (const fs::path& f : pics) {
            std::string stem = path_utf8(f.stem());
            for (char& c : stem) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (stem == n) return f;
        }
    for (const fs::path& f : pics) {
        std::string stem = path_utf8(f.stem());
        for (char& c : stem) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!folder.empty() && stem == folder) return f;
    }
    return pics.front();
}

// Why the cover of a book could not be shown ("" = fine or not decided yet); read by book_poll for the status line.
std::mutex g_cover_note_mutex;
std::string g_cover_note, g_cover_note_dir, g_cover_src;   // g_cover_src: the file the shown cover was read from
void set_cover_note(const std::string& dir, const std::string& note) {
    std::lock_guard<std::mutex> lk(g_cover_note_mutex);
    g_cover_note_dir = dir;
    g_cover_note = note;
}

// The first audio file of a book folder (by name): its embedded picture (an mp3's cover art) is the cover when no picture file is there.
fs::path first_audio_file(const fs::path& dir) {
    static const char* const kAudio[] = {".mp3", ".m4a", ".m4b", ".aac", ".flac", ".ogg", ".opus", ".wma", ".wav", ".mp4"};
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = path_utf8(it->path().extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* e : kAudio) if (ext == e) { files.push_back(it->path()); break; }
    }
    if (files.empty()) return {};
    std::sort(files.begin(), files.end());
    return files.front();
}

// ffmpeg writes the picture as one PAM image (a tiny header, then RGBA bytes) at most 640 x 640; the grey level of it is
// 0.15 + 0.85 * luminance, times the alpha, like the standard picture's.
CoverData load_cover(const std::string& dir) {
    CoverData d;
    d.dir = dir;
    fs::path file = find_cover_file(path_from_utf8(dir));
    bool embedded = false;
    if (file.empty()) { file = first_audio_file(path_from_utf8(dir)); embedded = true; }   // no picture file: the audio file's own cover art
    if (file.empty()) { set_cover_note(dir, "book cover: no picture file and no audio file in the folder"); return d; }
    Args cmd = {"ffmpeg", "-v", "error", "-i", path_utf8(file)};
    if (embedded) cmd.push_back("-an");
    cmd.insert(cmd.end(), {"-vf", "scale=w=640:h=640:force_original_aspect_ratio=decrease", "-frames:v", "1",
                           "-f", "image2pipe", "-vcodec", "pam", "-pix_fmt", "rgba", "-"});
    // (read straight from the process: run_capture would write the picture's bytes into the console log)
    std::string o;
    if (std::unique_ptr<ChildProcess> child = spawn_capture(cmd, false)) {
        std::vector<char> buf(1 << 16);
        std::ptrdiff_t n;
        while ((n = child->read(buf.data(), buf.size())) > 0) o.append(buf.data(), static_cast<size_t>(n));
        child->wait();
    }
    if (o.size() < 16 || o.compare(0, 2, "P7") != 0) {
        const std::string why = embedded ? "no picture file in the folder and no cover art in " + path_utf8(file.filename())
                                         : "ffmpeg could not read " + path_utf8(file.filename());
        ConsoleLog::instance().log_basic("book cover: " + why + " (" + path_utf8(file) + ")");
        set_cover_note(dir, "book cover: " + why);
        return d;
    }
    ConsoleLog::instance().log_basic("book cover: using " + path_utf8(file));
    const size_t hdr = o.find("ENDHDR\n");
    if (hdr == std::string::npos) return d;
    int w = 0, h = 0;
    {
        std::istringstream in(o.substr(0, hdr));
        std::string tok;
        while (in >> tok) {
            if (tok == "WIDTH") in >> w;
            else if (tok == "HEIGHT") in >> h;
        }
    }
    const size_t at = hdr + 7;
    if (w < 2 || h < 2 || o.size() < at + static_cast<size_t>(w) * h * 4) return d;
    d.w = w; d.h = h;
    d.level.assign(static_cast<size_t>(w) * h, 0);
    d.rgb.assign(static_cast<size_t>(w) * h * 3, 0);
    d.alpha.assign(static_cast<size_t>(w) * h, 0);
    for (size_t i = 0; i < d.level.size(); ++i) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(o.data()) + at + i * 4;
        const double lum = (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255.0;
        const double v = (0.15 + 0.85 * lum) * (p[3] / 255.0) * 255.0;
        d.rgb[i * 3] = p[0]; d.rgb[i * 3 + 1] = p[1]; d.rgb[i * 3 + 2] = p[2]; d.alpha[i] = p[3];
        d.level[i] = p[3] ? static_cast<unsigned char>(std::clamp(static_cast<int>(std::lround(v)), 1, 255)) : 0;
    }
    d.ok = true;
    set_cover_note(dir, "");
    { std::lock_guard<std::mutex> lk(g_cover_note_mutex); g_cover_src = path_utf8(file.filename()) + (embedded ? " (embedded)" : ""); }
    return d;
}

// The cover of the open book, once it has been decoded (nullptr: no cover, not decoded yet, or Book Cover is off).
const CoverData* current_cover(const std::string& dir) {
    static CoverData have;
    static std::future<CoverData> pending;
    static std::string asked;
    if (dir.empty()) return nullptr;
    if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) { CoverData got = pending.get(); if (got.ok || !have.ok || have.dir != got.dir) have = std::move(got); }
    // (a book whose cover could not be read is tried again a few times: the first try may meet a busy disk or a starting ffmpeg)
    static int tries = 0;
    static std::chrono::steady_clock::time_point last_try{};
    const auto now = std::chrono::steady_clock::now();
    const bool retry = asked == dir && !pending.valid() && !(have.ok && have.dir == dir) && tries < 3 && now - last_try > std::chrono::seconds(4);
    if ((asked != dir || retry) && !pending.valid()) {
        tries = asked == dir ? tries + 1 : 1;
        last_try = now;
        asked = dir;
        pending = std::async(std::launch::async, load_cover, dir);
    }
    return (have.ok && have.dir == dir) ? &have : nullptr;
}
} // namespace

// What the cover needs and how it stands (the status line after Book Cover is switched). Short on purpose: it must fit the line
// next to the setting's name; the full story (path, ffmpeg message) is in the console log (t).
std::string App::book_cover_report() const {
    if (gfxs_.proto == GfxProto::None)
        return "no Sixel/Kitty found (OsciImageProtocol=sixel in config.txt)";
    const std::string proto = gfx_name(gfxs_.proto);
    if (book_->dir.empty()) return proto + " | open a book to see its cover";
    std::string note, nd, src;
    { std::lock_guard<std::mutex> lk(g_cover_note_mutex); note = g_cover_note; nd = g_cover_note_dir; src = g_cover_src; }
    if (!note.empty() && nd == book_->dir) {
        if (note.compare(0, 12, "book cover: ") == 0) note.erase(0, 12);
        if (note.size() > 60) { note.resize(57); note += "..."; }
        return proto + " | no cover: " + note;
    }
    if (current_cover(book_->dir)) {
        std::lock_guard<std::mutex> lk(g_cover_note_mutex);
        return proto + " | cover: " + (g_cover_src.empty() ? std::string("found") : g_cover_src);
    }
    return proto + " | reading the cover ...";
}

// The audio book picture as a real transparent pixel image: the terminal draws it (Kitty graphics or Sixel) and the colour wave
// of the braille picture runs over it diagonally. The picture sits centred in the field of cols x rows cells at column col, row 1;
// it keeps its proportions and is as large as the field allows. Returns false when the picture cannot be an image here (no graphics
// protocol, the setting is off, the field is too small): the caller draws the braille picture then.
// The level of every pixel is the grey value of the picture (an area average of its 1-bit original), the hue the position of
// the pixel in the colour wave: v = (1 - amount) * d + amount * ripple, d = the diagonal (x + y) / (w + h) of the picture,
// the same wave as the braille picture's (the cells are twice as tall as wide there, the pixels are square here).
bool App::book_picture_gfx(int col, int cols, int rows) const {
    if (!audiobook_ || gfxs_.proto == GfxProto::None || cols < 8 || rows < 4) return false;
    int cw0 = 10, ch0 = 20;
    gfx_cell_pixels(settings_.cell_pixels, cw0, ch0);
    cw0 = std::max(4, cw0); ch0 = std::max(8, ch0);
    GfxFrame& g = gfx_;
    const int scale = std::clamp(settings_.image_scale, 1, 3);
    const int cws = std::max(2, cw0 / scale);
    const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
    const int w = cols * cws, h = rows * chs;
    int pw = 0, ph = 0;
    // Book Cover on and the open book has a cover image: that one, scaled to fit and centred with a margin of a row at the top and
    // the bottom and a column at the sides (the standard picture brings its own transparent margin and fills the field)
    const CoverData* cover = settings_.book_cover ? current_cover(book_->dir) : nullptr;
    const bool direct = cover && !settings_.book_cover_wave;   // the cover in its own colours, no wave
    const std::vector<unsigned char>* picp;
    const BookCoverScaled* cs = nullptr;
    int ox, oy;
    // Book Cover on, the book's cover is still being read: nothing yet (not the standard picture, which would then have to be wiped)
    bool waiting = false;
    if (settings_.book_cover && !cover && !book_->dir.empty()) {
        std::string note, nd;
        { std::lock_guard<std::mutex> lk(g_cover_note_mutex); note = g_cover_note; nd = g_cover_note_dir; }
        waiting = !(nd == book_->dir && !note.empty());
    }
    static const std::vector<unsigned char> nothing;
    if (waiting) { picp = &nothing; pw = ph = 0; ox = oy = 0; }
    else if (cover) {
        cs = &scaled_cover(*cover, std::max(8, w - 2 * cws), std::max(8, h - 2 * chs), direct, direct ? 0 : settings_.cover_contrast);   // (contrast only with the gradient wave)
        pw = cs->w; ph = cs->h;
        picp = direct ? &cs->level : &cs->grey;
        ox = (w - pw) / 2; oy = (h - ph) / 2;
    } else {
        picp = &BookArt::picture(w, h, pw, ph);   // fits the field with its own proportions
        ox = std::min((w - pw) / 2 + cws / 2, w - pw); oy = (h - ph) / 2;   // (half a column to the right of the exact middle)
    }
    const std::vector<unsigned char>& pic = *picp;
    static std::string last_src;
    const std::string src = cover ? book_->dir + (direct ? "|c" : "|g") + std::to_string(direct ? 0 : settings_.cover_contrast) + "|" +
                                        std::to_string(settings_.cover_brightness) + "|" + std::to_string(settings_.cover_colors)
                                  : waiting ? std::string("|w") : std::string();
    const bool changed_src = src != last_src;
    last_src = src;
    const bool resized = changed_src || g.w != w || g.h != h || g.level.size() != static_cast<size_t>(w) * h || !g.plain || g.id != 0 || g.col != col || g.cols != cols;
    // Cover Colors with Sixel: a fixed palette, drawn `direct` (see cover_colour.h)
    const bool six_colours = cover && !direct && settings_.cover_colors > 0 && gfxs_.proto == GfxProto::Sixel;
    g.plain = true;
    g.direct = direct || six_colours;
    g.scale = scale;
    g.out_cw = cw0; g.out_ch = ch0;
    g.cols = cols; g.rows = rows; g.w = w; g.h = h;
    g.col = col; g.row = 1;
    g.id = 0; g.grid.clear(); g.cut_w = 0;
    // the pictures per second: the wave is slow, and every Sixel picture is a lot of bytes
    using clock = std::chrono::steady_clock;
    static clock::time_point last_sent{};
    const auto now = clock::now();
    const auto gap = std::chrono::milliseconds(gfxs_.proto == GfxProto::Sixel ? 120 : 50);
    const bool due = resized || now - last_sent >= gap;
    g.active = true;
    g.fresh = due;
    if (!due) return true;
    last_sent = now;
    // the colours: the disk colour -> disk colour end gradient (hue 0..255 along it)
    int sr = 255, sg = 255, sb = 255, er = 255, eg = 255, eb = 255;
    const bool gradient = settings_.disk_color_gradient && !settings_.disk_color_end.empty();
    if (!try_parse_rgb(settings_.disk_color, sr, sg, sb)) { sr = sg = sb = 255; }
    if (gradient) { if (!try_parse_rgb(settings_.disk_color_end, er, eg, eb)) { er = sr; eg = sg; eb = sb; } }
    else { er = sr; eg = sg; eb = sb; }
    if (direct) {   // the cover's own palette; the pixels keep their colour index, the opacity is the level
        g.pal = cs->pal;
        g.level.assign(static_cast<size_t>(w) * h, 0);
        g.hue.assign(static_cast<size_t>(w) * h, 0);
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) {
                const size_t from = static_cast<size_t>(y) * pw + x, at = static_cast<size_t>(y + oy) * w + x + ox;
                g.level[at] = pic[from];
                g.hue[at] = cs->hue[from];
            }
        return true;
    }
    for (int i = 0; i < 256; ++i) {
        const double u = i / 255.0;
        g.pal[static_cast<size_t>(i)] = {static_cast<uint8_t>(std::lround(sr + (er - sr) * u)), static_cast<uint8_t>(std::lround(sg + (eg - sg) * u)),
                                         static_cast<uint8_t>(std::lround(sb + (eb - sb) * u))};
    }
    static const auto t0 = clock::now();
    const double phase = std::chrono::duration<double>(now - t0).count() * 0.15;   // the wave never stops, a little slower than the sign's
    const double waves = 1.5, amount = 0.5;
    g.level.assign(static_cast<size_t>(w) * h, 0);
    g.hue.assign(static_cast<size_t>(w) * h, 0);
    // the ripple only depends on the diagonal: one table of w + h steps
    std::vector<uint8_t> hue_of(static_cast<size_t>(pw + ph), 0);
    for (int k = 0; k < pw + ph; ++k) {
        const double d = k / static_cast<double>(pw + ph - 1);
        const double ripple = 0.5 + 0.5 * std::sin(6.283185307179586 * (d * waves - phase));
        hue_of[static_cast<size_t>(k)] = static_cast<uint8_t>(std::lround(std::clamp((1.0 - amount) * d + amount * ripple, 0.0, 1.0) * 255.0));
    }
    // the book's cover: Cover Brightness (a brightness curve) and Cover Colors (its own colours shine through the gradient)
    const std::array<unsigned char, 256> curve = book_brightness_curve(cover ? settings_.cover_brightness : 50);
    const float tcol = cover ? std::clamp(settings_.cover_colors, 0, 100) / 100.f : 0.f;
    const CoverRgb* crgb = tcol > 0.f ? &cover_scaled_rgb(book_cover_image(*cover), std::max(8, w - 2 * cws), std::max(8, h - 2 * chs), settings_.cover_contrast) : nullptr;
    if (crgb && (crgb->w != pw || crgb->h != ph)) crgb = nullptr;
    const auto grad = g.pal;   // the gradient (hue -> colour)
    if (crgb) g.pal = six_colours ? cube_palette() : hue_sat_palette();
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) {
            const unsigned char v0 = pic[static_cast<size_t>(y) * pw + x];
            if (!v0) continue;
            const unsigned char v = curve[v0];
            const size_t at = static_cast<size_t>(y + oy) * w + x + ox;
            const uint8_t hu = hue_of[static_cast<size_t>(x + y)];
            if (!crgb) {
                g.level[at] = v;   // (grey = brightness; the transparent parts of the picture are 0)
                g.hue[at] = hu;
                continue;
            }
            const size_t si = static_cast<size_t>(y) * pw + x;
            const auto c = mix_colour(grad[hu], v, crgb->rgb[si * 3], crgb->rgb[si * 3 + 1], crgb->rgb[si * 3 + 2], tcol);
            if (six_colours) {
                const int ix = cube_index(c[0], c[1], c[2], x + ox, y + oy);
                if (ix == 0) continue;
                g.level[at] = 255;
                g.hue[at] = static_cast<uint8_t>(ix);
                continue;
            }
            uint8_t idx = 0;
            float lev = 0.f;
            hue_sat_encode(c[0], c[1], c[2], idx, lev);
            if (lev < 3.f) continue;
            g.level[at] = static_cast<unsigned char>(std::min(255.f, lev));
            g.hue[at] = idx;
        }
    return true;
}

// One frame of an audio book animation as terminal lines (braille dots, every cell coloured from a wave gradient). `t` = seconds
// on the animation's own clock. Book: a still picture, its colours move. Stack: builds up, holds for a few seconds, starts over.
// The colours are the disk colour -> disk colour end gradient with a wave travelling over the picture, made like the radio's ON
// AIR sign: the same wave speed (0.25 cycles per second), 1.5 crests over the diagonal and 35 % wave in the mix.
std::vector<std::string> App::book_art_lines(int which, double t) const {
    // Which 0: the audiobook picture (a woman reading a book, a still picture of braille dots: only its colours move). Which 1: the stack
    // of books (builds up, holds ~4 s, starts over) at a quarter of the speed of its Lottie file.
    const BookArt& art = BookArt::get(which == 1 ? BookArt::Which::Stack : BookArt::Which::Book);
    const double fps = art.fps() * 0.25;
    int frame = static_cast<int>(t * fps);
    if (which == 1) {
        const double hold = 4.0;
        const double cycle = art.frames() / fps + hold;
        const double tc = std::fmod(t, cycle);
        frame = std::min(art.frames() - 1, static_cast<int>(tc * fps));
    } else {
        frame %= art.frames();
    }
    std::vector<unsigned char> shade;
    std::vector<unsigned char> cells = art.braille_cells(frame, &shade);
    int cols = art.cols(), rows = art.rows();
    // The frames are a strong-contrast black and white version of the animations; the gradient is put on them: the bright parts
    // (the sparkles, the pages turning) take the full gradient colour, the rest of the picture a darker one. 65 colour steps along
    // the gradient times 8 brightness steps, made when first needed (quantised like the ON AIR sign: fewer colour runs).
    int sr = 0, sg = 0, sb = 0, er = 0, eg = 0, eb = 0;
    const bool gradient = settings_.disk_color_gradient && !settings_.disk_color_end.empty();
    const bool rgb_ok = try_parse_rgb(settings_.disk_color, sr, sg, sb) && (!gradient || try_parse_rgb(settings_.disk_color_end, er, eg, eb));
    if (!gradient) { er = sr; eg = sg; eb = sb; }
    std::vector<std::string> ramp(65 * 8);
    auto colour = [&](int step, int level) -> const std::string& {
        std::string& c = ramp[static_cast<size_t>(step * 8 + level)];
        if (!c.empty()) return c;
        if (!rgb_ok) {   // a colour value that has no RGB: the plain gradient, no darker parts
            c = gradient ? gradient_ansi(settings_.disk_color, settings_.disk_color_end, static_cast<float>(step) / 64.0f) : ansi_for(settings_.disk_color, false);
            return c;
        }
        const double u = step / 64.0, f = level / 7.0;
        char b[40];
        std::snprintf(b, sizeof b, "\x1b[38;2;%d;%d;%dm", static_cast<int>((sr + (er - sr) * u) * f + 0.5), static_cast<int>((sg + (eg - sg) * u) * f + 0.5),
                      static_cast<int>((sb + (eb - sb) * u) * f + 0.5));
        c = b;
        return c;
    };
    static const auto t0 = std::chrono::steady_clock::now();
    const double wave_t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();   // the wave never stops, like the sign's
    const double phase = wave_t * 0.15;
    const double waves = 1.5, amount = 0.5;   // (the wave runs over the diagonal of the picture, from the upper left to the lower right)
    std::vector<std::string> lines;
    for (int r = 0; r < rows; ++r) {
        std::string line, last;
        for (int c = 0; c < cols; ++c) {
            const size_t cell = static_cast<size_t>(r * cols + c);
            const unsigned char b = cells[cell];
            if (!b) { line += ' '; continue; }
            // cells are about twice as tall as wide: x + 2y is a true 45 degree diagonal
            const double d = (c + 2.0 * r) / static_cast<double>(cols - 1 + 2 * (rows - 1));
            const double ripple = 0.5 + 0.5 * std::sin(6.283185307179586 * (d * waves - phase));
            const double v = std::clamp((1.0 - amount) * d + amount * ripple, 0.0, 1.0);
            const double bright = std::max(0.25, std::pow(shade[cell] / 255.0, 1.4));   // white = the full colour, dark grey = a dark one
            const std::string& a = colour(static_cast<int>(std::lround(v * 64.0)), static_cast<int>(std::lround(bright * 7.0)));
            if (a != last) { line += a; last = a; }
            BookArt::append_braille(line, b);
        }
        line += "\x1b[0m";
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::string> App::book_phone_picture(int& cols, int& rows) const {
    const BookArt& art = BookArt::get(BookArt::Which::Book);
    cols = art.cols();
    rows = art.rows();
    const CoverData* cover = settings_.book_cover && !book_->dir.empty() ? current_cover(book_->dir) : nullptr;
    if (!cover) {
        if (settings_.book_cover && !book_->dir.empty()) {   // a cover that is being read: nothing yet; a book without one: the standard picture
            std::lock_guard<std::mutex> lk(g_cover_note_mutex);
            if (g_cover_note.empty() || g_cover_note_dir != book_->dir) return {};
        }
        return book_art_lines(0, book_->anim_t);
    }
    // The cover as braille dots: the dot field is cols*2 x rows*4, a dot is lit where the brightness beats an ordered-dither
    // threshold; a cell takes the average colour of the pixels in it (or, with the wave on, the wave's colour as in the standard picture).
    const bool wave = settings_.book_cover_wave;
    const BookCoverScaled& cs = scaled_cover(*cover, cols * 2, rows * 4, !wave, wave ? settings_.cover_contrast : 0);
    // with the wave: Cover Brightness (a brightness curve) and Cover Colors (the cover's own colours through the gradient)
    const std::array<unsigned char, 256> curve = book_brightness_curve(wave ? settings_.cover_brightness : 50);
    const float tcol = wave ? std::clamp(settings_.cover_colors, 0, 100) / 100.f : 0.f;
    const CoverRgb* crgb = tcol > 0.f ? &cover_scaled_rgb(book_cover_image(*cover), cols * 2, rows * 4, settings_.cover_contrast) : nullptr;
    if (crgb && (crgb->w != cs.w || crgb->h != cs.h)) crgb = nullptr;
    const int fw = cols * 2, fh = rows * 4;
    const int ox = std::max(0, (fw - cs.w) / 2), oy = std::max(0, (fh - cs.h) / 2);
    int sr = 255, sg = 255, sb = 255, er = 255, eg = 255, eb = 255;
    const bool gradient = settings_.disk_color_gradient && !settings_.disk_color_end.empty();
    if (wave) {
        if (!try_parse_rgb(settings_.disk_color, sr, sg, sb)) { sr = sg = sb = 255; }
        er = sr; eg = sg; eb = sb;
        if (gradient && !try_parse_rgb(settings_.disk_color_end, er, eg, eb)) { er = sr; eg = sg; eb = sb; }
    }
    static const auto t0 = std::chrono::steady_clock::now();
    const double phase = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 0.15;
    static const int kBayer[4][2] = {{0, 4}, {6, 2}, {1, 5}, {7, 3}};   // thresholds 0..7 over the 2 x 4 dots of a cell
    static const int kBit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    std::vector<std::string> lines;
    for (int r = 0; r < rows; ++r) {
        std::string line, last;
        for (int c = 0; c < cols; ++c) {
            int bits = 0, n = 0, rs = 0, gs = 0, bs = 0;
            float cr = 0, cg = 0, cb = 0;   // the cover's own colour under the cell (Cover Colors)
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int x = c * 2 + dx - ox, y = r * 4 + dy - oy;
                    if (x < 0 || y < 0 || x >= cs.w || y >= cs.h) continue;
                    const size_t at = static_cast<size_t>(y) * cs.w + x;
                    int lum, pr, pg, pb;
                    if (wave) {
                        lum = cs.grey[at];
                        if (lum == 0) continue;
                        lum = curve[static_cast<size_t>(lum)];
                        pr = pg = pb = lum;
                        if (crgb) { cr += crgb->rgb[at * 3]; cg += crgb->rgb[at * 3 + 1]; cb += crgb->rgb[at * 3 + 2]; }
                    } else {
                        if (cs.level[at] < 128) continue;
                        const auto& p = cs.pal[cs.hue[at]];
                        pr = p[0]; pg = p[1]; pb = p[2];
                        lum = (pr * 30 + pg * 59 + pb * 11) / 100;
                    }
                    ++n; rs += pr; gs += pg; bs += pb;
                    const double lv = std::pow(lum / 255.0, 0.7);   // (a dark cover still shows its shapes)
                    if (lv * 8.0 > kBayer[dy][dx] + 0.5) bits |= kBit[dy][dx];
                }
            if (!bits || !n) { line += ' '; continue; }
            char col[64];
            if (wave) {
                const double d = (c + 2.0 * r) / static_cast<double>(std::max(1, cols - 1 + 2 * (rows - 1)));
                const double ripple = 0.5 + 0.5 * std::sin(6.283185307179586 * (d * 1.5 - phase));
                const double v = std::clamp(0.5 * d + 0.5 * ripple, 0.0, 1.0);
                const double f = std::max(0.25, std::pow(rs / n / 255.0, 1.4));
                if (crgb) {
                    const std::array<uint8_t, 3> base = {static_cast<uint8_t>(std::lround(sr + (er - sr) * v)), static_cast<uint8_t>(std::lround(sg + (eg - sg) * v)),
                                                         static_cast<uint8_t>(std::lround(sb + (eb - sb) * v))};
                    const auto m = mix_colour(base, static_cast<float>(f * 255.0), cr / n, cg / n, cb / n, tcol);
                    std::snprintf(col, sizeof col, "\x1b[38;2;%d;%d;%dm", static_cast<int>(m[0] + 0.5f), static_cast<int>(m[1] + 0.5f), static_cast<int>(m[2] + 0.5f));
                } else
                std::snprintf(col, sizeof col, "\x1b[38;2;%d;%d;%dm", static_cast<int>((sr + (er - sr) * v) * f + 0.5),
                              static_cast<int>((sg + (eg - sg) * v) * f + 0.5), static_cast<int>((sb + (eb - sb) * v) * f + 0.5));
            } else {
                std::snprintf(col, sizeof col, "\x1b[38;2;%d;%d;%dm", rs / n, gs / n, bs / n);
            }
            if (last != col) { line += col; last = col; }
            const unsigned cp = 0x2800u + static_cast<unsigned>(bits);   // (the braille character of the dots)
            line += static_cast<char>(0xE0 | (cp >> 12));
            line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            line += static_cast<char>(0x80 | (cp & 0x3F));
        }
        line += "\x1b[0m";
        lines.push_back(std::move(line));
    }
    return lines;
}

// The remote app's picture: the open book's cover (its own colours, or grey in the disk colours when the wave is on) or the standard
// picture, as a PNG with a transparent background of at most 640 x 640 pixels -- the phone draws it at full resolution where the
// disk would be. `key` names what the picture shows (it changes when the book, the cover or the colours do); the PNG is made once
// per key. nullptr: nothing yet (the cover is still being read).
const std::string* App::book_phone_png(std::string& key) const {
    static std::string cache_key, cache_png;
    const CoverData* cover = settings_.book_cover && !book_->dir.empty() ? current_cover(book_->dir) : nullptr;
    if (!cover && settings_.book_cover && !book_->dir.empty()) {
        std::lock_guard<std::mutex> lk(g_cover_note_mutex);
        if (g_cover_note.empty() || g_cover_note_dir != book_->dir) return nullptr;
    }
    const bool direct = cover && !settings_.book_cover_wave;
    key = (cover ? book_->dir + (direct ? "|c" : "|g") + std::to_string(direct ? 0 : settings_.cover_contrast) + "|" + std::to_string(settings_.cover_brightness) + "|" +
                       std::to_string(settings_.cover_colors) : std::string("standard")) + "|" + settings_.disk_color + "|" +
          (settings_.disk_color_gradient ? settings_.disk_color_end : std::string());
    if (key == cache_key && !cache_png.empty()) return &cache_png;
    int sr = 255, sg = 255, sb = 255, er = 255, eg = 255, eb = 255;
    const bool gradient = settings_.disk_color_gradient && !settings_.disk_color_end.empty();
    if (!try_parse_rgb(settings_.disk_color, sr, sg, sb)) { sr = sg = sb = 255; }
    er = sr; eg = sg; eb = sb;
    if (gradient && !try_parse_rgb(settings_.disk_color_end, er, eg, eb)) { er = sr; eg = sg; eb = sb; }
    int pw = 0, ph = 0;
    const std::vector<unsigned char>* lv = nullptr;
    const BookCoverScaled* cs = nullptr;
    if (cover) {
        cs = &scaled_cover(*cover, 640, 640, direct, direct ? 0 : settings_.cover_contrast);
        pw = cs->w; ph = cs->h;
        lv = direct ? &cs->level : &cs->grey;
    } else {
        lv = &BookArt::picture(640, 640, pw, ph);
    }
    if (pw <= 0 || ph <= 0 || lv->size() < static_cast<size_t>(pw) * ph) return nullptr;
    // the cover with the wave: Cover Brightness / Cover Colors as on the computer
    const std::array<unsigned char, 256> curve = book_brightness_curve(cover && !direct ? settings_.cover_brightness : 50);
    const float tcol = cover && !direct ? std::clamp(settings_.cover_colors, 0, 100) / 100.f : 0.f;
    const CoverRgb* crgb = tcol > 0.f ? &cover_scaled_rgb(book_cover_image(*cover), 640, 640, settings_.cover_contrast) : nullptr;
    if (crgb && (crgb->w != pw || crgb->h != ph)) crgb = nullptr;
    std::vector<unsigned char> rgba(static_cast<size_t>(pw) * ph * 4, 0);
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) {
            const size_t i = static_cast<size_t>(y) * pw + x;
            const unsigned char a = (*lv)[i];
            if (!a) continue;
            unsigned char* o = &rgba[i * 4];
            if (direct) {
                const auto& p = cs->pal[cs->hue[i]];
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
            } else {   // the picture's brightness is its opacity; the colour runs along the diagonal (the wave, standing still)
                const double d = (x + y) / static_cast<double>(std::max(1, pw + ph - 2));
                o[0] = static_cast<unsigned char>(std::lround(sr + (er - sr) * d));
                o[1] = static_cast<unsigned char>(std::lround(sg + (eg - sg) * d));
                o[2] = static_cast<unsigned char>(std::lround(sb + (eb - sb) * d));
                if (crgb) {
                    const auto m = mix_colour({o[0], o[1], o[2]}, 255.f, crgb->rgb[i * 3], crgb->rgb[i * 3 + 1], crgb->rgb[i * 3 + 2], tcol);
                    o[0] = static_cast<unsigned char>(m[0] + 0.5f); o[1] = static_cast<unsigned char>(m[1] + 0.5f); o[2] = static_cast<unsigned char>(m[2] + 0.5f);
                }
            }
            o[3] = direct ? a : curve[a];
        }
    size_t n = 0;
    void* p = tdefl_write_image_to_png_file_in_memory(rgba.data(), pw, ph, 4, &n);
    if (!p) return nullptr;
    cache_png.assign(static_cast<const char*>(p), n);
    mz_free(p);
    cache_key = key;
    return &cache_png;
}

std::vector<std::string> App::build_chapter_panel(int total_width, int height) const {
    const int inner = total_width - 4;
    const std::string border_ansi = ansi_for(settings_.border_color, false);
    const std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;
    std::string title = "CHAPTERS";
    if (!book_->dir.empty()) {
        const std::string name = path_utf8(path_from_utf8(book_->dir).filename());
        const int room = std::max(4, total_width - 14);
        title += " - " + truncate_str(name, room);
    }
    if (qst_.focus) title += focus_indicator(settings_, border_ansi);
    out.push_back(box_top(title, total_width, border_ansi));

    const int n = static_cast<int>(chapters_.size());
    const int playing = book_current_chapter();
    for (int row = 0; row < height; ++row) {
        const int idx = qst_.scroll + row;
        std::string content;
        bool is_playing = false, is_hover = false, is_bookmark = false;
        if (n == 0) {
            if (row == height / 2) {
                const std::string text = apply_font_map("OPEN A BOOK: ENTER ON A FOLDER", settings_.font_map);
                content = std::string(static_cast<size_t>(std::max(0, (inner - display_width(text)) / 2)), ' ') + text;
            }
        } else if (idx < n) {
            const BookChapter& c = chapters_[static_cast<size_t>(idx)];
            std::string dur;
            std::string name = c.title;
            {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto it = row_meta_cache_.find(path_utf8(c.path));
                if (it != row_meta_cache_.end()) {
                    if (it->second.duration_sec > 0) dur = clock_text(it->second.duration_sec);
                    if (!it->second.title.empty()) name = it->second.title;   // the "Chapter" tag, when the file has one
                }
            }
            const int idx_w = 3, dur_w = 8;
            const int title_w = std::max(5, inner - idx_w - dur_w - 2 * (display_width(settings_.list_separator) + 1));
            is_playing = idx == playing;
            is_hover = qst_.focus && idx == qst_.selected;
            const bool marked = book_mark_in_chapter(idx) >= 0;   // a chapter with a bookmark is marked; the one with the newest also gets the header colour
            if (marked) is_bookmark = idx == book_bookmark_chapter();
            std::string shown_name = apply_font_map(name, settings_.font_map);
            if (marked) shown_name = "\u25c6 " + shown_name;
            // the chapter name scrolls when it is too long (Settings -> GENERAL -> Marquee Scrolling): the chosen row, or while the pane
            // has no cursor the chapter that plays
            const bool scroll_row = is_hover || (!qst_.focus && is_playing);
            const std::string name_cell = scroll_row ? marquee_or_truncate(shown_name, title_w, idx, marquee_.ch_idx, marquee_.ch_since)
                                                     : pad_right(truncate_str(shown_name, title_w), title_w);
            content = pad_right(apply_font_map(std::to_string(idx + 1), settings_.font_map), idx_w) + settings_.list_separator + " " +
                      name_cell + settings_.list_separator + " " + pad_right(dur, dur_w);
        }
        const std::string padded = pad_right(truncate_str(content, inner), inner);
        std::string color_ansi;
        if (is_hover) color_ansi = cursor_sgr(settings_.queue_cursor_color, settings_.queue_cursor_bg_color);
        else if (is_bookmark) color_ansi = header_sgr(settings_);
        else if (is_playing) color_ansi = ansi_for(settings_.queue_playing_color, true) + bg_ansi_for(settings_.queue_playing_bg_color);
        else color_ansi = ansi_for(settings_.queue_color, false) + bg_ansi_for(settings_.queue_inactive_bg_color);
        out.push_back(bar + " " + color_ansi + padded + "\x1b[0m " + bar);
    }
    std::string footer;
    const int remaining = n - (qst_.scroll + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}


// ---------------------------------------------------------------------
// The tag editor of the audio book player (SHIFT+m). It only edits: the file name and the tags an audio book has. Nothing is
// fetched. Every row is written with ffmpeg -c copy (the audio stays bit for bit), one file per frame so the screen stays alive.
// ---------------------------------------------------------------------
const char* App::book_tag_label(int i) {
    static const char* const names[kBookTagCount] = {"File name", "Book title", "Author", "Narrator", "Chapter", "Publisher, Year", "Genre"};
    return names[std::clamp(i, 0, kBookTagCount - 1)];
}

// ffprobe once; the tags of a book vary a lot between taggers, so the usual alternatives are tried per row.
BookTagVals App::book_read_tags(const fs::path& file) {
    BookTagVals v{};
    v[0] = path_utf8(file.stem());   // the file name without its ending: the ending is not editable
    const ProcResult r = run_capture({"ffprobe", "-v", "error", "-show_entries", "format_tags", "-of", "default=nw=1",
                                      path_utf8(file)}, false);
    std::map<std::string, std::string> kv;
    std::istringstream in(r.out);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.rfind("TAG:", 0) != 0) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = ascii_lower_str(line.substr(4, eq - 4));
        if (!kv.count(key)) kv[key] = line.substr(eq + 1);
    }
    auto first_of = [&](std::initializer_list<const char*> keys) {
        for (const char* k : keys) { auto it = kv.find(k); if (it != kv.end() && !trim_ws(it->second).empty()) return it->second; }
        return std::string();
    };
    v[1] = first_of({"album"});
    v[2] = first_of({"artist", "album_artist", "author"});
    v[3] = first_of({"narrator", "composer", "performer"});
    v[4] = first_of({"title"});
    std::string year = first_of({"date", "year", "originaldate"});
    if (year.size() > 4) year.resize(4);
    const std::string pub = first_of({"publisher", "label", "copyright"});
    v[5] = pub.empty() ? year : year.empty() ? pub : pub + ", " + year;
    v[6] = first_of({"genre"});
    // Narrator, publisher and year of the BOOK live in the .cue next to it (a WAV file cannot hold them); what is there wins.
    BookCue cue;
    if (cue.load(BookCue::path_for(file.parent_path()))) {
        const std::string n = cue.get("REM NARRATOR"), p = cue.get("REM PUBLISHER"), d = cue.get("REM DATE");
        if (!n.empty()) v[3] = n;
        if (!p.empty() || !d.empty()) {
            const std::string yy = d.size() > 4 ? d.substr(0, 4) : d;
            const std::string pp = p.empty() ? (pub.empty() ? std::string() : pub) : p;
            // a part missing in the cue is taken from the tags
            const std::string y2 = yy.empty() ? year : yy;
            v[5] = pp.empty() ? y2 : y2.empty() ? pp : pp + ", " + y2;
        }
    }
    return v;
}

void App::book_ov_load(int chapter) {
    if (chapters_.empty()) return;
    book_->ov_chapter = std::clamp(chapter, 0, static_cast<int>(chapters_.size()) - 1);
    book_->ov_val = book_read_tags(chapters_[static_cast<size_t>(book_->ov_chapter)].path);
    book_->ov_orig = book_->ov_val;
    book_->ov_editing = false;
}

// The music player's library does not list the audio books: files under the AUDIO BOOK PATH are dropped from the scan result (so
// they are in no list, search or folder filter of the player). The book folder may lie inside a music folder. A music folder that
// itself lies inside the book folder (or is the book folder, which the settings call a conflict) keeps its own files.
void App::exclude_book_files(std::vector<LocalTrack>& tracks) const {
    if (audiobook_ || settings_.audiobook_path.empty()) return;
    auto norm = [](const std::string& s) {
        std::string n = path_utf8(path_from_utf8(s).lexically_normal());
        for (char& c : n) {
            if (c == '\\') c = '/';
#ifdef _WIN32
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
        }
        while (n.size() > 1 && n.back() == '/') n.pop_back();
        return n + "/";
    };
    const std::string book = norm(settings_.audiobook_path);
    std::vector<std::string> kept;   // music folders inside the book folder
    for (const std::string& root : settings_.local_music_paths) {
        const std::string r = norm(root);
        if (r.compare(0, book.size(), book) == 0) kept.push_back(r);
    }
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const LocalTrack& t) {
        const std::string p = norm(path_utf8(t.path));
        if (p.compare(0, book.size(), book) != 0) return false;
        for (const std::string& k : kept) if (p.compare(0, k.size(), k) == 0) return false;
        return true;
    }), tracks.end());
}

// Length of the whole book and how much of it has been heard (the chapters before the loaded one plus its elapsed time). A chapter
// whose length is not known yet is skipped and `complete` is false.
void App::book_totals(double elapsed, double& total, double& done, bool& complete) const {
    total = done = 0.0;
    complete = !chapters_.empty();
    const int cur = book_current_chapter();
    std::lock_guard<std::mutex> lk(row_meta_mutex_);
    for (size_t i = 0; i < chapters_.size(); ++i) {
        auto it = row_meta_cache_.find(path_utf8(chapters_[i].path));
        const double d = it != row_meta_cache_.end() && it->second.duration_sec > 0 ? it->second.duration_sec : 0.0;
        if (d <= 0) { complete = false; if (static_cast<int>(i) != cur) continue; }
        total += d;
        if (cur >= 0 && static_cast<int>(i) < cur) done += d;
    }
    if (cur >= 0) done += std::max(0.0, elapsed);
}

bool App::book_ov_dirty() const { return book_->ov_val != book_->ov_orig; }

void App::book_tags_open() {
    if (chapters_.empty()) { status_line_ = "open a book first (ENTER on a book), then SHIFT+m edits the tags of its chapters"; return; }
    int ch = qst_.focus ? qst_.selected : book_current_chapter();
    if (ch < 0) ch = qst_.selected;
    book_->ov_kind = BookOv::Tags;
    book_ov_load(ch);
    book_->ov_row = 0;
    book_->ov_confirm = false;
    book_->ov_rename_ask = false;
    book_->ov_status.clear();
    text_disengage();
    mode_ = Mode::BookOverlay;
}

// The renames a save would do. This chapter only: its file gets the typed name. Book-wide (whole_book): the typed name is the base of
// ALL files and the chapter number is added -- "<name> 01", "<name> 02" ... -- so no two chapters can end up with the same name.
// The file ending of every file stays as it is.
// 0 the file names carry no chapter number, 1 every one starts with it ("01 Title"), 2 every one ends with it ("Title 01")
int App::book_numbering() const {
    std::vector<std::string> stems;
    for (const BookChapter& c : chapters_) stems.push_back(path_utf8(c.path.stem()));
    return detect_numbering(stems);
}

// The renames a save would do. The chapter number always goes in FRONT of the name ("01 Title"): the folder is sorted by name, and
// the number keeps the chapters in their order however the titles are changed. A number that is there already is not added a second
// time (it stays, or, when it is at the end of every name, it is moved to the front).
std::vector<std::pair<fs::path, fs::path>> App::book_rename_plan(bool whole_book) const {
    std::vector<std::pair<fs::path, fs::path>> plan;
    if (chapters_.empty() || book_->ov_val[0] == book_->ov_orig[0]) return plan;
    const int kind = book_numbering();
    std::string typed = clean_file_stem(book_->ov_val[0]);
    typed = kind == 1 ? strip_front_number(typed) : kind == 2 ? strip_end_number(typed) : typed;
    if (typed.empty()) typed = clean_file_stem(book_->ov_val[0]);
    if (typed.empty()) return plan;
    auto target = [&](const fs::path& f, const std::string& name) { return f.parent_path() / path_from_utf8(name + path_utf8(f.extension())); };
    const size_t digits = std::max<size_t>(2, std::to_string(chapters_.size()).size());
    auto padded = [&](size_t n) { std::string t = std::to_string(n); t.insert(0, digits - std::min(digits, t.size()), '0'); return t; };
    if (whole_book) {   // every file: the number of its place, then the new name
        for (size_t i = 0; i < chapters_.size(); ++i) plan.emplace_back(chapters_[i].path, target(chapters_[i].path, padded(i + 1) + " " + typed));
        return plan;
    }
    for (size_t i = 0; i < chapters_.size(); ++i) {
        const std::string own = path_utf8(chapters_[i].path.stem());
        const bool is_this = static_cast<int>(i) == book_->ov_chapter;
        const std::string num = kind == 1 ? front_number(own) : kind == 2 ? end_number(own) : (is_this ? padded(i + 1) : std::string());
        if (is_this) plan.emplace_back(chapters_[i].path, target(chapters_[i].path, num + " " + typed));
        else if (kind == 2) plan.emplace_back(chapters_[i].path, target(chapters_[i].path, num + " " + strip_end_number(own)));   // the numbers at the end go to the front
    }
    return plan;
}

// Queues the writing. whole_book: the rows that describe the book (title, author, narrator, publisher + year, genre) that were
// changed go to every chapter, the chapter title stays this chapter's own, and a changed file name numbers every file (see above).
// Narrator, publisher and year also go to the book's .cue file.
void App::book_ov_save(bool whole_book) {
    if (book_->job.active) { book_->ov_status = "still saving ..."; return; }
    BookTagJob job;
    auto changed = [&](int i) { return book_->ov_val[static_cast<size_t>(i)] != book_->ov_orig[static_cast<size_t>(i)]; };
    for (size_t i = 0; i < chapters_.size(); ++i) {
        const bool is_this = static_cast<int>(i) == book_->ov_chapter;
        if (!whole_book && !is_this) continue;
        std::array<std::pair<bool, std::string>, 7> c;
        bool any = false;
        for (int r = 1; r < kBookTagCount; ++r) {   // row 0, the file name, is a rename (below), not a tag
            const bool book_wide = r == 1 || r == 2 || r == 3 || r == 5 || r == 6;
            const bool want = changed(r) && (is_this || book_wide);
            c[static_cast<size_t>(r)] = {want, book_->ov_val[static_cast<size_t>(r)]};
            any = any || want;
        }
        if (any) job.todo.emplace_back(chapters_[i].path, c);
    }
    if (!chapters_.empty()) {
        const fs::path dir = chapters_[static_cast<size_t>(book_->ov_chapter)].path.parent_path();
        job.cue_file = BookCue::path_for(dir);
        if (changed(3)) job.cue_set.emplace_back("REM NARRATOR", book_->ov_val[3]);
        if (changed(5)) {
            std::string pub, year;
            split_publisher_year(book_->ov_val[5], pub, year);
            job.cue_set.emplace_back("REM PUBLISHER", pub);
            job.cue_set.emplace_back("REM DATE", year);
        }
        if (whole_book && changed(1)) job.cue_set.emplace_back("TITLE", book_->ov_val[1]);
        if (whole_book && changed(2)) job.cue_set.emplace_back("PERFORMER", book_->ov_val[2]);
    }
    job.renames = book_rename_plan(whole_book);
    if (job.todo.empty() && job.cue_set.empty() && job.renames.empty()) { book_->ov_status = "nothing changed"; return; }
    job.active = true;
    book_->job = std::move(job);
    book_->ov_status = "saving ...";
}

// The last step of a save: the .cue, then the renames. A rename is done in two phases (every file to a temporary name, then to its
// new name) so a file may take the name another chapter has just left; if anything fails everything is put back.
void App::book_finish_job(BookTagJob& job) {
    std::error_code ec;
    if (!job.cue_set.empty()) {
        BookCue cue;
        cue.load(job.cue_file);
        for (const auto& kv : job.cue_set) cue.set(kv.first, kv.second);
        if (cue.empty()) { /* nothing to keep: no empty .cue is created */ }
        else if (!cue.save(job.cue_file)) { if (job.error.empty()) job.error = "could not write " + path_utf8(job.cue_file.filename()); }
        else job.cue_written = true;
    }
    auto& rn = job.renames;
    rn.erase(std::remove_if(rn.begin(), rn.end(), [](const auto& r) { return r.first == r.second; }), rn.end());
    if (rn.empty()) return;
    auto is_source = [&](const fs::path& p) {
        for (const auto& r : rn) if (path_key(r.first) == path_key(p)) return true;
        return false;
    };
    for (const auto& r : rn) {
        if (fs::exists(r.second, ec) && !is_source(r.second)) {
            if (job.error.empty()) job.error = "\"" + path_utf8(r.second.filename()) + "\" already exists, nothing renamed";
            return;
        }
    }
    if (has_track_ && current_.is_local) {   // the chapter that plays is held open by the player's decoder: it lets go first
        for (const auto& r : rn)
            if (path_key(r.first) == path_key(current_.path)) {
                device_stop();   // on the device worker (see App::device_run())
                if (current_.pcm) current_.pcm->cancelled.store(true);
                current_.pcm.reset();
                has_track_ = false;
                history_end_current_play();
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                break;
            }
    }
    std::vector<fs::path> tmp;
    for (size_t i = 0; i < rn.size(); ++i) tmp.push_back(rn[i].first.parent_path() / path_from_utf8(".mousiki-ren-" + std::to_string(i) + path_utf8(rn[i].first.extension())));
    size_t done1 = 0;
    for (; done1 < rn.size(); ++done1) {
        fs::rename(rn[done1].first, tmp[done1], ec);
        if (ec) break;
    }
    if (done1 < rn.size()) {   // phase 1 failed: put the moved files back
        for (size_t i = 0; i < done1; ++i) fs::rename(tmp[i], rn[i].first, ec);
        if (job.error.empty()) job.error = "could not rename " + path_utf8(rn[done1].first.filename()) + " (is it playing?)";
        return;
    }
    size_t done2 = 0;
    for (; done2 < rn.size(); ++done2) {
        fs::rename(tmp[done2], rn[done2].second, ec);
        if (ec) break;
    }
    if (done2 < rn.size()) {   // phase 2 failed: everything back to its old name
        for (size_t i = 0; i < done2; ++i) fs::rename(rn[i].second, rn[i].first, ec);
        for (size_t i = done2; i < rn.size(); ++i) fs::rename(tmp[i], rn[i].first, ec);
        if (job.error.empty()) job.error = "rename failed: " + ec.message();
        return;
    }
    for (const auto& r : rn) {
        for (auto& c : chapters_) if (c.path == r.first) { c.path = r.second; c.title = path_utf8(r.second.stem()); }   // every reference follows the new name
        if (path_utf8(current_.path) == path_utf8(r.first)) current_.path = r.second;
    }
    job.renamed = static_cast<int>(rn.size());
    {   // the bookmarks, the auto-mark, the chapter marks and the notes name their file: they follow the rename
        std::vector<std::pair<std::string, std::string>> names;
        for (const auto& r : rn) names.emplace_back(path_utf8(r.first.filename()), path_utf8(r.second.filename()));
        auto renamed = [&](const std::string& raw) {   // raw: "<file>" <rest>
            if (raw.empty() || raw[0] != '"') return raw;
            const size_t q = raw.find('"', 1);
            if (q == std::string::npos) return raw;
            const std::string f = raw.substr(1, q - 1);
            for (const auto& nm : names) if (nm.first == f) return "\"" + nm.second + raw.substr(q);
            return raw;
        };
        const fs::path dir = rn.front().first.parent_path();
        const fs::path cp = BookCue::path_for(dir);
        BookCue cue;
        if (cue.load(cp)) {
            for (const char* key : {"REM BOOKMARK", "REM CHAPTER"}) {
                std::vector<std::string> v = cue.get_all_raw(key);
                for (std::string& x : v) x = renamed(x);
                cue.set_all_raw(key, v);
            }
            const std::string am = cue.get_raw("REM AUTOMARK");
            if (!am.empty()) cue.set_raw("REM AUTOMARK", renamed(am));
            cue.save(cp);
        }
        std::vector<BookNote> notes;
        const fs::path nf = book_notes::path_for(dir);
        if (book_notes::load(nf, notes)) {
            bool any = false;
            for (BookNote& n : notes) for (const auto& nm : names) if (n.file == nm.first) { n.file = nm.second; any = true; break; }
            if (any) book_notes::save(nf, notes);
        }
        if (path_utf8(dir) == book_->dir) book_bookmark_load();
    }
}

// Render loop: reads the tags of a newly loaded chapter (for the metadata block) and runs one file of a tag save per frame.
void App::book_poll() {
    player_.set_speed(settings_.book_speed);   // the playback speed (Settings, SHIFT+LEFT / SHIFT+RIGHT): an atomic, cheap every frame
    {   // the picture in the field changes between nothing (cover being read), the cover and the standard picture: wipe the old image
        static std::string shown_kind;
        const std::string kind = !settings_.book_cover ? "std" : (current_cover(book_->dir) ? "cover:" + book_->dir : "other:" + book_->dir);
        if (kind != shown_kind) { shown_kind = kind; force_redraw_ = true; }
    }
    if (settings_.book_cover && gfxs_.proto == GfxProto::None && audiobook_) {
        static bool said = false;
        if (!said) { said = true; status_line_ = "book cover: " + book_cover_report(); }
    }
    if (settings_.book_cover && !book_->dir.empty()) {   // why there is no cover, once per book
        std::string note, nd;
        { std::lock_guard<std::mutex> lk(g_cover_note_mutex); note = g_cover_note; nd = g_cover_note_dir; }
        static std::string shown;
        if (!note.empty() && nd == book_->dir && shown != nd + note) { shown = nd + note; status_line_ = note + " (the console log, t, has the path)"; }
    }
    if (has_track_ && current_.is_local && book_->meta_path != path_utf8(current_.path)) {
        book_->meta_path = path_utf8(current_.path);
        book_->meta_vals = book_read_tags(current_.path);
    }
    if (!has_track_) book_->meta_path.clear();
    // a chapter that was started "at" a time: once it is loaded, go there
    if (book_->seek_pending) {
        if (has_track_ && current_.is_local && path_utf8(current_.path) == book_->seek_file && !load_in_progress_.load()) {
            player_.seek_relative(book_->seek_sec - player_.poll_elapsed());
            book_->seek_pending = false;
            book_->prev_elapsed = book_->seek_sec;
        } else if (std::chrono::steady_clock::now() - book_->seek_at > std::chrono::seconds(20)) book_->seek_pending = false;
    }
    book_notes_follow();
    book_automark_tick(false);
    if (book_->cut.active) { book_cut_step(); return; }
    BookTagJob& job = book_->job;
    if (!job.active) return;
    if (job.next >= job.todo.size()) {
        book_finish_job(job);
        job.active = false;
        std::string what = std::to_string(job.ok) + " chapter" + (job.ok == 1 ? "" : "s");
        if (job.renamed > 0) what += ", " + std::to_string(job.renamed) + " file name" + (job.renamed == 1 ? "" : "s");
        if (job.cue_written) what += ", .cue";
        book_->ov_status = job.error.empty() ? "saved " + what : "saved " + what + ", failed: " + job.error;
        if (!chapters_.empty()) {
            const int keep_row = book_->ov_row;
            book_ov_load(book_->ov_chapter);
            book_->ov_row = keep_row;
        }
        book_->meta_path.clear();   // the metadata block reads the loaded chapter again
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);   // the tags and names on disk changed: the cached row data is stale
            row_meta_cache_.clear();
        }
        search_.idx.clear();
        if (job.ok > 0 || job.renamed > 0) rescan_library();   // names and tags changed: the lists read them again
        return;
    }
    const auto& [file, ch] = job.todo[job.next++];
    std::error_code ec;
    const std::string ext = path_utf8(file.extension());
    const fs::path tmp = file.parent_path() / (path_utf8(file.stem()) + ".mousiki-tmp" + ext);
    Args cmd = {"ffmpeg", "-y", "-v", "error", "-i", path_utf8(file), "-map", "0", "-map_metadata", "0", "-c", "copy"};
    auto meta = [&](const std::string& k, const std::string& v) { cmd.push_back("-metadata"); cmd.push_back(k + "=" + v); };
    if (ch[1].first) meta("album", ch[1].second);
    if (ch[2].first) { meta("artist", ch[2].second); meta("album_artist", ch[2].second); }
    if (ch[3].first) { meta("composer", ch[3].second); meta("narrator", ch[3].second); }
    if (ch[4].first) meta("title", ch[4].second);
    if (ch[5].first) {
        std::string pub, year;
        split_publisher_year(ch[5].second, pub, year);
        meta("publisher", pub);
        meta("date", year);
    }
    if (ch[6].first) meta("genre", ch[6].second);
    cmd.push_back(path_utf8(tmp));
    const ProcResult r = run_capture(cmd, true);
    if (!r.ok()) {
        fs::remove(tmp, ec);
        if (job.error.empty()) job.error = "ffmpeg could not write " + path_utf8(file.filename());
    } else {
        fs::rename(tmp, file, ec);
        if (ec) {
            fs::remove(tmp, ec);
            if (job.error.empty()) job.error = "could not replace " + path_utf8(file.filename()) + " (is it playing?)";
        } else {
            ++job.ok;
        }
    }
    book_->ov_status = "saving " + std::to_string(job.next) + "/" + std::to_string(job.todo.size()) + " ...";
}

bool App::book_tags_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (book_->job.active) return true;   // a save is running: keys wait
    if (book_->ov_rename_ask) {   // [a] with a changed file name: the chapter numbers are added to every file
        if (key == 'y' || key == 'Y') { book_->ov_rename_ask = false; book_ov_save(book_->ov_rename_whole); }
        else if (key == 'n' || key == 'N' || key == 27 || key == '\r' || key == '\n') book_->ov_rename_ask = false;
        return true;
    }
    if (book_->ov_confirm) {   // ESC with unsaved changes
        if (key == 'y' || key == 'Y') { book_->ov_confirm = false; book_ov_save(false); }
        else if (key == 'n' || key == 'N') { book_->ov_confirm = false; mode_ = Mode::Browse; }
        else if (key == 27 || key == '\r' || key == '\n') book_->ov_confirm = false;
        return true;
    }
    if (book_->ov_editing) {
        std::string& cur = book_->ov_val[static_cast<size_t>(book_->ov_row)];
        const std::string owner = "booktag" + std::to_string(book_->ov_row);
        if (!arrow && (key == '\r' || key == '\n' || key == 27)) { book_->ov_editing = false; return true; }
        edit_focus(owner, cur);
        edit_text_key(cur, edit_caret_, edit_anchor_, key, book_->ov_row == 0 ? 200 : 160, &book_->ov_status);
        return true;
    }
    if (arrow && key == 'A') { book_->ov_row = (book_->ov_row + kBookTagCount - 1) % kBookTagCount; return true; }
    if (arrow && key == 'B') { book_->ov_row = (book_->ov_row + 1) % kBookTagCount; return true; }
    if (arrow && (key == 'C' || key == 'D')) {   // the next / previous chapter
        if (book_ov_dirty()) { book_->ov_status = "unsaved changes: [s] save, [d] discard them first"; return true; }
        book_ov_load(book_->ov_chapter + (key == 'C' ? 1 : -1));
        book_->ov_status.clear();
        return true;
    }
    if (key == '\r' || key == '\n') {
        book_->ov_editing = true;
        edit_owner_ = "booktag" + std::to_string(book_->ov_row);
        edit_caret_ = edit_anchor_ = book_->ov_val[static_cast<size_t>(book_->ov_row)].size();
        return true;
    }
    if (key == 's') {
        if (book_->ov_val[0] != book_->ov_orig[0] && book_numbering() == 2 && !book_rename_plan(false).empty()) { book_->ov_rename_ask = true; book_->ov_rename_whole = false; }   // the numbers at the end move to the front: say so first
        else book_ov_save(false);
        return true;
    }
    if (key == 'a') {
        if (book_->ov_val[0] != book_->ov_orig[0] && !book_rename_plan(true).empty()) { book_->ov_rename_ask = true; book_->ov_rename_whole = true; }   // numbering is added: say so first
        else book_ov_save(true);
        return true;
    }
    if (key == 'd') { book_->ov_val = book_->ov_orig; book_->ov_status = "changes discarded"; return true; }
    if (key == 27 || key == 'q') {
        if (book_ov_dirty()) { book_->ov_confirm = true; return true; }
        mode_ = Mode::Browse;
        return true;
    }
    return true;
}

std::vector<std::string> App::build_book_tags_panel(int panel_w) const {
    const int W = panel_w, inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    auto framed = [&](const std::string& body) { return bar + " " + body + R + " " + bar; };
    std::vector<std::string> lines;
    lines.push_back(box_top("Book tags", W, border));
    const std::string plain_ink = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
    const std::string head = chapters_.empty() ? std::string() :
        "Chapter " + std::to_string(book_->ov_chapter + 1) + " of " + std::to_string(chapters_.size()) + "   " + path_utf8(chapters_[static_cast<size_t>(book_->ov_chapter)].path.filename());
    lines.push_back(framed(legend_sgr(settings_) + pad_right(truncate_str(head, inner), inner)));
    lines.push_back(framed(pad_right("", inner)));
    const int label_w = 18;
    // The file name row edits the name only: the ending (.mp3 ...) is shown after it and cannot be changed.
    const std::string ext0 = chapters_.empty() ? std::string() : path_utf8(chapters_[static_cast<size_t>(book_->ov_chapter)].path.extension());
    const int val_w_full = std::max(8, inner - label_w - 2);
    for (int i = 0; i < kBookTagCount; ++i) {
        const bool hov = i == book_->ov_row;
        const bool edited = book_->ov_val[static_cast<size_t>(i)] != book_->ov_orig[static_cast<size_t>(i)];
        const std::string lab = std::string(hov ? "> " : "  ") + pad_right(book_tag_label(i), label_w - 2);
        const std::string base = hov ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) : plain_ink;
        std::string val_raw = book_->ov_val[static_cast<size_t>(i)];
        const std::string ending = i == 0 ? ext0 : std::string();
        const int val_w = std::max(4, val_w_full - display_width(ending));
        std::string body;
        int used;
        if (hov && book_->ov_editing) {
            const std::string owner = "booktag" + std::to_string(i);
            size_t c = val_raw.size();
            size_t a = c;
            if (edit_owner_ == owner) { c = edit_caret_; a = edit_anchor_; }
            const std::string restore = change_sgr(settings_);
            const EditPaint p = paint_edit_field(val_raw, c, a, val_w, restore, true);
            body = base + lab + restore + p.s + base + ending;
            used = display_width(lab) + p.cols + display_width(ending);
        } else {
            const std::string shown = truncate_str(val_raw.empty() ? std::string("-") : val_raw, val_w);
            body = base + lab + (edited ? header_sgr(settings_) : std::string()) + shown + (edited ? R + base : std::string()) + ending;
            used = display_width(lab) + display_width(shown) + display_width(ending);
        }
        lines.push_back(framed(body + std::string(static_cast<size_t>(std::max(0, inner - used)), ' ') + R));
    }
    lines.push_back(framed(pad_right("", inner)));
    // The note rows: the status, or the question that is asked. The question is shown as a block in the header colour with black
    // text (three rows, a long text is wrapped over them); the status takes two rows.
    std::string note = book_->ov_status;
    const bool asking = book_->ov_rename_ask;
    if (asking) {
        const auto plan = book_rename_plan(true);
        // (the number is in front: a name that has to be shortened keeps it)
        auto ex = [&](size_t i) {
            if (plan.size() <= i) return std::string();
            return truncate_str(path_utf8(plan[i].second.stem()), 26);
        };
        const bool at_end = book_numbering() == 2;
        if (book_->ov_rename_whole)
            note = std::string("The chapter number is put in FRONT of every file name (\"") + ex(0) + "\", \"" + ex(1) + "\" ...) so renaming titles can never jumble the track order when the folder is sorted by name." +
                   (at_end ? " The numbers at the end of the old names are removed." : "") + "   [y] rename all   [n] back";
        else
            note = "Your files end in their chapter number (\"" + path_utf8(chapters_[0].path.stem()) + "\"). It is moved to the FRONT of every name (\"" + ex(0) + "\" ...) so renaming titles can never jumble the track order when the folder is sorted by name.   [y] rename the folder's files   [n] back";
    } else if (book_->ov_confirm) note = "Unsaved changes: [y] save this chapter  [n] discard  [ESC] keep editing";
    else if (note.empty() && book_->ov_val[0] != book_->ov_orig[0]) note = "[a] renames the whole book (\"01 " + truncate_str(clean_file_stem(book_->ov_val[0]), 22) + "\", \"02 ...\"); [s] renames this chapter and puts its chapter number in front of the name";
    else if (note.empty() && book_ov_dirty()) note = "unsaved changes";
    const size_t note_rows = asking ? 3 : 2;
    std::vector<std::string> nl = wrap_words(note, inner);
    if (nl.size() > note_rows) { nl.resize(note_rows); nl.back() = truncate_str(nl.back(), inner); }
    nl.resize(note_rows);
    const std::string hdr_bg = bg_ansi_for(settings_.header_color);
    const std::string ask_sgr = hdr_bg.empty() ? std::string("\x1b[7m") : hdr_bg + "\x1b[30m";   // header colour as the background, black text (no header colour: reverse video)
    for (const std::string& t0 : nl) {   // the note rows are centred
        const std::string t = truncate_str(t0, inner);
        const std::string centred = std::string(static_cast<size_t>(std::max(0, (inner - display_width(t)) / 2)), ' ') + t;
        if (asking) lines.push_back(framed(ask_sgr + pad_right(centred, inner)));
        else lines.push_back(framed(legend_sgr(settings_) + pad_right(centred, inner)));
    }
    // The key legend in the colour of the bottom border, centred; several rows when one would not fit (the middle one sits on the border).
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return bar + " " + border_bottom + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, inner - left - display_width(tx))), ' ') + "\x1b[0m" + " " + bar;
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    if (book_->ov_editing) push_legend_block(lines, W, border_bottom, {"Type the text  [ENTER] / [ESC] done typing"}, leg_in, leg_bot);
    else push_legend_block(lines, W, border_bottom, {"[UP/DOWN] row  [ENTER] edit  [LEFT/RIGHT] other chapter", "[s] save this chapter  [a] save the book-wide rows to all chapters", "[d] discard changes  [ESC] close"}, leg_in, leg_bot);
    return lines;
}

bool App::book_field_has_note() const { return audiobook_ && (book_->note_edit || book_->note_shown); }

} // namespace muisc
