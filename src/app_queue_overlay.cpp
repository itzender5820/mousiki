// app_queue_overlay.cpp
// Part of class App (src/app.h): The big queue overlay, floating panels and the bulk add panel.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "app_state_adev.h"
#include "app_state_bulk.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "book_art.h"
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
#include "app_internal.h"
#include "update_check.h"
#include "open_url.h"
#include "app_settings_tables.h"
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
#include <unordered_map>
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

namespace muisc {


// ---------------------------------------------------------------------
// Big queue overlay (HKeyQueueOverlay, SHIFT+k) -- see app.h for the design.
// ---------------------------------------------------------------------

// Shown in the same modes as the list overlay (the ones with the main UI
// underneath); in the full-screen takeovers the flag just stays set.
bool App::queue_overlay_active() const {
    if (!qst_.overlay_open) return false;
    if (mode_ == Mode::AudioDevice) return !adev_->from_settings;
    switch (mode_) {
        case Mode::Browse: case Mode::Search: case Mode::BulkAdd:
        case Mode::RetryLyrics: case Mode::ClearQueue: case Mode::UpdateNotice: case Mode::QueueToPlaylist: case Mode::OsciMenu: case Mode::SpectroMenu: case Mode::NormMenu: case Mode::BarsMenu: case Mode::Equalizer: case Mode::SleepTimer: case Mode::MenuIndex: case Mode::Creator: case Mode::LyricsEdit: case Mode::BookOverlay: case Mode::VideoMenu: case Mode::VideoLink: case Mode::VideoSearch: return true;
        default: return false;
    }
}

void App::queue_overlay_geometry(int W, int& panel_w, int& queue_rows) const {
    panel_w = std::min(W, std::max(40, std::min(W - 6, 160)));
    // Rows 2 .. term_rows_-1 belong to the panel: queue box border (2) + the queue rows.
    // ...minus the key legend lines below the frame (see list_overlay_geometry()).
    const int legend = static_cast<int>(queue_overlay_legend(panel_w).size());
    queue_rows = std::max(1, term_rows_ - 2 + overlay_legend_shift(legend) - kQueueOverlayChromeRows - kQueueSearchRows - legend);
}

void App::queue_overlay_open() {
    if (convert_overlay_open_) convert_overlay_close();
    folder_overlay_open_ = false; // the overlays are mutually exclusive
    if (list_overlay_open_) list_overlay_close(); // mutually exclusive with the big list overlay
    // Remember the focus to give it back on close. Coming from the list
    // overlay the queue was unfocused (list_overlay_open() forces that), so
    // closing the queue overlay then lands on the list again.
    qst_.overlay_prev_focus = qst_.focus;
    qst_.overlay_open = true;
    qst_.focus = true; // arrow keys, d, 4/5 and a all act on the queue while it is open
    clamp_queue_selected();
}

void App::queue_overlay_close() {
    qst_.overlay_open = false;
    qst_.find_edit = false;
    qst_.find.clear();
    text_disengage();
    qst_.focus = qst_.overlay_prev_focus;
    // Both panes share qst_.selected/qst_.scroll: pull the window back so
    // the cursor is inside the small pane's (much shorter) view again.
    clamp_queue_selected();
}

// ---- the search bar of the queue overlay ---------------------------------------------------------------------------
// A queue entry matches when the query is found (the same typo tolerant search as the library's) in its title, artist, album,
// year, file name, the names of its folders, the tags the program has read for the lists and EVERY other tag of the file
// (album artist, genre, composer, comment, ...: also the ones no screen shows; read in the background, see queue_all_tags_resolve()).
bool App::queue_find_match(const std::string& query, size_t idx) const {
    if (idx >= queue_.size() || query.empty()) return false;
    const auto& q = queue_[idx];
    std::string hay = (q.is_local ? list_row_title(q.local_path, q.title) : q.title) + " " + q.artist;
    if (q.is_local) {
        const std::string key = path_utf8(q.local_path);
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            const auto it = row_meta_cache_.find(key);
            if (it != row_meta_cache_.end()) hay += " " + it->second.artist + " " + it->second.title + " " + it->second.album + " " + it->second.year;
            const auto all = qst_.all_tags.find(key);
            if (all != qst_.all_tags.end()) hay += " " + all->second;
        }
        hay += " " + path_utf8(q.local_path.stem());
        const fs::path dir = q.local_path.parent_path();
        hay += " " + path_utf8(dir.filename()) + " " + path_utf8(dir.parent_path().filename());
    }
    return fuzzy_score(query, with_latin_spelling(hay)) > 0.0;
}

