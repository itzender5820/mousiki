// Listening history overlay (HISTORY / TOP TRACKS / TOP PLAYLISTS / HABITS) and the smart-history queue. Split out of app.cpp.
// Member functions of App (declared in app.h); the helpers shared with app.cpp are in app_internal.h.
#include "app.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "keyboard_layout.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "console_log.h"
#include "karaoke_font.h"
#include "glyph_raster.h"
#include "chiptune.h"
#include "scope_big.h"
#include "scope_window.h"
#include "wizard.h"
#include "creator.h"
#include "mic_capture.h"
#include "process_util.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <sys/types.h>
#include <sys/stat.h>   // file_added_time(): stat() / _wstat64()
#if defined(_WIN32)
#include "win_compat.h"
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "app_internal.h"

namespace muisc {

void App::history_open() {
    mode_ = Mode::History;
    history_tab_ = 0;
    history_selected_ = 0;
    history_scroll_ = 0;
    history_pane_ = 0;
    history_add_sel_ = 0;
    history_status_.clear();
    if (history_.plays().empty())
        history_status_ = "nothing played yet in this installation";
    history_refresh_top();
    force_redraw_ = true; // full-screen takeover, same rule as entering Settings
}

void App::history_refresh_top() {
    history_top_view_ = history_top(history_.plays(), history_most_first_, &history_.archive());
}

// Closes the record of whatever is playing right now. Called on every
// hand-over (a new track replacing it, playback running out, the app
// quitting), never from anywhere that could see a record twice -- the store
// itself refuses to close one that isn't live.
void App::history_end_current_play() {
    if (!history_.live()) return;
    const HistoryPlay* p = history_.live_play();
    const double len = p ? p->len_sec : 0.0;
    const double heard = p ? p->listened_sec : 0.0;
    // "Heard to the end" = the player ran out of track, or the listener got
    // through 90% of it (which also covers seeking close to the end and then
    // letting it run out). Everything else -- a skip, an app closed
    // mid-track -- is not a completion, and that is what the Habits tab's
    // completion rate is built on.
    const bool completed = player_.finished() || (len > 0.0 && heard >= 0.9 * len);
    history_.finish_live(completed);
    history_.save(); // one small JSON write per finished track
}

// Opens a record for current_path_/current_video_id_/metadata_. Must only be
// called once those three already describe the NEW track.
void App::history_begin_current_play() {
    if (!has_track_) return;
    HistoryPlay p;
    p.id = history_track_id(current_is_local_, path_utf8(current_path_), current_video_id_);
    if (p.id.empty()) return; // nothing to aggregate by: record nothing rather than a phantom title
    p.title = metadata_.name;
    if (p.title.empty() || p.title == "-")
        p.title = path_utf8(current_path_.stem()); // untagged local file
    p.artist = (metadata_.artist == "-") ? std::string() : metadata_.artist;
    p.len_sec = static_cast<double>(total_sec_);
    p.started_at = static_cast<long long>(std::time(nullptr));
    p.playlist = current_playlist_;
    if (audiobook_) {   // the reading history: the chapter, the book's name as the "artist" and as the "playlist" (TOP BOOKS)
        const std::string book = path_utf8(current_path_.parent_path().filename());
        p.artist = book;
        p.playlist = book;
    }
    history_.begin_play(p);
}

// The smart grid of the ADD SMART HISTORY TO QUEUE pane (Top Tracks tab):
// column titles and the label of every list, [column][row].
static const char* const kSmartColTitles[4] = {"TOP TRACKS", "TOP OF THE ...", "TIME OF DAY", "REDISCOVER"};
static const char* const kSmartLabels[4][4] = {
    {"Top 10 tracks", "Top 25 tracks", "Top 50 tracks", "Top 100 tracks"},
    {"Top 25 of the week", "Top 25 of the month", "Top 25 of the quarter", "Top 25 of the year"},
    {"Morning (7-11 a.m.)", "Day (11 a.m.-6 p.m.)", "Evening (6-10 p.m.)", "Night (10 p.m.-7 a.m.)"},
    {"Last 25 newly added", "Least 25 played", "Least played: month", "Least played: year"},
};

// When a local file arrived in the library: the later of its modification
// time and its creation / status-change time (a file copied or downloaded
// into a music folder keeps an old modification time on many systems, but
// its creation time -- Windows -- or inode change time -- POSIX -- is when it
// arrived). 0 when the file cannot be read.
long long file_added_time(const fs::path& p) {
#if defined(_WIN32)
    struct _stat64 st;
    if (_wstat64(p.c_str(), &st) != 0) return 0;
#else
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return 0;
#endif
    return static_cast<long long>(std::max(st.st_mtime, st.st_ctime));
}

void App::history_queue_rows(const std::vector<HistoryTopRow>& rows, int take, const std::string& label,
                             const std::string& empty_msg) {
    if (rows.empty()) { history_status_ = empty_msg; return; }
    const int n = std::min(take, static_cast<int>(rows.size()));
    int added = 0, missing = 0;
    for (int i = 0; i < n; ++i) {
        const HistoryTopRow& r = rows[static_cast<size_t>(i)];
        const std::string artist = (r.artist == "-") ? std::string() : r.artist;
        if (r.id.compare(0, 3, "yt:") == 0) {
            queue_.push_back({false, r.title, artist, {}, r.id.substr(3), false, ""});
            ++added;
        } else {
            fs::path path = path_from_utf8(r.id);
            std::error_code ec;
            if (!fs::exists(path, ec)) { ++missing; continue; } // moved/deleted since it was played
            const std::string title = r.title.empty() ? path_utf8(path.stem()) : r.title;
            queue_.push_back({true, title, artist, path, "", false, ""});
            ++added;
        }
    }
    clamp_queue_selected();
    queue_reveal(static_cast<int>(queue_.size()) - 1);
    std::string msg = "queued " + std::to_string(added) + " tracks: " + label;
    if (n < take) msg += " (only " + std::to_string(n) + " available)";
    if (missing > 0) msg += " (" + std::to_string(missing) + " missing, skipped)";
    history_status_ = msg;
    log_event(msg);
}

void App::history_add_top_to_queue(int n) {
    // Ranked independently of the list above (which 'r' can flip to
    // least-played first): "top N" always means the N most-played titles.
    history_queue_rows(history_top(history_.plays(), /*most_first=*/true, &history_.archive()), n,
                       "Top " + std::to_string(n) + " tracks", "nothing played yet -- nothing to queue");
}

void App::history_queue_smart(int col, int row) {
    col = std::clamp(col, 0, 3);
    row = std::clamp(row, 0, 3);
    const std::string label = kSmartLabels[col][row];
    const long long now = static_cast<long long>(std::time(nullptr));
    const HistoryArchive* arch = &history_.archive();
    if (col == 0) { history_add_top_to_queue(kHistoryAddCounts[row]); return; }
    if (col == 1) {   // top of the last 7 / 30 / 91 / 365 days
        static const int kDays[4] = {7, 30, 91, 365};
        history_queue_rows(history_top_since(history_.plays(), arch, now - kDays[row] * 86400LL, true),
                           kSmartListSize, label, "nothing played in that time -- nothing to queue");
        return;
    }
    if (col == 2) {   // morning / day / evening / night
        history_queue_rows(history_top_tod(history_.plays(), arch, row, true),
                           kSmartListSize, label, "nothing played at that time of day yet -- nothing to queue");
        return;
    }
    if (row == 2 || row == 3) {   // least played among what was played in the last 30 / 365 days
        history_queue_rows(history_top_since(history_.plays(), arch, now - (row == 2 ? 30 : 365) * 86400LL, false),
                           kSmartListSize, label, "nothing played in that time -- nothing to queue");
        return;
    }
    // Rows 0 and 1 work on the local library itself, not on the history.
    if (all_local_tracks_.empty()) { history_status_ = "the local library is empty -- nothing to queue"; return; }
    std::vector<HistoryTopRow> rows;
    if (row == 0) {   // the newest arrivals in the music folders
        std::vector<std::pair<long long, size_t>> by_time;
        by_time.reserve(all_local_tracks_.size());
        for (size_t i = 0; i < all_local_tracks_.size(); ++i) {
            if (LocalSource::is_video_file(all_local_tracks_[i].path) && video_is_paired(all_local_tracks_[i].path)) continue;   // (its song is listed)
            by_time.push_back({file_added_time(all_local_tracks_[i].path), i});
        }
        std::stable_sort(by_time.begin(), by_time.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t k = 0; k < by_time.size() && static_cast<int>(rows.size()) < kSmartListSize; ++k) {
            const LocalTrack& t = all_local_tracks_[by_time[k].second];
            HistoryTopRow r;
            r.id = path_utf8(t.path); r.title = t.title; r.artist = t.folder_artist;
            rows.push_back(r);
        }
    } else {          // the whole library, fewest plays first -- never played counts as 0
        std::map<std::string, int> plays;
        for (const auto& r : history_top(history_.plays(), true, arch)) plays[r.id] = r.plays;
        std::vector<std::pair<int, size_t>> by_plays;
        by_plays.reserve(all_local_tracks_.size());
        for (size_t i = 0; i < all_local_tracks_.size(); ++i) {
            if (LocalSource::is_video_file(all_local_tracks_[i].path) && video_is_paired(all_local_tracks_[i].path)) continue;
            auto it = plays.find(path_utf8(all_local_tracks_[i].path));
            by_plays.push_back({it == plays.end() ? 0 : it->second, i});
        }
        // Shuffled first, so tracks with the same count come in a different
        // order every time (otherwise "least played" would always be the
        // same alphabetical handful of never-played files).
        std::shuffle(by_plays.begin(), by_plays.end(), std::mt19937(std::random_device{}()));
        std::stable_sort(by_plays.begin(), by_plays.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t k = 0; k < by_plays.size() && static_cast<int>(rows.size()) < kSmartListSize; ++k) {
            const LocalTrack& t = all_local_tracks_[by_plays[k].second];
            HistoryTopRow r;
            r.id = path_utf8(t.path); r.title = t.title; r.artist = t.folder_artist;
            rows.push_back(r);
        }
    }
    history_queue_rows(rows, kSmartListSize, label, "the local library is empty -- nothing to queue");
}

void App::handle_history_key(int key) {
    if (key == 0) return;
    const bool arrow = last_key_was_arrow();

    // ESC in the smart grid only gives the focus back to the track list.
    if (key == 27 && history_tab_ == 1 && history_pane_ == 1) { history_pane_ = 0; history_status_.clear(); return; }
    // Leaving: ESC, or SHIFT+h again -- the very key that opened this
    // (nothing here is a text field, so an uppercase 'H' is always that key).
    if (key == 27 || key_is("HKeyHistory", key)) { mode_ = Mode::Browse; return; }

    // Top Tracks tab: TAB switches between the two panes (the track list and
    // ADD SMART HISTORY TO QUEUE). It never switches tabs (Left/Right do).
    if (key == 9) {
        if (history_tab_ == 1 && !audiobook_) { history_pane_ ^= 1; history_status_.clear(); }
        return;
    }

    // Smart grid focused: all four arrows move in the grid (Left/Right do NOT
    // switch tabs here), Enter queues the highlighted list.
    if (history_tab_ == 1 && history_pane_ == 1) {
        if (arrow && key == 'A') { if (history_add_sel_ > 0) --history_add_sel_; return; }
        if (arrow && key == 'B') { if (history_add_sel_ < 3) ++history_add_sel_; return; }
        if (arrow && key == 'D') { if (history_add_col_ > 0) --history_add_col_; return; }
        if (arrow && key == 'C') { if (history_add_col_ < 3) ++history_add_col_; return; }
        if (key == '\r' || key == '\n') { history_queue_smart(history_add_col_, history_add_sel_); return; }
    }

    // Tab strip: Left/Right cycle, 1/2/3/4 jump outright.
    if (arrow && (key == 'C' || key == 'D')) {
        // 'D' is Left, 'C' is Right -- the same convention the seek keys and
        // the caret movement above use (see handle_key()'s "key == 'D' //
        // left = seek back"). Swapped here, every arrow press walked the tab
        // strip backwards, which on a 3-tab strip looks like jumping 1->3.
        const int dir = (key == 'D') ? -1 : 1;
        history_tab_ = (history_tab_ + dir + kHistTabs) % kHistTabs;
        history_selected_ = 0;
        history_scroll_ = 0;
        history_pane_ = 0;
        history_status_.clear();
        return;
    }
    if (key == '1' || key == '2' || key == '3' || key == '4') {
        history_tab_ = key - '1';
        history_selected_ = 0;
        history_scroll_ = 0;
        history_pane_ = 0;
        history_status_.clear();
        return;
    }

    // Enter on a track of the HISTORY / TOP TRACKS list: play it right away (the overlay stays open).
    if (!arrow && (key == '\r' || key == '\n') && history_tab_ != kHistTabPlaylists && history_tab_ != kHistTabHabits) {
        std::string id, title, artist;
        const auto& plays = history_.plays();
        if (history_tab_ == 0) {
            const int n = static_cast<int>(std::min<size_t>(100, plays.size()));
            if (history_selected_ >= 0 && history_selected_ < n) {
                const HistoryPlay& p = plays[static_cast<size_t>(history_selected_)];
                id = p.id; title = p.title; artist = p.artist;
            }
        } else if (history_selected_ >= 0 && history_selected_ < static_cast<int>(history_top_view_.size())) {
            const HistoryTopRow& r = history_top_view_[static_cast<size_t>(history_selected_)];
            id = r.id; title = r.title; artist = r.artist;
        }
        if (id.empty()) return;
        if (audiobook_) {   // a chapter: its book opens, the chapter plays (with the bookmark questions)
            history_status_ = book_play_file(id) ? "playing: " + title : "cannot play: the file no longer exists";
            return;
        }
        if (artist == "-") artist.clear();
        if (id.compare(0, 3, "yt:") == 0) {
            OnlineResult r{id.substr(3), title, artist};
            start_online_track(r);
            history_status_ = "playing: " + title;
        } else {
            fs::path path = path_from_utf8(id);
            std::error_code ec;
            if (!fs::exists(path, ec)) { history_status_ = "cannot play: the file no longer exists"; return; }
            LocalTrack t{path_utf8(path.stem()), path, artist};
            start_local_track(t);
            history_status_ = "playing: " + (title.empty() ? path_utf8(path.stem()) : title);
        }
        return;
    }

    // Up/Down: a cursor on the two list tabs, plain scrolling on Top Playlists / Habits
    // (whose content has no cursor and may be taller than the panel).
    const bool scroll_tab = (history_tab_ == kHistTabPlaylists || history_tab_ == kHistTabHabits);
    const int total = (history_tab_ == 0)
        ? static_cast<int>(std::min<size_t>(100, history_.plays().size()))
        : static_cast<int>(history_top_view_.size());
    if (arrow && key == 'A') {
        if (scroll_tab) history_scroll_ = std::max(0, history_scroll_ - (last_key_was_shifted() ? 10 : 1));   // SHIFT+arrow / PgUp / PgDn: a page of 10
        else history_selected_ = std::max(0, history_selected_ - (last_key_was_shifted() ? 10 : 1));
        return;
    }
    if (arrow && key == 'B') {
        if (scroll_tab) history_scroll_ += (last_key_was_shifted() ? 10 : 1); // clamped against the real content while drawing
        else history_selected_ = std::max(0, std::min(total - 1, history_selected_ + (last_key_was_shifted() ? 10 : 1)));
        return;
    }

    // 'r' flips the Top Tracks order: most played on top (the default) and
    // least played on top. Nowhere else to do it, so it is a no-op there
    // rather than a surprise.
    if (key == 'r' || key == 'R') {
        if (history_tab_ != 1) return;
        history_most_first_ = !history_most_first_;
        history_refresh_top();
        if (history_selected_ >= static_cast<int>(history_top_view_.size()))
            history_selected_ = std::max(0, static_cast<int>(history_top_view_.size()) - 1);
        history_status_ = history_most_first_ ? "TOP TRACKS: most played first"
                                              : "TOP TRACKS: least played first";
        return;
    }
    if (key == 'q' || key == 'Q') mode_ = Mode::Browse;
}

void App::build_history_screen(std::ostringstream& frame, int W, int target_height) {
    // Rebuilt every frame: a track can finish (and another start) while this
    // overlay is on screen -- playback never pauses for it.
    history_refresh_top();

    if (W < 60) W = 60;
    std::string border = ansi_for(settings_.border_color, false);
    std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string HI = "\x1b[7m", R = "\x1b[0m";

    {   // one fused pane: LISTENING HISTORY rests on the top border line, the settings-style tab strip below it (no numbers)
        const auto strip = menu_tab_strip(W, audiobook_ ? "READING HISTORY" : "LISTENING HISTORY",
                                          audiobook_ ? std::vector<std::string>{"HISTORY", "TOP CHAPTERS", "TOP BOOKS", "HABITS"} : std::vector<std::string>{"HISTORY", "TOP TRACKS", "TOP PLAYLISTS", "HABITS"}, history_tab_, border, border,
                                          settings_.box_upper_left, settings_.box_upper_right, settings_.box_vertical, 0, "", 0,
                                          ansi_for(settings_.tab_current_color, false), ansi_for(settings_.tab_other_color, false));
        frame << strip.first << "\n" << strip.second << "\n";
    }

    // Chrome around the rows: the two strip lines + the bottom border = 3 (the panel adds its bottom border itself,
    // so only 1 is counted here besides the "- 2" below).
    int fixed_rows = 1;

    // Footer first, because the panel takes exactly the rows it leaves: the
    // legend (as many rows as it needs at this width -- one on a 120-column
    // terminal) plus a status row ONLY while a message is showing. Nothing is
    // reserved to sit blank (only the legend and, while a message is up, one status row are shown);
    // while a message is up the list
    // simply gives up one row for it, the way Browse does for its prompt.
    const std::string hint = (history_tab_ == 1 && history_pane_ == 1)
        ? "[\u2191\u2193\u2190\u2192] Pick a list  [ENTER] Add it to the queue  [TAB] Track list  [ESC] Leave the pane"
        : (history_tab_ == 1 && audiobook_)
        ? "[\u2190\u2192] Switch Tab  [1-4] Tab  [\u2191\u2193] Move  [SHIFT+\u2191\u2193] Page  [ENTER] Play the chapter  [r] Flip sort  [ESC] Exit"
        : (history_tab_ == 1)
        ? "[\u2190\u2192] Switch Tab  [1-4] Tab  [TAB] Smart history pane  [\u2191\u2193] Move  [SHIFT+\u2191\u2193] Page  [ENTER] Play  [r] Flip sort  [ESC] Exit"
        : (history_tab_ == kHistTabPlaylists || history_tab_ == kHistTabHabits)
        ? "[\u2190\u2192] Switch Tab  [1-4] Tab  [\u2191\u2193] Scroll  [SHIFT+\u2191\u2193] Page  [ESC] Exit"
        : "[\u2190\u2192] Switch Tab  [1-4] Tab  [\u2191\u2193] Move  [SHIFT+\u2191\u2193] Page  [ENTER] Play  [ESC] Exit";
    std::vector<std::string> hint_lines(1);
    size_t pos = 0;
    for (;;) {
        size_t sep = hint.find("  ", pos);
        std::string entry = (sep == std::string::npos) ? hint.substr(pos) : hint.substr(pos, sep - pos);
        std::string& cur = hint_lines.back();
        std::string joined = cur.empty() ? entry : cur + "  " + entry;
        if (cur.empty() || display_width(joined) <= W || hint_lines.size() >= 2) cur = joined;
        else hint_lines.push_back(entry);
        if (sep == std::string::npos) break;
        pos = sep + 2;   // entries are separated by two blanks
    }
    const int legend_rows = static_cast<int>(hint_lines.size());
    const int status_rows = history_status_.empty() ? 0 : 1;

    // Same two bounds build_meta_screen() sizes against: term_rows_ - 1 is
    // what clamp_output_rows() keeps, target_height is what the other
    // overlays use. 2 = the panel's own border rows.
    int budget = std::min(target_height, term_rows_ - 1);
    // Top Tracks carries a second pane (ADD SMART HISTORY TO QUEUE: a title
    // row + four option rows + its own two border rows) below the list, so the list gives those
    // rows up -- the whole tab still adds up to `budget` lines (29 on a
    // 30-line terminal). No upper cap on the list: it fills a maximised window.
    const bool top_tab = (history_tab_ == 1) && !audiobook_;   // (no smart queue pane for books)
    const int add_body = 5;                       // column titles + 4 rows of lists
    const int add_rows = top_tab ? add_body + 2 : 0;
    int panel_h = std::max(top_tab ? 3 : 6, budget - fixed_rows - 2 - legend_rows - status_rows - add_rows);
    for (const auto& l : build_history_panel(W, panel_h)) frame << l << "\n";
    if (top_tab)
        for (const auto& l : build_history_add_panel(W, add_body)) frame << l << "\n";

    for (const auto& l : hint_lines)
        frame << legend_colored(settings_, truncate_str(l, W)) << "\n";
    if (status_rows) frame << "\x1b[38;5;246m" << truncate_str(history_status_, W) << "\x1b[0m\n";
}

std::vector<std::string> App::build_history_panel(int total_width, int height) {
    const int inner = std::max(20, total_width - 4);
    const int body = std::max(1, height); // rows between box_top() and box_bottom()
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    const std::string R = "\x1b[0m";
    std::vector<std::string> out;

    // The info that sits on the bottom border line.
    std::string info;
    if (history_tab_ == 0) info = std::to_string(history_.plays().size()) + (audiobook_ ? " chapter plays (newest 100 shown)" : " plays (newest 100 shown)");
    else if (history_tab_ == 1) info = history_most_first_ ? "most played first" : "least played first";
    else if (history_tab_ == kHistTabPlaylists) info = "top 5 each";

    // Column helpers. Both are only ever fed PLAIN text: display_width()
    // counts escape bytes as columns, so a decorated string must be padded
    // first and coloured after (same rule every other panel here follows).
    auto L = [](const std::string& s, int n) { // right-align a value (by display width: the PLAYS arrow is 3 bytes, 1 column)
        const int w = display_width(s);
        if (w >= n) return truncate_str(s, n);
        return std::string(static_cast<size_t>(n - w), ' ') + s;
    };
    auto T = [&](const std::string& s, int n) { // left-align, truncated at display width
        std::string t = truncate_str(s, n);
        const int fill = n - display_width(t);
        return t + (fill > 0 ? std::string(static_cast<size_t>(fill), ' ') : std::string());
    };
    auto row = [&](const std::string& plain, const std::string& sgr) {
        out.push_back(bar + " " + sgr + pad_right(truncate_str(plain, inner), inner) + R + " " + bar);
    };
    // A section header: only the title carries the colour, like every other
    // header in the app -- a full-width bar would read as a selected row.
    auto head = [&](const std::string& plain) {
        std::string t = truncate_str(plain, inner);
        out.push_back(bar + " " + header_sgr(settings_) + t + R +
                      std::string(static_cast<size_t>(std::max(0, inner - display_width(t))), ' ') + " " + bar);
    };
    auto blank = [&]() { out.push_back(bar + " " + std::string(static_cast<size_t>(inner), ' ') + " " + bar); };

    // text + style: 0 plain, 1 section header, 2 cursor row
    std::vector<std::pair<std::string, int>> lines;
    int cursor = -1; // index of the selected line, for the two list tabs

    if (history_tab_ == 0) {
        const auto& plays = history_.plays();
        const int total = static_cast<int>(std::min<size_t>(100, plays.size()));
        const int when_w = 11, len_w = 7, state_w = 5;
        // WHEN, then TITLE and ARTIST in their own columns (two blanks between them, the same on the TOP TRACKS tab), the length and the state
        const std::string tsep = "  ";
        const int text_w = std::max(16, inner - when_w - 2 - len_w - 2 - state_w - static_cast<int>(tsep.size()));
        const int title_w = text_w * 3 / 5;
        const int artist_w = text_w - title_w;
        lines.push_back({T("WHEN", when_w) + "  " + T("TITLE", title_w) + tsep + T(audiobook_ ? "BOOK" : "ARTIST", artist_w) + L("LEN", len_w) + "  " + T("STATE", state_w), 1});
        if (total == 0) {
            lines.push_back({"nothing played yet -- plays show up here as soon as music runs", 0});
        } else {
            for (int i = 0; i < total; ++i) {
                const HistoryPlay& p = plays[i];
                std::string t = p.title;
                if (t.empty() || t == "-") t = p.id;
                const std::string a = (p.artist.empty() || p.artist == "-") ? std::string("-") : p.artist;
                const bool live = (i == 0 && history_.live());
                lines.push_back({T(format_when(p.started_at), when_w) + "  " + T(t, title_w) + tsep + T(a, artist_w) +
                                 L(format_mmss(p.len_sec), len_w) + "  " +
                                 T(live ? "PLAY" : (p.finished ? "done" : "skip"), state_w),
                                 0});
            }
            cursor = history_selected_ + 1; // +1 for the column header above the rows
        }
    } else if (history_tab_ == 1) {
        const int len_w = 7, plays_w = 7;
        // Title and Artist in their own columns (two blanks between them, the same on the HISTORY tab), then the length and the play count
        const std::string tsep = "  ";
        const int text_w = std::max(16, inner - len_w - 2 - plays_w - static_cast<int>(tsep.size()));
        const int title_w = text_w * 3 / 5;
        const int artist_w = text_w - title_w;
        lines.push_back({T("TITLE", title_w) + tsep + T(audiobook_ ? "BOOK" : "ARTIST", artist_w) + L("LEN", len_w) + "  " + std::string(static_cast<size_t>(std::max(0, plays_w - 7)), ' ') + "PLAYS " + (history_most_first_ ? "\u25bc" : "\u25b2"), 1});
        if (history_top_view_.empty()) {
            lines.push_back({"nothing played yet -- plays show up here as soon as music runs", 0});
        } else {
            for (int i = 0; i < static_cast<int>(history_top_view_.size()); ++i) {
                const HistoryTopRow& r = history_top_view_[static_cast<size_t>(i)];
                std::string t = r.title.empty() ? r.id : r.title;
                const std::string a = (r.artist.empty() || r.artist == "-") ? std::string("-") : r.artist;
                lines.push_back({T(t, title_w) + tsep + T(a, artist_w) + L(format_mmss(r.len_sec), len_w) + "  " +
                                 L(std::to_string(r.plays), plays_w), 0});
            }
            cursor = history_selected_ + 1; // +1 for the column header above the rows
        }
    } else if (history_tab_ == kHistTabPlaylists) {
        history_playlists_lines(inner, lines);
        cursor = -1; // no cursor: this tab scrolls like Habits
    } else {
        // --- Habits: three categories, each under a highlighted header ---
        const HistoryStats s = history_stats(history_.plays(), &history_.archive());
        const int lbl_w = std::max(20, inner - 20);
        auto num1 = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return std::string(b); };
        auto pct = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.0f", v * 100.0); return std::string(b); };
        auto cat = [&](const char* name) { if (!lines.empty()) lines.push_back({std::string(), 0}); lines.push_back({std::string(name), 1}); };
        auto item = [&](const std::string& label, const std::string& value) {
            lines.push_back({T(label, lbl_w) + value, 0});
        };
        cat("SESSIONS");
        item("Sessions", std::to_string(s.sessions));
        item("Average session length", format_len(s.avg_session_sec));
        item("Tracks per session", num1(s.tracks_per_session));
        cat("TIME PLAYED PER DAY");
        item("Played today", format_len(s.today_sec));
        item("Average day with music", format_len(s.avg_day_sec));
        item("Days with music", std::to_string(s.days));
        item("Total listened", format_len(s.total_sec));
        cat("PLAY BEHAVIOUR");
        item("Tracks played", std::to_string(s.plays));
        item("Finished (played to the end)", std::to_string(s.finished));
        item("Skipped", std::to_string(s.skipped));
        item("Replays", std::to_string(s.replays));
        item("Completion rate", pct(s.completion) + "%");
        cat("LISTENING BY HOUR OF THE DAY");
        {
            static const char* lv[9] = {" ", "\u2581", "\u2582", "\u2583", "\u2584", "\u2585", "\u2586", "\u2587", "\u2588"};
            double mx = 0;
            for (double v : s.hours) mx = std::max(mx, v);
            std::string bars = "  ", labs = "  ";
            for (int h = 0; h < 24; ++h) {
                const int l = mx > 0 ? static_cast<int>(std::ceil(s.hours[static_cast<size_t>(h)] / mx * 8.0 - 1e-9)) : 0;
                const std::string g = lv[std::clamp(l, 0, 8)];
                bars += " " + g + g + " ";
                char b[8]; std::snprintf(b, sizeof b, " %02d ", h);
                labs += b;
            }
            lines.push_back({bars, 0});
            lines.push_back({labs, 0});
        }
        cat("LISTENING BY WEEKDAY");
        {
            // The bar and the AVERAGE column show the average time on one such weekday (HistoryStats::weekday_avg), the
            // CUMULATIVE column the lifetime total. Only the two value columns get a name, in the header colour.
            static const char* wd[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
            const int day_w = 11, avg_w = 10, cum_w = 10;   // "AVERAGE" / "CUMULATIVE" and values like "123h 45m"
            const int bar_w = std::clamp(inner - (2 + day_w + 2 + avg_w + 2 + cum_w), 10, 40);
            const std::string lead(static_cast<size_t>(2 + day_w + bar_w + 2), ' ');
            lines.push_back({lead + T("AVERAGE", avg_w) + "  " + T("CUMULATIVE", cum_w), 1});
            double mx = 0;
            for (double v : s.weekday_avg) mx = std::max(mx, v);
            for (int d = 0; d < 7; ++d) {
                const double avg = s.weekday_avg[static_cast<size_t>(d)];
                const double cum = s.weekdays[static_cast<size_t>(d)];
                const int len = mx > 0 ? std::clamp(static_cast<int>(std::lround(avg / mx * bar_w)), 0, bar_w) : 0;
                std::string bars;
                for (int i = 0; i < len; ++i) bars += "\u2588";
                lines.push_back({"  " + T(wd[d], day_w) + bars + std::string(static_cast<size_t>(bar_w - len), ' ') + "  " +
                                 T(format_len(avg), avg_w) + "  " + T(format_len(cum), cum_w), 0});
            }
        }
        cursor = -1; // no cursor: this tab scrolls
    }

    // Window over the content: around the cursor on the two list tabs,
    // from history_scroll_ on Habits (whose scroll offset is clamped here,
    // where the real content height is finally known).
    int start = 0;
    if (cursor >= 0) {
        // Same behaviour as the file list: the column header stays put and the window only moves when the
        // cursor leaves it (no re-centering on every step).
        const int hdr = cursor - history_selected_;                      // fixed lines above the rows
        const int rows_total = static_cast<int>(lines.size()) - hdr;
        const int vis = std::max(1, body - hdr);
        int top = history_view_top_;
        if (history_selected_ < top) top = history_selected_;
        if (history_selected_ >= top + vis) top = history_selected_ - vis + 1;
        top = std::clamp(top, 0, std::max(0, rows_total - vis));
        history_view_top_ = top;
        lines.erase(lines.begin() + hdr, lines.begin() + hdr + top);     // scroll the rows, keep the header
        cursor -= top;
        start = 0;
    } else {
        const int max_scroll = std::max(0, static_cast<int>(lines.size()) - body);
        history_scroll_ = std::clamp(history_scroll_, 0, max_scroll);
        start = std::min(history_scroll_, max_scroll);
    }

    int shown = 0;
    for (int k = start; k < static_cast<int>(lines.size()) && shown < body; ++k, ++shown) {
        const std::string& text = lines[static_cast<size_t>(k)].first;
        const int style = lines[static_cast<size_t>(k)].second;
        if (style == 3) out.push_back(bar + " " + legend_sgr(settings_) + text + R + " " + bar);
        else if (style == 4) out.push_back(bar + " " + text + " " + bar);
        else if (style == 1) head(text);
        else if (style == 2 || (k == cursor && !(history_tab_ == 1 && history_pane_ == 1)))
            row(text, cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color));
        else row(text, "");
    }
    while (shown < body) { blank(); ++shown; }