void App::queue_all_tags_resolve() {
    auto paths = std::make_shared<std::vector<fs::path>>();
    for (const auto& q : queue_) {
        if (!q.is_local) continue;
        if (qst_.all_tags_asked.insert(path_utf8(q.local_path)).second) paths->push_back(q.local_path);
    }
    if (paths->empty()) return;
    spawn_bg("queue tags", [this, paths]() {
        for (const fs::path& p : *paths) {
            if (bg_->quit.load()) return;   // quitting
            std::string text = probe_all_tag_text(p);
            {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                qst_.all_tags[path_utf8(p)] = std::move(text);
            }
            row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);   // the screen is drawn again with the new matches
        }
    });
}

int App::queue_find_count() const {
    int n = 0;
    for (size_t i = 0; i < queue_.size(); ++i) if (queue_find_match(qst_.find, i)) ++n;
    return n;
}

void App::queue_find_jump(int dir) {
    const int total = static_cast<int>(queue_.size());
    if (total == 0 || qst_.find.empty()) return;
    const int step = dir < 0 ? -1 : 1;
    for (int k = (dir == 0 ? 0 : 1); k <= total; ++k) {
        const int i = ((qst_.selected + step * k) % total + total) % total;
        if (queue_find_match(qst_.find, static_cast<size_t>(i))) {
            qst_.selected = i;
            // keep the cursor inside the window
            const int rows = std::max(1, overlay_queue_rows_);
            if (qst_.selected < qst_.scroll) qst_.scroll = qst_.selected;
            else if (qst_.selected >= qst_.scroll + rows) qst_.scroll = qst_.selected - rows + 1;
            clamp_queue_selected();
            return;
        }
    }
    status_line_ = "no title of the queue matches";
}

// Keys while the caret is in the bar: typing searches at once, Up / Down go to the previous / next match, ENTER leaves the bar
// (the matches stay marked), ESC clears the search.
void App::queue_find_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (key == 27) { qst_.find.clear(); qst_.find_edit = false; text_disengage(); edit_caret_ = edit_anchor_ = 0; return; }
    if (key == '\r' || key == '\n') { qst_.find_edit = false; text_disengage(); return; }
    if (arrow && (key == 'A' || key == 'B')) { queue_find_jump(key == 'A' ? -1 : 1); return; }
    edit_focus("queue-find", qst_.find);
    text_engage("queue-find");
    if (edit_text_key(qst_.find, edit_caret_, edit_anchor_, key, 120, &status_line_)) {
        if (!qst_.find.empty()) queue_all_tags_resolve();   // (every tag of the queue's files, once)
        queue_find_jump(0);
    }
}

// One page up/down; same behaviour as list_overlay_page(): the cursor keeps
// its row within the window, and at either end it jumps to the last/first entry.
void App::queue_overlay_page(int dir) {
    const int total = static_cast<int>(queue_.size());
    if (total <= 0) return;
    const int rows = std::max(1, overlay_queue_rows_);
    const int max_scroll = std::max(0, total - rows);
    const int rel = std::clamp(qst_.selected - qst_.scroll, 0, rows - 1);
    const int new_scroll = std::clamp(qst_.scroll + dir * rows, 0, max_scroll);
    if (new_scroll == qst_.scroll) {
        qst_.selected = (dir > 0) ? total - 1 : 0;
    } else {
        qst_.scroll = new_scroll;
        qst_.selected = std::clamp(new_scroll + rel, 0, total - 1);
    }
    clamp_queue_selected();
}

// The normal build_queue_panel() (same rows and colours) in the wider/taller
// box; only its bottom border is replaced, to carry the page counter, the
// number of tracks; the key legend follows below the frame (see build_list_overlay_panel()).
std::vector<std::string> App::build_queue_overlay_panel(int panel_w, int queue_rows) const {
    std::vector<std::string> out = build_queue_panel(panel_w, queue_rows, true);
    if (!out.empty()) {
        const int total = static_cast<int>(queue_.size());
        const int rows = std::max(1, queue_rows);
        const int pages = std::max(1, (total + rows - 1) / rows);
        const int page = std::clamp((qst_.scroll + rows / 2) / rows + 1, 1, pages);
        const int remaining = total - (qst_.scroll + rows);

        std::vector<std::string> parts; // dropped from the back until it fits
        if (remaining > 0) parts.push_back("( " + std::to_string(remaining) + " more )");
        parts.push_back("page " + std::to_string(page) + "/" + std::to_string(pages));
        parts.push_back(std::to_string(total) + (total == 1 ? " track" : " tracks"));
        std::string footer;
        for (;;) {
            footer.clear();
            for (size_t i = 0; i < parts.size(); ++i) footer += (i ? "  " : "") + parts[i];
            if (parts.empty() || display_width(footer) + 5 <= panel_w) break;
            parts.pop_back();
        }
        if (parts.empty()) footer.clear();
        out.back() = box_bottom(panel_w, footer, ansi_for(settings_.border_color_bottom, false));
        // The search bar: its own small box under the queue (the list overlay has its search at the top).
        {
            const std::string border = ansi_for(settings_.border_color, false);
            const std::string bar = border + settings_.box_vertical + "\x1b[0m";
            const int inner = panel_w - 4;
            out.push_back(box_top(std::string("SEARCH QUEUE") + (qst_.find_edit ? focus_indicator(settings_, border) : std::string()), panel_w, border));
            const std::string prefix = "/ ";
            const int field_w = std::max(4, inner - display_width(prefix) - 14);
            std::string content;
            int cols = display_width(prefix);
            if (qst_.find_edit) {
                EditPaint p = paint_edit_field(qst_.find, edit_caret_, edit_anchor_, field_w, qst_.find.empty() ? search_idle_sgr(settings_) : search_sgr(settings_), true);
                content = search_idle_sgr(settings_) + prefix + search_run(settings_, p.s, !qst_.find.empty()) + "\x1b[0m";
                cols += p.cols;
            } else {
                const std::string plain = truncate_str(qst_.find.empty() ? std::string("(press [/] to search the queue)") : qst_.find, field_w);
                content = search_idle_sgr(settings_) + prefix + search_run(settings_, plain, !qst_.find.empty()) + "\x1b[0m";
                cols += display_width(plain);
            }
            std::string hits;
            if (!qst_.find.empty()) { const int n = queue_find_count(); hits = std::to_string(n) + (n == 1 ? " match" : " matches"); }
            const int gap = std::max(0, inner - cols - display_width(hits));
            out.push_back(bar + " " + content + ansi_for(settings_.list_color, false) + std::string(static_cast<size_t>(gap), ' ') + hits + "\x1b[0m " + bar);
            out.push_back(box_bottom(panel_w, "", ansi_for(settings_.border_color_bottom, false)));
        }
        // Key legend: below the frame, outside it, in the legend color.
        {
            const auto lrows = queue_overlay_legend(panel_w);
            for (size_t i = 0; i < lrows.size(); ++i) out.push_back(paint_legend(settings_, lrows, i, ansi_for(settings_.border_color, false), panel_w, false));
        }
    }
    return out;
}

// ---------------------------------------------------------------------
// Floating panels (Bulk Add, Retry Lyrics) -- see app.h's comment on
// draw_floating_panel() for why these don't clear the screen.
// ---------------------------------------------------------------------