    out.push_back(box_bottom(total_width, info, border_ansi_bottom));
    return out;
}

// TOP PLAYLISTS tab: the top 5 saved playlists of the week, month, quarter, year and overall in ONE pane. Wide
// terminals get two columns (week / month / quarter on the left, year / overall / summary on the right), narrow
// ones one column that scrolls like Habits. Style per line: 0 plain, 1 section header, 3 legend-coloured rule.
void App::history_playlists_lines(int inner, std::vector<std::pair<std::string, int>>& lines) const {
    struct Period { const char* title; long long days; };   // 0 days = overall
    static const Period kPeriods[5] = {
        {"TOP PLAYLISTS OF THE WEEK", 7}, {"TOP PLAYLISTS OF THE MONTH", 30}, {"TOP PLAYLISTS OF THE QUARTER", 91},
        {"TOP PLAYLISTS OF THE YEAR", 365}, {"TOP PLAYLIST OVERALL", 0}};
    const long long now = static_cast<long long>(std::time(nullptr));
    const bool two = inner >= 84;
    const std::string sep_txt = " " + (settings_.list_separator.empty() ? std::string("|") : settings_.list_separator) + " ";
    const int sep_w = display_width(sep_txt);
    const int col_w = two ? (inner - sep_w) / 2 : inner;
    const int plays_w = 9, time_w = 8;
    const int name_w = std::max(6, col_w - 3 - plays_w - time_w - 2);
    auto T = [](const std::string& s, int n) {
        std::string t = truncate_str(s, n);
        const int fill = n - display_width(t);
        return t + (fill > 0 ? std::string(static_cast<size_t>(fill), ' ') : std::string());
    };
    auto R = [](const std::string& s, int n) {
        const int w = display_width(s);
        return w >= n ? truncate_str(s, n) : std::string(static_cast<size_t>(n - w), ' ') + s;
    };
    // (in the audio book mode this tab lists the books: the word is swapped in every text)
    auto bk = [&](std::string t) {
        if (!audiobook_) return t;
        for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{{"PLAYLISTS", "BOOKS"}, {"PLAYLIST", "BOOK"}, {"Playlists", "Books"}, {"playlists", "books"}, {"playlist", "book"}}) {
            size_t pos = 0;
            while ((pos = t.find(from, pos)) != std::string::npos) { t.replace(pos, from.size(), to); pos += to.size(); }
        }
        return t;
    };
    auto section = [&](int p) {
        std::vector<std::pair<std::string, int>> out;
        out.push_back({T(bk(kPeriods[p].title), col_w), 1});
        const auto rows = history_top_playlists(history_.plays(), &history_.archive(),
                                                kPeriods[p].days > 0 ? now - kPeriods[p].days * 86400LL : 0, 5);
        for (size_t i = 0; i < 5; ++i) {
            if (i < rows.size()) {
                const auto& r = rows[i];
                out.push_back({std::to_string(i + 1) + ". " + T(r.name, name_w) + "  " +
                               R(std::to_string(r.plays) + (r.plays == 1 ? " play" : " plays"), plays_w) +
                               R(format_len(r.listened_sec), time_w), 0});
            } else {
                out.push_back({std::to_string(i + 1) + ". " + (i == 0 ? bk("-- no playlist played in this period --") : "-"), 3});
            }
        }
        return out;
    };
    std::vector<std::vector<std::pair<std::string, int>>> sec;
    for (int p = 0; p < 5; ++p) sec.push_back(section(p));

    // Summary block (fills the free spot of the right column).
    int n_playlists = 0;
    long long n_plays = 0;
    history_playlist_totals(history_.plays(), &history_.archive(), n_playlists, n_plays);
    const HistoryStats st = history_stats(history_.plays(), &history_.archive());
    std::vector<std::pair<std::string, int>> sum;
    sum.push_back({T("SUMMARY", col_w), 1});
    sum.push_back({T(bk("Playlists played"), col_w - 8) + R(std::to_string(n_playlists), 8), 0});
    sum.push_back({T(bk("Plays from playlists"), col_w - 8) + R(std::to_string(n_plays), 8), 0});
    sum.push_back({T("Share of all plays", col_w - 8) +
                   R(st.plays > 0 ? std::to_string(std::min(100, static_cast<int>(100.0 * static_cast<double>(n_plays) / st.plays + 0.5))) + "%" : "-", 8), 0});
    if (n_plays == 0) {
        sum.push_back({T(audiobook_ ? "Every chapter you play counts for its book." : "Queue a saved playlist (Enter on a playlist row) -- its plays are counted from then on.", col_w), 3});
    }

    auto append_col = [&](std::vector<std::pair<std::string, int>>& dst, const std::vector<std::vector<std::pair<std::string, int>>>& parts) {
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) dst.push_back({std::string(), 0});
            for (const auto& l : parts[i]) dst.push_back(l);
        }
    };
    if (!two) {
        append_col(lines, {sec[0], sec[1], sec[2], sec[3], sec[4], sum});
        return;
    }
    std::vector<std::pair<std::string, int>> left, right;
    append_col(left, {sec[0], sec[1], sec[2]});
    append_col(right, {sec[3], sec[4], sum});
    const size_t n = std::max(left.size(), right.size());
    const std::string Z = "\x1b[0m";
    const std::string sep = ansi_for(settings_.border_color, false) + sep_txt + Z;
    auto paint = [&](const std::pair<std::string, int>& l, int w) {
        const std::string t = pad_right(truncate_str(l.first, w), w);
        if (l.second == 1) return header_sgr(settings_) + t + Z;
        if (l.second == 3) return legend_sgr(settings_) + t + Z;
        return t;
    };
    const std::pair<std::string, int> none{std::string(), 0};
    const int right_w = inner - sep_w - col_w;
    for (size_t i = 0; i < n; ++i)   // style 4 = already painted, exactly `inner` columns wide
        lines.push_back({paint(i < left.size() ? left[i] : none, col_w) + sep +
                         paint(i < right.size() ? right[i] : none, right_w), 4});
}