// BorderGradient=smooth for a panel that is stamped onto the screen (absolute cursor moves, so it never passes through
// clamp_output_rows()): its own border rows fade from the top to the bottom border colour, top row to bottom row.
std::vector<std::string> App::panel_gradient(const std::vector<std::string>& lines) const {
    if (!settings_.border_smooth || lines.size() < 2) return lines;
    static const std::vector<std::string> kGlyphs = {"\u2502", "\u2500", "\u256d", "\u256e", "\u2570", "\u256f", "\u251c", "\u2524", "\u252c", "\u2534", "\u253c", "\u250c", "\u2514"};
    std::vector<std::string> glyphs = kGlyphs;
    for (const std::string* g : {&settings_.box_upper_left, &settings_.box_upper_right, &settings_.box_lower_left,
                                 &settings_.box_lower_right, &settings_.box_vertical, &settings_.box_horizontal})
        if (!g->empty()) glyphs.push_back(*g);
    const std::string top = ansi_for(settings_.border_color, false), bottom = ansi_for(settings_.border_color_bottom, false);
    return border_gradient::apply(lines, top, bottom, glyphs, [&](float t) {
        return gradient_ansi(settings_.border_color, settings_.border_color_bottom, t, false);
    });
}

void App::draw_floating_panel(std::ostringstream& frame, const std::vector<std::string>& lines_in, int panel_w, int W, int fixed_col, bool exact_center, int fixed_row) const {
    const std::vector<std::string> lines = panel_gradient(lines_in);
    int panel_h = static_cast<int>(lines.size());
    int start_col = fixed_col > 0 ? fixed_col : 1 + std::max(0, (W - panel_w) / 2);
    int start_row = 1 + std::max(0, (term_rows_ - panel_h) / 2 - (exact_center ? 0 : kFloatingPanelUpShift));
    if (fixed_row > 0) start_row = fixed_row;
    start_row = std::clamp(start_row, 1, std::max(1, term_rows_ - panel_h));
    float_rect_[0] = start_col - 1; float_rect_[1] = start_row - 1; float_rect_[2] = panel_w; float_rect_[3] = panel_h;   // 0-based, for the scope image
    for (int i = 0; i < panel_h; ++i) {
        frame << "\x1b[" << (start_row + i) << ";" << start_col << "H" << lines[i];
    }
}

std::vector<std::string> App::build_bulk_add_panel() const {
    const int W = kBulkAddPanelWidth;   // 62, matches the reference design
    const int inner_w = W - 2;          // 60 -- nested box width, flush against the outer border (no gap)
    std::string border = ansi_for(settings_.border_color, false);
    std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    auto wrap = [&](const std::string& inner_line) { return obar + inner_line + obar; };

    std::vector<std::string> lines;
    lines.push_back(box_top("BULK ADD", W, border));

    // --- input box (nested) ---
    lines.push_back(wrap(box_top("", inner_w, border)));
    if (bulk_->add_buffer.empty() && !bulk_->add_results_ready) {
        lines.push_back(wrap(box_line("//:paste yt playlist link here", inner_w, border)));
    } else {
        const bool mine = edit_owner_ == "bulk-add";
        const size_t cc = mine ? std::min(edit_caret_, bulk_->add_buffer.size()) : bulk_->add_buffer.size();
        const size_t aa = mine ? std::min(edit_anchor_, bulk_->add_buffer.size()) : bulk_->add_buffer.size();
        const EditPaint p = paint_edit_field(bulk_->add_buffer, cc, aa, std::max(4, inner_w - 4), "", !bulk_->add_results_ready);
        lines.push_back(wrap(box_line_field("", p.s, p.cols, inner_w, border)));
    }
    lines.push_back(wrap(box_bottom(inner_w, "", border)));

    // --- results box (nested) -- always present at a fixed row count so
    // the panel's total footprint never changes between phase 1 (typing)
    // and phase 2 (results in) -- unused rows are just blank, not
    // omitted, which is what keeps draw_floating_panel()'s fixed-
    // rectangle overwrite artifact-free without ever needing a clear. ---
    lines.push_back(wrap(box_top("", inner_w, border)));

    const auto& items = pending_bulk_add_.items;
    int total = static_cast<int>(items.size());
    int show = bulk_->add_results_ready ? std::min(total, kBulkAddVisibleRows) : 0;
    int scroll = bulk_->add_results_ready ? std::clamp(bulk_->add_scroll, 0, std::max(0, total - show)) : 0;

    // Column widths measured directly from the reference design at its
    // 56-column content width (inner_w - 4): title=27, channel=15, the
    // rest is mark + " | " separators + the unpadded time text.
    const int title_w = 27, chan_w = 15;

    for (int r = 0; r < kBulkAddVisibleRows; ++r) {
        if (r >= show) { lines.push_back(wrap(box_line("", inner_w, border))); continue; }
        int idx = scroll + r;
        const auto& it = items[idx];
        bool starred = idx < static_cast<int>(bulk_->add_selected.size()) && bulk_->add_selected[idx];
        bool hovering = idx == bulk_->add_cursor;

        std::string mark = starred ? "*" : " ";
        std::string title = truncate_str(it.title, title_w);
        std::string chan = truncate_str(it.uploader.empty() ? "-" : it.uploader, chan_w);
        std::string time_str = "-";
        if (it.duration_sec >= 0) {
            int secs = static_cast<int>(it.duration_sec);
            time_str = std::to_string(secs / 60) + ":" + (secs % 60 < 10 ? "0" : "") + std::to_string(secs % 60);
        }

        std::string line = mark + " | " + pad_right(title, title_w) + " | " + pad_right(chan, chan_w) + " | " + time_str;
        // Built plain first, then box_line() pads/truncates it (its
        // truncate_str/pad_right count raw bytes, not display columns --
        // they don't skip ANSI escapes), and only *after* that do we
        // splice the reverse-video hover highlight into the already-
        // finished bar+content+bar string. Wrapping `line` in "\x1b[7m"
        // before handing it to box_line() would get its own length
        // miscounted against the ANSI bytes and risk truncate_str
        // slicing straight through the trailing "\x1b[0m" reset,
        // leaking reverse-video onto every line after it -- exactly the
        // bug build_list_panel avoids by coloring after padding, not
        // before (see its own hover/cursor rendering).
        std::string boxed = box_line(line, inner_w, border);
        if (hovering) {
            std::string vbar_len = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
            size_t start = vbar_len.size() + 1; // past "bar + ' '"
            size_t end = boxed.size() - vbar_len.size() - 1; // before "' ' + bar"
            boxed.insert(end, "\x1b[0m");
            boxed.insert(start, "\x1b[7m");
        }
        lines.push_back(wrap(boxed));
    }

    // Bottom border of the results box carries two plain-text labels --
    // "[ N more ]" on the left (only once there's more than fits), "ALL"
    // and "SELECT" on the right as the two commit actions. Just text, no
    // button/tab border art.
    int more = bulk_->add_results_ready ? (total - show) : 0;
    std::string left_label = more > 0 ? ("[ " + std::to_string(more) + " more ]") : "";
    std::string right_label = bulk_->add_results_ready ? "ALL   SELECT" : "";
    std::string prefix = settings_.box_lower_left + settings_.box_horizontal;
    if (!left_label.empty()) prefix += " " + left_label + " ";
    std::string suffix = right_label.empty() ? "" : (" " + right_label + " ");
    suffix += settings_.box_lower_right;
    int used = display_width(prefix) + display_width(suffix);
    int dashes = std::max(0, inner_w - used);
    std::string bottom = prefix;
    for (int i = 0; i < dashes; ++i) bottom += settings_.box_horizontal;
    bottom += suffix;
    bottom = pad_right(bottom, inner_w);
    lines.push_back(wrap(border.empty() ? bottom : (border + bottom + "\x1b[0m")));

    // The two options (CTRL+s / CTRL+o), one row under the results box: the panel keeps one fixed height in both phases.
    {
        const std::string opts = " [CTRL+s] Save: " + std::string(settings_.bulk_save ? "ON " : "OFF") + "    [CTRL+o] Video: " + (settings_.bulk_video ? "ON" : "OFF");
        lines.push_back(wrap(box_line(opts, inner_w, border)));
    }

    // Outer box's own bottom border carries "[ESC] cancel" directly.
    lines.push_back(box_bottom(W, "[ESC] cancel", border));
    return lines;
}

} // namespace muisc