// Top Tracks tab, second pane: ADD SMART HISTORY TO QUEUE, a 4 x 4 grid of
// lists (kSmartLabels) under a row of column titles, the columns split by the
// list separator in the border colour. The cursor is only drawn while this
// pane has focus (history_pane_ == 1).
std::vector<std::string> App::build_history_add_panel(int total_width, int height) const {
    const int inner = std::max(20, total_width - 4);
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    const std::string R = "\x1b[0m";
    std::vector<std::string> out;
    out.push_back(box_top(std::string("ADD SMART HISTORY TO QUEUE") + (history_pane_ == 1 ? focus_indicator(settings_, border_ansi) : ""),
                          total_width, border_ansi));

    const std::string sep_txt = " " + (settings_.list_separator.empty() ? std::string("|") : settings_.list_separator) + " ";
    const int sep_w = display_width(sep_txt);
    const int col_w = std::max(4, (inner - 3 * sep_w) / 4);
    const int last_w = std::max(4, inner - 3 * sep_w - 3 * col_w);   // the last column takes the rounding rest
    const std::string sep = border_ansi + sep_txt + R;
    const bool focused = (history_pane_ == 1);
    const int played = static_cast<int>(history_top_view_.size()); // distinct titles, whatever the sort
    for (int i = 0; i < std::max(1, height); ++i) {
        std::string line;
        for (int c = 0; c < 4; ++c) {
            const int w = (c == 3) ? last_w : col_w;
            std::string text, sgr;
            if (i == 0) {
                text = kSmartColTitles[c];
                sgr = header_sgr(settings_);
            } else if (i <= 4) {
                const int row = i - 1;
                text = kSmartLabels[c][row];
                if (c == 0 && played < kHistoryAddCounts[row])
                    text += played == 0 ? " (none yet)" : " (" + std::to_string(played) + ")";
                if (focused && c == history_add_col_ && row == history_add_sel_)
                    sgr = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            }
            if (c > 0) line += sep;
            line += sgr + pad_right(truncate_str(text, w), w) + R;
        }
        out.push_back(bar + " " + line + " " + bar);
    }
    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}


// ---------------------------------------------------------------------------
// READ LYRICS (Mode::ReadLyrics, CTRL+L)
//
// A full-screen reader for the lyrics saved for a track. Two one-line panes on top: READ LYRICS (the lyrics files of
// the track) and TRANSLATION (the translations saved for the chosen file); TAB moves the keys between them. Below:
// the plain lyrics of the chosen file, timestamps removed, centred. `t` opens the translate overlay (service, language
// to translate into, language of the lyrics) and ENTER there fetches a translation, which is saved in
// <track folder>/lyrics/translations/ and shown in "( )" right below every original line. SHIFT+t only shows / hides
// the saved translations (and steps through them when there are several).
//
// A translation takes a while. It runs on a worker thread that does not depend on the overlay: it goes on when the
// translate overlay is closed, when another file is chosen and even when READ LYRICS is closed (it is saved when it is
// done). Asking for a second one meanwhile is answered with a warning.
// ---------------------------------------------------------------------------

std::string n_files_label(size_t n) {
    return n == 0 ? std::string() : std::to_string(n) + (n == 1 ? " lyrics file" : " lyrics files");
}


} // namespace muisc
