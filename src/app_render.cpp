// app_render.cpp
// Part of class App (src/app.h): Drawing a whole frame of the main screen (render_frame) and the pieces it is made of.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
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


// The Console and Settings overlays must always be exactly as tall as
// the Browse-mode player view -- see the comment on this declaration in
// app.h. This recomputes the same panel line counts render_frame()'s
// Browse-mode branch does, plus list_visible_rows_ (already kept
// up to date each frame -- see render_frame()) for the list/queue
// panel's share.
int App::player_view_height(int w) const {
    int h = static_cast<int>(build_metadata_panel(w).size());
    h += static_cast<int>(build_progress_panel(w).size());
    h += static_cast<int>(build_search_bar(w).size());
    // The list rows the Browse view would get on THIS terminal, computed from
    // term_rows_ right here instead of read from list_visible_rows_: that
    // member is only refreshed by Browse frames, so after a resize (going
    // full screen while a menu is open) it was one resize behind and the
    // overlays kept the old, shorter height. Same formula as render_frame():
    // the rows left under the chrome minus the box's 2 border rows and the
    // untouched last terminal row.
    h += std::max(0, term_rows_ - h - 1 - 2);
    h += 2; // the list/queue box's own top+bottom border rows (build_list_panel()/
            // build_queue_panel() add them ON TOP of the content rows they're given)
    h += 1; // the row Browse draws the AcoustID prompt / loading indicator in
            // (idle Browse no longer prints anything there -- the log moved to
            // Settings -- but overlays still size to this taller frame: it is
            // the worst case, and overlay_budget() caps the result at
            // term_rows_ - 1 anyway)
    return h;
}

// ---------------------------------------------------------------------
// Frame assembly
// ---------------------------------------------------------------------

// Full-screen modes (Settings, Console, Cheatsheet, Playlist, Meta editor, History) start their
// frame with "ESC[2J" (erase the whole screen). Erasing and repainting 25 times a second makes
// terminals flicker (WSL and ConPTY do this readily), so these modes repaint in place instead
// and erase only once, when the mode changes.
//
// Once the mode is already on screen (see hard_clear in render_frame()) the
// erase is not needed: this drops it and instead repaints in place, clearing
// only what is actually stale -- the rest of every line that is shorter than
// the terminal ("ESC[K"), and everything below the last line ("ESC[0J").
// Lines that already span the full width get no "ESC[K": with the cursor
// parked in the last column (pending wrap) it would erase that last cell.
static std::string soften_fullscreen_frame(const std::string& frame, int cols) {
    static const std::string kClear = "\x1b[2J";
    // Only valid for frames that are painted top-to-bottom with plain newlines.
    // Frames that place their content with absolute cursor moves ("ESC[row;colH", as the Settings
    // screen does) keep their full clear. Painted in place they would leave cells they no longer use
    // on screen, and a trailing "ESC[0J" would wipe a caret parked mid-screen (while a value is edited).
    {
        size_t i = (frame.compare(0, kClear.size(), kClear) == 0) ? kClear.size() : 0;
        if (frame.compare(i, 3, "\x1b[H") == 0) i += 3; // the plain cursor-home that follows the clear
        for (; i < frame.size(); ++i) {
            if (frame[i] != '\x1b' || i + 1 >= frame.size() || frame[i + 1] != '[') continue;
            size_t j = i + 2;
            while (j < frame.size() && !(static_cast<unsigned char>(frame[j]) >= 0x40 && static_cast<unsigned char>(frame[j]) <= 0x7E)) ++j;
            if (j < frame.size() && std::string("HfdGABCDEFsu").find(frame[j]) != std::string::npos) return frame;
            i = j;
        }
    }
    std::string in = frame;
    if (in.compare(0, kClear.size(), kClear) == 0) in.erase(0, kClear.size());

    // Only valid for frames painted top to bottom with plain newlines. A frame
    // that places text with absolute cursor moves ("ESC[row;colH" -- the
    // Settings screen does this for every cell) only overwrites the cells it
    // writes; whatever the previous tab/screen drew elsewhere would stay on
    // screen and the two would be mushed together. Those frames keep the full
    // erase (the identical-frame skip and the synchronized write still stop it
    // from flickering).
    for (size_t i = 0; i + 1 < in.size(); ++i) {
        if (in[i] != '\x1b' || in[i + 1] != '[') continue;
        size_t j = i + 2;
        bool has_digit = false;
        while (j < in.size() && ((in[j] >= '0' && in[j] <= '9') || in[j] == ';' || in[j] == '?')) {
            if (in[j] >= '0' && in[j] <= '9') has_digit = true;
            ++j;
        }
        if (j < in.size() && in[j] == 'H' && has_digit) return frame;
        i = j;
    }

    std::string out;
    out.reserve(in.size() + 256);
    size_t pos = 0;
    while (pos < in.size()) {
        size_t nl = in.find('\n', pos);
        if (nl == std::string::npos) {           // trailing partial line: "ESC[0J" below covers it
            out.append(in, pos, std::string::npos);
            break;
        }
        // Visible text of this line = everything except CSI escape sequences.
        std::string visible;
        visible.reserve(nl - pos);
        for (size_t i = pos; i < nl; ) {
            if (in[i] == '\x1b' && i + 1 < nl && in[i + 1] == '[') {
                i += 2;
                while (i < nl && !(static_cast<unsigned char>(in[i]) >= 0x40 && static_cast<unsigned char>(in[i]) <= 0x7E)) ++i;
                if (i < nl) ++i;                  // the final byte
            } else {
                visible += in[i++];
            }
        }
        out.append(in, pos, nl - pos);
        if (display_width(visible) < cols) out += "\x1b[K";
        out += '\n';
        pos = nl + 1;
    }
    out += "\x1b[0J";
    return out;
}

// `line` (text with colour codes, `line_w` columns) with `text` (`text_w` columns, its own colours) laid over it from
// column x: what is under the text is left out, the colours in effect after it carry on.
std::string App::overlay_cells(const std::string& line, int x, const std::string& text, int text_w, int line_w) {
    std::string out, sgr;   // sgr: the last colour code seen (re-applied after the text)
    int col = 0;
    size_t i = 0;
    auto next_char = [&](size_t k) {   // one UTF-8 character
        const unsigned char c = static_cast<unsigned char>(line[k]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        return std::min(line.size(), k + n);
    };
    auto copy_escape = [&](bool keep) {   // line[i] == ESC: one CSI sequence
        size_t j = i + 1;
        if (j < line.size() && line[j] == '[') { ++j; while (j < line.size() && !(line[j] >= '@' && line[j] <= '~')) ++j; }
        if (j < line.size()) ++j;
        const std::string seq = line.substr(i, j - i);
        if (!seq.empty() && seq.back() == 'm') sgr = seq == "\x1b[0m" ? std::string() : sgr + seq;
        if (keep) out += seq;
        i = j;
    };
    while (i < line.size() && col < x) {
        if (line[i] == '\x1b') { copy_escape(true); continue; }
        const size_t j = next_char(i);
        const int w = display_width(line.substr(i, j - i));
        if (col + w > x) break;
        out += line.substr(i, j - i);
        col += w;
        i = j;
    }
    out += std::string(static_cast<size_t>(std::max(0, x - col)), ' ');
    out += "\x1b[0m" + text;
    col = std::max(col, x);
    int skip = text_w;   // the columns under the text
    while (i < line.size() && skip > 0) {
        if (line[i] == '\x1b') { copy_escape(false); continue; }
        const size_t j = next_char(i);
        skip -= display_width(line.substr(i, j - i));
        i = j;
    }
    out += "\x1b[0m" + sgr + std::string(static_cast<size_t>(std::max(0, -skip)), ' ');
    out += line.substr(i);
    (void)line_w;
    return out;
}

// The karaoke overlay uses the lyrics engine whatever the main screen does: with the engine off there (".", Settings ->
// ANIMA/VIZ, Use Lyrics) nothing was fetched for the playing track, so the overlay starts the fetch itself -- once per
// track, also after a track change while it is open. The main screen's setting is left as it is.
void App::karaoke_ensure_lyrics() {
    if (!has_track_ || current_path_.empty()) return;
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (lyrics_path_ == current_path_) return;   // fetched (or being fetched) for this track
    }
    std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
    launch_lyrics_fetch(metadata_.name, artist, current_path_);
}

std::string App::render_frame(TerminalIO& term) {
    // SHIFT+p in the theme picker: the normal main screen with the highlighted theme, kept still (the first frame is stored and
    // handed out again; the caller skips identical frames) until a key closes it, the theme changes or the window is resized.
    if (theme_peek_ && !peek_busy_ && theme_sel_ >= 0 && theme_sel_ < static_cast<int>(theme_list_.size())) {
        if (peek_frame_.empty() || peek_sel_ != theme_sel_ || peek_cols_ != term.cols() || peek_rows_ != term.rows()) {
            const Settings saved = settings_;
            const Mode saved_mode = mode_;
            const int saved_ui = theme_ui_;
            const std::string saved_status = status_line_;
            theme_apply(settings_, theme_list_[static_cast<size_t>(theme_sel_)]);
            mode_ = Mode::Browse;
            theme_ui_ = 0;
            status_line_ = "THEME PREVIEW: " + theme_list_[static_cast<size_t>(theme_sel_)].name + "  (any key goes back)";
            peek_busy_ = true;
            force_redraw_ = true;
            try { peek_frame_ = render_frame(term); } catch (...) { peek_frame_.clear(); }
            peek_busy_ = false;
            settings_ = saved; mode_ = saved_mode; theme_ui_ = saved_ui; status_line_ = saved_status;
            peek_sel_ = theme_sel_; peek_cols_ = term.cols(); peek_rows_ = term.rows();
        }
        return peek_frame_;
    }
    frame_main_len_ = std::string::npos;   // set by the main screen: where its line part ends (the overlays follow)
    spectro_used_ = false;
    spectro_area_[0] = -1;
    pv_spectro_area_[0] = -1;
    pv_cover_pic_ = false;
    caption_rect_[2] = 0;
    caption_text_.clear();
    gfx_ok_ = false;          // set again at the very end of a Browse frame (the Settings / Console / ... screens return early and show no picture)
    gfx2_.active = false;
    gfx_.active = false;      // build_metadata_panel() switches it on again when the image style is drawn this frame
    float_rect_[2] = float_rect_[3] = 0;
    int term_cols = term.cols();
    // Was clamped to a minimum of 80 regardless of the real terminal
    // width -- on a narrower phone terminal (the screenshots suggest
    // something closer to 40-46 visible columns), every line rendered
    // here would already be wider than the physical screen and get
    // wrapped by the terminal itself before the next frame's cursor-home
    // redraw overwrites it. That reads exactly like "truncated mid-word"
    // or "garbled" text even though nothing in this file actually cut it
    // off -- the terminal did, one line later than expected. Lowering the
    // floor so the app actually renders to the real width instead of
    // always assuming at least 80 columns are available.
    int W = std::clamp(term_cols, 40, 200);

    // Real terminal row count -- see term_rows_'s comment in app.h for
    // the full story on why this now actually gets consulted. rows()
    // itself already falls back to a sane default (40) if the ioctl
    // fails, so no extra guarding needed here beyond a floor against
    // truly pathological values feeding into subtraction below.
    term_rows_ = std::max(term.rows(), 4);

    // Big list overlay (SHIFT+l): size it against the real terminal every
    // frame -- before anything below scrolls/paints the list -- so
    // list_nav_rows() (which every scroll-follows-the-cursor calculation
    // asks) already reflects the overlay's row count, and re-anchor the
    // window so a resize can never leave the cursor off-screen.
    int overlay_panel_w = 0;
    if (list_overlay_active()) {
        list_overlay_geometry(W, overlay_panel_w, overlay_list_rows_);
        list_overlay_fit_scroll(overlay_list_rows_);
    }
    if (convert_overlay_active()) {
        convert_overlay_poll();
        convert_overlay_geometry(W, overlay_panel_w, cv_rows_);
        const int n = convert_overlay_total();
        cv_sel_ = n > 0 ? std::clamp(cv_sel_, 0, n - 1) : 0;
        cv_scroll_ = std::clamp(cv_scroll_, 0, std::max(0, n - cv_rows_));
        if (cv_sel_ < cv_scroll_) cv_scroll_ = cv_sel_;
        if (cv_sel_ >= cv_scroll_ + cv_rows_) cv_scroll_ = cv_sel_ - cv_rows_ + 1;
    } else {
        convert_overlay_poll();   // a running conversion keeps reporting on the status line with the overlay closed
    }
    if (folder_overlay_active()) {
        folder_overlay_geometry(W, overlay_panel_w, fo_rows_);
        const int n = static_cast<int>(fo_view_.size());
        fo_sel_ = n > 0 ? std::clamp(fo_sel_, 0, n - 1) : 0;
        fo_scroll_ = std::clamp(fo_scroll_, 0, std::max(0, n - fo_rows_));
        if (fo_sel_ < fo_scroll_) fo_scroll_ = fo_sel_;
        if (fo_sel_ >= fo_scroll_ + fo_rows_) fo_scroll_ = fo_sel_ - fo_rows_ + 1;
    }
    // Big queue overlay (SHIFT+k): same per-frame sizing for the queue window.
    // (Never open together with the list overlay, so they can share overlay_panel_w.)
    if (queue_overlay_active()) {
        queue_overlay_geometry(W, overlay_panel_w, overlay_queue_rows_);
        clamp_queue_selected();
    }

    // Browse/BulkAdd/RetryLyrics all share the same live background (the
    // latter two float a small panel on top of it -- see
    // draw_floating_panel()'s comment in app.h), so switching between
    // them never needs a full clear, only a redraw. Settings/Console/
    // Cheatsheet are still genuine full-screen takeovers, so entering or
    // leaving any of *those* still forces one, same as a real mode
    // change always has.
    auto mode_family = [this](Mode m) {
        if ((m == Mode::LyricsEdit && lyrics_edit_from_reader_) || (m == Mode::RetryLyrics && rl_from_reader_)) return 9;   // (drawn on the READ LYRICS screen)
        if ((m == Mode::LyricsEdit && lyrics_edit_from_karaoke_) || (m == Mode::RetryLyrics && rl_from_karaoke_)) return 7;   // (drawn on the karaoke screen)
        if (m == Mode::MetaEdit && meta_sty_open_ && meta_sty_from_main_) return 0;   // the Style Option panel over the main screen
        if (m == Mode::AudioDevice) return adev_from_settings_ ? 1 : 0;     // over the settings screen or over the main screen
        switch (m) {
            case Mode::Browse: case Mode::Search: case Mode::BulkAdd: case Mode::RetryLyrics: case Mode::ClearQueue: case Mode::UpdateNotice: case Mode::QueueToPlaylist: case Mode::OsciMenu: case Mode::SpectroMenu: case Mode::NormMenu: case Mode::BarsMenu: case Mode::Equalizer: case Mode::SleepTimer: case Mode::MenuIndex: case Mode::Creator: case Mode::LyricsEdit: case Mode::BookOverlay: case Mode::AudioDevice: return 0;
            case Mode::Settings: case Mode::ColorEdit: return 1;
            case Mode::Console: return 2;
            case Mode::Cheatsheet: return 3;
            case Mode::Playlist: return 4;
            case Mode::MetaEdit: return 5;
            case Mode::CoverFetch: return 10;
            case Mode::History: return 6;
            case Mode::Karaoke: return 7;
            case Mode::Wizard: return 8;
            case Mode::ReadLyrics: return 9;
        }
        return 0;
    };
    // (the audio book's full-screen faces - notes, bookmarks, chapter marks - count too: closing them clears the whole screen, so no row of
    // their window or legend can stay behind in the main screen)
    const bool book_big_overlay = audiobook_ && mode_ == Mode::BookOverlay &&
        (book_ov_kind_ == BookOv::Notes || book_ov_kind_ == BookOv::Marks || book_ov_kind_ == BookOv::Chapters);
    const bool big_overlay_now = list_overlay_active() || convert_overlay_active() || folder_overlay_active() || queue_overlay_active() || book_big_overlay;
    const bool overlay_closed = last_big_overlay_ && !big_overlay_now;
    last_big_overlay_ = big_overlay_now;
    const bool force_was = force_redraw_;
    const Mode prev_mode_for_clear = last_render_mode_;
    bool hard_clear = overlay_closed || (W != last_render_w_) || (term_rows_ != last_render_rows_) || (mode_family(mode_) != mode_family(last_render_mode_)) || force_redraw_;
    if (last_render_w_ != -1 && W != last_render_w_) {
        // Verbose-only: raw ioctl terminal size alongside the clamped
        // app-usable width, i.e. "what the OS actually told us" versus
        // what we did with it.
        ConsoleLog::instance().log_verbose(
            "terminal resized: " + std::to_string(last_render_w_) + " -> " + std::to_string(W) +
            " cols (raw ioctl cols=" + std::to_string(term_cols) + ", rows=" + std::to_string(term_rows_) + ")");
    }
    force_redraw_ = false; // one-shot -- consumed by this frame
    last_render_w_ = W;
    last_render_rows_ = term_rows_;
    last_render_mode_ = mode_;
    // A pure size change of the plain Browse screen (no overlay, no picture on screen) is repainted in place: erasing the whole
    // screen first is what shows as the UI "jumping up, getting cut and redrawing" while a window is dragged. Everything else keeps
    // the full erase (pictures must be re-sent, other screens place cells absolutely).
    const bool size_only_change = hard_clear && !overlay_closed && !force_was && mode_family(mode_) == mode_family(prev_mode_for_clear) &&
        mode_ == Mode::Browse && !(gfx_ok_ && gfx_.active) && !creator_show_ && !list_overlay_active() && !convert_overlay_active() &&
        !folder_overlay_active() && !queue_overlay_active() && !book_big_overlay;
    const char* clear_prefix = (hard_clear && !size_only_change) ? "\x1b[2J\x1b[H" : "\x1b[H";
    if (hard_clear) cf_pic_key_.clear();   // the screen is wiped: a cover preview picture (meta editor, cover menus) must be sent again

    // The creator's preview (after "Create"): the oscilloscope music or the spectrogram of the new file over the whole screen
    if (creator_show_ && mode_ == Mode::Browse) {
        std::string out = build_creator_preview(W, clear_prefix);
        frame_main_len_ = out.size();
        gfx_ok_ = gfx_.active;
        return out;
    }

    // SHIFT+0: the oscilloscope over the whole screen, YT left and XY right (the options overlay SHIFT+o can sit on top of it)
    if (scope_big_ && (mode_ == Mode::Browse || mode_ == Mode::OsciMenu)) {
        std::string out = build_scope_big(W, clear_prefix);
        frame_main_len_ = out.size();
        if (mode_ == Mode::OsciMenu) {
            std::ostringstream floating;
            draw_floating_panel(floating, build_osci_menu_panel(), kOsciMenuPanelWidth, W, 0, false, 2);   // starts on row 2 and only grows downwards (braille / image style have different row counts)
            out += floating.str();
        } else if (scope_big_color_edit_) {
            std::ostringstream floating;
            draw_floating_panel(floating, build_scope_color_panel(), kScopeColorPanelWidth, W);
            out += floating.str();
        } else if (scope_metrics_open_) {
            std::ostringstream floating;
            draw_floating_panel(floating, build_scope_metrics_panel(), kScopeMetricsPanelWidth, W);
            out += floating.str();
        }
        // image style: a floating panel cuts a hole into the pictures it covers (their pixels there are cleared; Sixel also
        // keeps those cells), and a changed hole means new pictures
        gfx_ok_ = gfx_.active;
        for (int k = 0; k < 2 && gfx_.active; ++k) {
            GfxFrame& F = k == 0 ? gfx_ : gfx2_;
            int cut[4] = {-1, -1, -1, -1};
            const int fx = float_rect_[0], fy = float_rect_[1], fw = float_rect_[2], fh = float_rect_[3];
            if (fw > 0 && fh > 0) {
                const int c0 = std::max(fx, F.col), c1 = std::min(fx + fw, F.col + F.cols);
                const int r0 = std::max(fy, F.row), r1 = std::min(fy + fh, F.row + F.rows);
                if (c0 < c1 && r0 < r1) { cut[0] = c0 - F.col; cut[1] = r0 - F.row; cut[2] = c1 - c0; cut[3] = r1 - r0; }
            }
            if (cut[2] > 0) {
                F.cut_c = cut[0]; F.cut_r = cut[1]; F.cut_w = cut[2]; F.cut_h = cut[3];
                const int xa = F.cut_c * F.w / F.cols, xb = (F.cut_c + F.cut_w) * F.w / F.cols;
                const int ya = F.cut_r * F.h / F.rows, yb = (F.cut_r + F.cut_h) * F.h / F.rows;
                for (int y = ya; y < yb && y < F.h; ++y)
                    for (int x = xa; x < xb && x < F.w; ++x) {
                        F.level[static_cast<size_t>(y) * F.w + x] = 0;
                        if (!F.grid.empty()) F.grid[static_cast<size_t>(y) * F.w + x] = 0;
                    }
            } else F.cut_w = 0;
            if (!std::equal(cut, cut + 4, scope_cut_last_ + k * 4)) F.fresh = true;
            std::copy(cut, cut + 4, scope_cut_last_ + k * 4);
        }
        return out;
    }

    // SHIFT+u: the spectrogram over the whole screen (the options overlay SHIFT+i can sit on top of it)
    if (spectro_full_ && (mode_ == Mode::Browse || mode_ == Mode::SpectroMenu)) {
        std::string out = build_spectro_full(W, clear_prefix);
        frame_main_len_ = out.size();
        if (mode_ == Mode::SpectroMenu) {
            std::ostringstream floating;
            draw_floating_panel(floating, build_spectro_menu_panel(), kSpectroMenuPanelWidth, W);
            out += floating.str();
        }
        return out;
    }

    // Every overlay below lays itself out against player_view_height() --
    // the height of the Browse view it stands in for -- but that value can
    // exceed what actually fits: list_visible_rows_ is only recomputed
    // while Browse itself is being rendered, so anything that grows the
    // metadata panel while another mode is up (playback starting, for
    // instance) leaves it stale and pvH points past the bottom of the
    // screen. clamp_output_rows() keeps term_rows_ - 1 lines, so the
    // budget asked for here is min(pvH, term_rows_ - 1): an overlay laid
    // out taller than the screen would have its own footer -- hint line
    // plus status line -- chopped off on every single frame, which is the
    // same bug that would otherwise hide Browse's status line entirely.
    auto overlay_budget = [&](int w) {
        return std::min(player_view_height(w), std::max(1, term_rows_ - 1));
    };

    // Finishes a full-screen frame: clamp to the terminal height, and -- unless
    // this is the frame that switches the mode / resizes (hard_clear) -- repaint
    // in place instead of erasing the whole screen again (see
    // soften_fullscreen_frame()).
    auto fullscreen_out = [&](std::ostringstream& frame) {
        std::string clamped = clamp_output_rows(frame.str(), term_rows_);
        return hard_clear ? clamped : soften_fullscreen_frame(clamped, term_cols);
    };

    if (term_cols < 40) {   // narrower than any screen is drawn (W is never below 40): a note instead of a frame the terminal would wrap
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        const auto msg = wrap_lines("Window too narrow: Mousiki needs at least 40 columns.", std::max(1, term_cols), std::max(1, term_rows_ - 1));
        for (size_t i = 0; i < msg.size(); ++i) frame << "\x1b[" << (i + 1) << ";1H" << msg[i];
        return fullscreen_out(frame);
    }

    if (mode_ == Mode::Settings || mode_ == Mode::ColorEdit || (mode_ == Mode::AudioDevice && adev_from_settings_)) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_settings_screen(frame, W, overlay_budget(W));
        // The cover preview of the Settings screen is the one picture it may show; any other picture switched on while building the
        // screen (the colour tab's theme preview builds the main screen's disk) must stay off.
        if (!pv_cover_pic_) gfx_.active = false;
        gfx2_.active = false;   // (the main screen's second picture -- the oscilloscope next to a cover -- never belongs here)
        gfx_ok_ = gfx_.active;
        if (remote_ov_.open) remote_overlay_draw(frame, W);
        if (mode_ == Mode::AudioDevice) draw_floating_panel(frame, build_audio_device_panel(), kAudioDevicePanelWidth, W);
        return fullscreen_out(frame);
    }

    if (mode_ == Mode::Console) {
        capture_status();
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_console_screen(frame, W, overlay_budget(W));
        return fullscreen_out(frame);
    }

    if (mode_ == Mode::Cheatsheet) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_cheatsheet_screen(frame, W);
        return fullscreen_out(frame);
    }

    if (mode_ == Mode::Wizard) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_wizard_screen(frame, W);
        return fullscreen_out(frame);
    }

    // The karaoke screen.
    if (mode_ == Mode::Karaoke || (mode_ == Mode::LyricsEdit && lyrics_edit_from_karaoke_) || (mode_ == Mode::RetryLyrics && rl_from_karaoke_)) {
        karaoke_ensure_lyrics();   // the karaoke overlay always has the lyrics engine, whatever the main screen uses
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_karaoke_screen(frame, W, term_rows_);
        // The legend occupies the very last terminal row (it has no line break after it, so nothing scrolls): the clamp
        // is told about one more row than usual so it does not cut that row off.
        std::string out = clamp_output_rows(frame.str(), term_rows_ + 1);
        if (!hard_clear) out = soften_fullscreen_frame(out, term_cols);
        if (mode_ == Mode::LyricsEdit) {
            std::ostringstream floating;
            draw_floating_panel(floating, build_lyrics_edit_panel(), kLyricsEditPanelWidth, W);
            out += floating.str();
        }
        if (mode_ == Mode::RetryLyrics) {
            std::ostringstream floating;
            draw_floating_panel(floating, build_retry_lyrics_panel(), kRetryLyricsPanelWidth, W, 0, true);
            out += floating.str();
        }
        return out;
    }

    if (mode_ == Mode::Playlist) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_playlist_screen(frame, W, overlay_budget(W));
        std::string out = fullscreen_out(frame);
        if (playlist_export_open_) {
            // The export overlay floats on top (absolute cursor positions, no newlines) -- appended AFTER the row clamp,
            // which would otherwise cut it off with everything past the last screen row.
            std::ostringstream panel;
            const int pw = std::clamp(W - 10, 50, 100);
            draw_floating_panel(panel, build_playlist_export_panel(pw), pw, W);
            out += panel.str();
        }
        if (playlist_import_open_) { // same floating treatment as the export overlay
            std::ostringstream panel;
            const int pw = std::clamp(W - 10, 50, 100);
            draw_floating_panel(panel, build_playlist_import_panel(pw), pw, W);
            out += panel.str();
        }
        return out;
    }

    if (mode_ == Mode::CoverFetch) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        const int cf_budget = overlay_budget(W);   // (this builds the main screen's metadata panel, which switches the cover picture slots on)
        gfx_.active = false;
        gfx2_.active = false;
        build_cf_screen(frame, W, cf_budget);   // (switches gfx_ on when the cover is drawn as a real picture)
        std::string shown = fullscreen_out(frame);
        frame_main_len_ = shown.size();
        gfx_ok_ = gfx_.active;
        return shown;
    }

    if (mode_ == Mode::MetaEdit && !(meta_sty_open_ && meta_sty_from_main_)) {
        // The five field values are resolved right before drawing them:
        // reading a tag can promote into the shared cache, and the panel
        // builder itself is const (see meta_refresh_hover_values()).
        meta_refresh_hover_values();
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        const int meta_budget = overlay_budget(W);   // (this builds the main screen's metadata panel, which switches the picture slots on)
        gfx_.active = false;
        gfx2_.active = false;
        // Opened with the main UI's key (HKeyStyleOptions): only the Style Option panel, no meta editor behind it
        build_meta_screen(frame, W, meta_budget);   // (switches gfx_ on when the cover is drawn as a real picture)
        std::string shown = fullscreen_out(frame);
        frame_main_len_ = shown.size();
        gfx_ok_ = gfx_.active;
        if (meta_choice_open_) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_meta_choice_panel(), kMetaChoicePanelWidth, W);
            shown += panel.str();
        }
        if (meta_cov_open_) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_meta_cov_panel(W), kMetaCovPanelWidth, W);
            shown += panel.str();
            gfx_ok_ = gfx_.active;   // (the panel's preview is a real picture when [t] says so)
        }
        if (meta_sty_open_) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_meta_sty_panel(W), meta_sty_panel_width(W), W);
            shown += panel.str();
            gfx_ok_ = gfx_.active;   // (the preview is a real picture when the terminal can show one and [t] says so)
        }
        // Saving takes a moment for a few files and a while for hundreds: the panel only appears when it lasts longer than ~1 s.
        if (meta_save_.active && std::chrono::steady_clock::now() - meta_save_.started > std::chrono::milliseconds(1000)) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_meta_save_panel(), kMetaSavePanelWidth, W);
            shown += panel.str();
        }
        return shown;
    }

    if (mode_ == Mode::History) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_history_screen(frame, W, overlay_budget(W));
        return fullscreen_out(frame);
    }

    if (mode_ == Mode::ReadLyrics && rd_add_open_) {   // "add lyrics manually": a full-window screen of its own
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        draw_floating_panel(frame, build_readlyrics_add_panel(W), std::max(70, W), W, 1, true, 1);
        {   // the key legend under the panel
            const auto legend = build_readlyrics_add_legend(W);
            const int first_row = std::max(1, term_rows_ - 1 - static_cast<int>(legend.size()) + 1);
            for (size_t i = 0; i < legend.size(); ++i) frame << "\x1b[" << (first_row + static_cast<int>(i)) << ";1H" << legend[i];
        }
        return fullscreen_out(frame);
    }

    if (mode_ == Mode::ReadLyrics && rd_te_open_) {   // "edit / add translation": a full-window screen of its own
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        draw_floating_panel(frame, build_readlyrics_te_panel(W), std::max(70, W), W, 1, true, 1);
        {
            const auto legend = build_readlyrics_te_legend(W);
            const int first_row = std::max(1, term_rows_ - 1 - static_cast<int>(legend.size()) + 1);
            for (size_t i = 0; i < legend.size(); ++i) frame << "\x1b[" << (first_row + static_cast<int>(i)) << ";1H" << legend[i];
        }
        return fullscreen_out(frame);
    }

    if (mode_ == Mode::ReadLyrics || (mode_ == Mode::LyricsEdit && lyrics_edit_from_reader_) || (mode_ == Mode::RetryLyrics && rl_from_reader_)) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_readlyrics_screen(frame, W, overlay_budget(W));
        std::string shown = fullscreen_out(frame);
        if (mode_ == Mode::LyricsEdit) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_lyrics_edit_panel(), kLyricsEditPanelWidth, W);
            shown += panel.str();
        }
        if (mode_ == Mode::RetryLyrics) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_retry_lyrics_panel(), kRetryLyricsPanelWidth, W, 0, true);
            shown += panel.str();
        }
        if (rd_opts_open_) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_readlyrics_options_panel(), 66, W);
            shown += panel.str();
        }
        if (rd_exp_open_) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_readlyrics_export_panel(), 74, W);
            shown += panel.str();
        }
        if (rd_search_open_) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_readlyrics_search_panel(), 74, W);
            shown += panel.str();
        }
        return shown;
    }

    // --- Browse mode (and the background behind BulkAdd/RetryLyrics):
    // figure out how many list/queue rows actually fit before building
    // anything, so the panel is sized right the first time instead of
    // being built tall and then chopped.
    auto metadata_lines = build_metadata_panel(W);
    auto progress_lines = build_progress_panel(W);
    auto search_lines = build_search_bar(W);
    // The AcoustID confirmation raised by SHIFT+b lives in Browse's
    // status area too (there is no footer here), but the disclaimer alone is
    // 110 characters -- wider than many terminals -- so instead of trusting
    // one physical line to hold it (which would wrap in the terminal and
    // push the whole frame into a scroll), it is wrapped here and as many
    // rows as it actually needs are reserved for it, exactly like the single
    // loading indicator row -- and, since neither of them is there most of
    // the time, no row at all is reserved when both are absent (see below).
    std::vector<std::string> prompt_lines;
    if (meta_prompt_ != MetaPrompt::None && mode_ == Mode::Browse) {
        prompt_lines = wrap_lines(meta_fetch_disclaimer() + "   [Y]es   [N]o   [ESC] cancel",
                                  std::max(10, W - 2), 2);
    }
    // The ONLY things still drawn below the list box are the AcoustID
    // prompt (wrapped, so as many rows as it actually needs) or -- mutually
    // exclusive with it -- the live "resolving/downloading..." indicator.
    // Nothing else is printed under the list box: the status line lives on the Settings screen. A
    // row is reserved below the box only when the prompt or the live "resolving/downloading" note
    // is shown (status_rows counts exactly the rows printed there). The frame is then term_rows_ - 1
    // lines tall in every case, so there is no jitter and no scroll. The resolving/downloading note
    // itself is shown in the search box's top border (build_search_bar).
    int status_rows = static_cast<int>(prompt_lines.size());
    int fixed_h = static_cast<int>(metadata_lines.size() + progress_lines.size() + search_lines.size())
                + status_rows; // rows really drawn under the list box (0 when none are)
    // The budget counts the box's own top and bottom border rows (build_list_panel() and
    // build_queue_panel() add them to the content rows they are given). clamp_output_rows() keeps
    // term_rows_ - 1 lines, so the frame ends with the prompt or loading row, or with the list box's
    // bottom border when there is none. The last terminal row stays blank, and a trailing newline on
    // the final line can never scroll the screen.
    int available_for_list = term_rows_ - fixed_h - 1 - 2;
    // No upper cap: the list and queue panes take every row the terminal leaves, so a maximised
    // window is filled to its last row.
    list_visible_rows_ = std::max(0, available_for_list);

    ensure_visible_row_meta();

    std::ostringstream frame;
    frame << clear_prefix;

    for (auto& l : metadata_lines) frame << l << "\n";
    for (auto& l : progress_lines) frame << l << "\n";
    for (auto& l : search_lines) frame << l << "\n";

    int list_h = list_visible_rows_;
    if (settings_.element_queue || audiobook_) {   // (the audio book always shows its chapters)
        int list_w = W / 2;
        int queue_w = W - list_w; // exact 50/50, remainder (odd W) goes to queue
        auto list_lines = build_list_panel(list_w, list_h);
        auto queue_lines = build_queue_panel(queue_w, list_h);
        size_t rows = std::max(list_lines.size(), queue_lines.size());
        for (size_t i = 0; i < rows; ++i) {
            std::string l = (i < list_lines.size()) ? list_lines[i] : std::string(list_w, ' ');
            std::string r = (i < queue_lines.size()) ? queue_lines[i] : std::string(queue_w, ' ');
            frame << l << r << "\n";
        }
    } else {
        for (auto& l : build_list_panel(W, list_h)) frame << l << "\n";
    }

    // No blank separator above this last row any more: with the list box's
    // border rows now counted in available_for_list, dropping that row is
    // what actually leaves room for it (see the comment there -- it used to
    // be cut off by clamp_output_rows() every frame).
    //
    // The row is NOT the log/status line any more. status_line_ (written by
    // log_event() and by every action's own message -- "added to queue",
    // "SAVED", "searching online ...") was the one line in this frame that
    // was printed raw: never width-limited, so a message longer than W
    // wrapped in the terminal and added a physical row to an already
    // full-height frame. One row too many is all it takes for the frame to
    // scroll: the terminal's own scrollbar appears, that scrollbar costs
    // the last text column, and then *every* full-width box row of the next
    // frame wraps too -- the UI visibly shifts left and keeps scrolling.
    // The log now lives on the Settings screen instead (see the status line
    // at the bottom of build_settings_screen()), where a stray wrap is
    // cosmetic and never touches this frame. So nothing is written into this
    // slot when no prompt is up and nothing is loading -- and status_rows
    // (above) reserves nothing in that case either, which is what keeps the
    // frame at exactly term_rows_ - 1 lines instead of leaving one blank
    // line short of the bottom while the panes above lose a row to it. When
    // something IS drawn here, its rows are reserved up front, so the prompt
    // and the loading indicator keep a fixed position and the layout never
    // jitters.
    if (!prompt_lines.empty()) {
        // Same yellow-on-black as the meta menu's own confirmation footer.
        // wrap_lines() already cut these to W - 2, and the " "+...+" "
        // padding adds exactly the 2 columns that takes back, so the result
        // is exactly W wide -- this line can never wrap either.
        for (const auto& l : prompt_lines) frame << "\x1b[43;30m " << l << " \x1b[0m\n";
    }

    frame << "\x1b[0J";

    // Hard safety net on top of the list_visible_rows_ sizing above: even
    // if the fixed chrome alone (metadata+progress+search bar) is taller
    // than the terminal -- a case list_visible_rows_ can't do anything
    // about, since it only controls the list panel -- this guarantees
    // the actual byte stream handed to the terminal never contains more
    // rows than the terminal has, so it structurally cannot scroll no
    // matter what future panels/config combinations produce.
    std::string out = clamp_output_rows(frame.str(), term_rows_);
    if (size_only_change) out = soften_fullscreen_frame(out, W);
    frame_main_len_ = out.size();

    // Bulk Add / Retry Lyrics / Clear Queue: stamp their floating panel on top
    // of the still-live background just built above, rather than replacing
    // it. Uses absolute positioning (draw_floating_panel()), so it is simply
    // appended after the background's own sequential top-to-bottom writes --
    // whichever content lands on a given screen cell last in the stream
    // wins, and the panel is emitted after, so it draws over the background
    // wherever they overlap without needing a clear.
    //
    // It MUST be appended after the clamp above, not before: on a terminal
    // where the background already fills term_rows_ - 1 lines (any terminal
    // too short for the full-height list, e.g. 30 rows), clamp_output_rows()
    // cuts everything after the last kept newline -- which would include
    // the panel, so it never appeared while the mode still swallowed keys.
    // The panel adds no '\n' (absolute moves only) and draw_floating_panel()
    // already keeps it inside the screen, so it cannot cause a scroll.
    std::ostringstream floating;
    if (list_overlay_active()) {
        // Stamped first so the small forms (Retry Lyrics, Bulk Add, Clear
        // Queue) still float on top of it when they are opened from inside
        // the overlay. Absolute moves only -- no '\n', so it cannot scroll --
        // and rows past the last usable terminal row are dropped.
        const auto lines = panel_gradient(build_list_overlay_panel(overlay_panel_w, overlay_list_rows_));
        const int start_col = 1 + std::max(0, (W - overlay_panel_w) / 2);
        const int start_row = 2; // one background row stays visible above and below
        for (size_t i = 0; i < lines.size(); ++i) {
            const int row = start_row + static_cast<int>(i);
            if (row > term_rows_) break;   // the legend block may reach the last terminal row (see overlay_legend_shift())
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    } else if (convert_overlay_active()) {
        const auto lines = panel_gradient(build_convert_overlay_panel(overlay_panel_w, cv_rows_));
        const int start_col = 1 + std::max(0, (W - overlay_panel_w) / 2);
        const int start_row = 2;
        for (size_t i = 0; i < lines.size(); ++i) {
            const int row = start_row + static_cast<int>(i);
            if (row > term_rows_) break;   // the legend block may reach the last terminal row (see overlay_legend_shift())
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    } else if (folder_overlay_active()) {
        const auto lines = panel_gradient(build_folder_overlay_panel(overlay_panel_w, fo_rows_));
        const int start_col = 1 + std::max(0, (W - overlay_panel_w) / 2);
        const int start_row = 2;
        for (size_t i = 0; i < lines.size(); ++i) {
            const int row = start_row + static_cast<int>(i);
            if (row > term_rows_) break;   // the legend block may reach the last terminal row (see overlay_legend_shift())
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    } else if (queue_overlay_active()) {
        // Same placement as the list overlay; the queue pane's own rows live in it.
        const auto lines = panel_gradient(build_queue_overlay_panel(overlay_panel_w, overlay_queue_rows_));
        const int start_col = 1 + std::max(0, (W - overlay_panel_w) / 2);
        const int start_row = 2;
        for (size_t i = 0; i < lines.size(); ++i) {
            const int row = start_row + static_cast<int>(i);
            if (row > term_rows_) break;   // the legend block may reach the last terminal row (see overlay_legend_shift())
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    }
    if (mode_ == Mode::BulkAdd) {
        draw_floating_panel(floating, build_bulk_add_panel(), kBulkAddPanelWidth, W);
    } else if (mode_ == Mode::RetryLyrics) {
        draw_floating_panel(floating, build_retry_lyrics_panel(), kRetryLyricsPanelWidth, W, 0, true);
    } else if (mode_ == Mode::ClearQueue) {
        draw_floating_panel(floating, build_clear_queue_panel(), kClearQueuePanelWidth, W);
    } else if (mode_ == Mode::QueueToPlaylist) {
        draw_floating_panel(floating, build_queue_to_playlist_panel(), kQtpPanelWidth, W);
    } else if (mode_ == Mode::UpdateNotice) {
        draw_floating_panel(floating, build_update_panel(), kUpdatePanelWidth, W);
    } else if (mode_ == Mode::AudioDevice) {
        draw_floating_panel(floating, build_audio_device_panel(), kAudioDevicePanelWidth, W);
    } else if (mode_ == Mode::OsciMenu) {
        // Centred like the other overlays; a scope image under it is cropped to the columns that stay free.
        draw_floating_panel(floating, build_osci_menu_panel(), kOsciMenuPanelWidth, W, 0, false, 2);   // starts on row 2 and only grows downwards (braille / image style have different row counts)
    } else if (mode_ == Mode::SpectroMenu) {
        draw_floating_panel(floating, build_spectro_menu_panel(), kSpectroMenuPanelWidth, W);
    } else if (mode_ == Mode::BarsMenu) {   // under the upper pane: the spectrum bars (the bottom of its meta column) stay free
        draw_floating_panel(floating, build_bars_menu_panel(W), kBarsMenuPanelWidth, W, 0, false,
                            static_cast<int>(build_metadata_panel(W).size()) + 1);
    } else if (mode_ == Mode::NormMenu) {
        draw_floating_panel(floating, build_norm_menu_panel(), kNormMenuPanelWidth, W);
    } else if (mode_ == Mode::LyricsEdit) {
        draw_floating_panel(floating, build_lyrics_edit_panel(), kLyricsEditPanelWidth, W);
    } else if (mode_ == Mode::MenuIndex) {
        draw_floating_panel(floating, build_menu_index_panel(W), std::min(kMenuIndexPanelWidth, std::max(40, W)), W, 0, true);
    } else if (mode_ == Mode::SleepTimer) {
        draw_floating_panel(floating, build_sleep_timer_panel(), kSleepTimerPanelWidth, W);
    } else if (mode_ == Mode::BookOverlay) {
        { const int bw = book_ov_width(W); draw_floating_panel(floating, build_book_overlay_panel(bw), bw, W); }
    } else if (mode_ == Mode::Creator) {
        draw_floating_panel(floating, build_creator_panel(), kCreatorPanelWidth, W);
    } else if (mode_ == Mode::Equalizer) {
        draw_floating_panel(floating, build_eq_panel(), kEqPanelWidth, W);
    } else if (mode_ == Mode::MetaEdit) {   // the Style Option panel of the playing track (HKeyStyleOptions), over the main screen
        meta_refresh_hover_values();
        meta_sty_pic_ = 0;
        draw_floating_panel(floating, build_meta_sty_panel(W), meta_sty_panel_width(W), W);
    }
    out += floating.str();
    if (gfx_.active) {
        const bool floating_mode = mode_ != Mode::Browse && mode_ != Mode::Search;
        const bool covered_all = list_overlay_active() || queue_overlay_active() || folder_overlay_active() || convert_overlay_active();
        if (covered_all || (mode_family(mode_) != 0)) gfx_.active = false;
        else if (meta_sty_pic_ == 1 && mode_ == Mode::MetaEdit) {   // the preview of the Style Option panel (over the main screen) owns the picture slot: it lies inside the panel
            gfx_.crop = 0;
            gfx_.cut_w = 0;
        } else {
            gfx_.crop = 0;
            const int fx = float_rect_[0], fy = float_rect_[1], fw = float_rect_[2], fh = float_rect_[3];
            if (gfx_.plain) {
                // The audio book picture stays under a floating panel (a question, the "<" menu): only the pixels the panel covers
                // are left out. `gfx_full_level_` is the picture as made; the hole is cut into the copy that is sent.
                int cut[4] = {0, 0, 0, 0};
                if (floating_mode && fw > 0 && fy < gfx_.row + gfx_.rows && fy + fh > gfx_.row && fx < gfx_.col + gfx_.cols && fx + fw > gfx_.col &&
                    gfx_.cols > 0 && gfx_.rows > 0) {
                    cut[0] = std::clamp((fx - gfx_.col) * gfx_.w / gfx_.cols, 0, gfx_.w);
                    cut[2] = std::clamp((fx + fw - gfx_.col) * gfx_.w / gfx_.cols + 1, 0, gfx_.w);
                    cut[1] = std::clamp((fy - gfx_.row) * gfx_.h / gfx_.rows, 0, gfx_.h);
                    cut[3] = std::clamp((fy + fh - gfx_.row) * gfx_.h / gfx_.rows + 1, 0, gfx_.h);
                }
                // the same hole in cells, for Sixel pictures that paint or blank their cells (opaque / erase): not under the panel
                gfx_.cut_w = 0;
                if (cut[2] > cut[0] && cut[3] > cut[1]) {
                    const int c0 = std::max(fx, gfx_.col), c1 = std::min(fx + fw, gfx_.col + gfx_.cols);
                    const int r0 = std::max(fy, gfx_.row), r1 = std::min(fy + fh, gfx_.row + gfx_.rows);
                    gfx_.cut_c = c0 - gfx_.col; gfx_.cut_r = r0 - gfx_.row; gfx_.cut_w = c1 - c0; gfx_.cut_h = r1 - r0;
                }
                const bool changed = std::memcmp(cut, gfx_last_cut_, sizeof(cut)) != 0;
                const bool new_picture = gfx_.fresh || gfx_full_level_.size() != gfx_.level.size();
                if (new_picture) gfx_full_level_ = gfx_.level;
                else if (changed) gfx_.level = gfx_full_level_;
                if (new_picture || changed) {
                    for (int y = cut[1]; y < cut[3]; ++y)
                        std::fill(gfx_.level.begin() + static_cast<long>(y) * gfx_.w + cut[0], gfx_.level.begin() + static_cast<long>(y) * gfx_.w + cut[2], 0);
                    if (changed) gfx_.fresh = true;
                }
                std::memcpy(gfx_last_cut_, cut, sizeof(cut));
            } else
            if (floating_mode && fw > 0 && fy < gfx_.row + gfx_.rows && fy + fh > gfx_.row && fx < gfx_.col + gfx_.cols && fx + fw > gfx_.col) {
                // an overlay covers part of the picture: free the columns up to its right edge (all of it when it sits further right)
                gfx_.crop = fx <= gfx_.col ? std::clamp(fx + fw - gfx_.col, 0, gfx_.cols) : gfx_.cols;
            }
        }
        if (gfx_.active && gfx_.crop != gfx_last_crop_) gfx_.fresh = true;
        gfx_last_crop_ = gfx_.active ? gfx_.crop : -1;
        // the oscilloscope picture next to an album cover (second slot): a floating panel frees its columns as it does for gfx_
        if (gfx_.active && gfx2_.active) {
            gfx2_.crop = 0;
            const bool own_second = meta_sty_pic_ == 2 && mode_ == Mode::MetaEdit;   // the Style Option preview lies inside its own panel
            const bool floating_mode = !own_second && mode_ != Mode::Browse && mode_ != Mode::Search;
            const int fx = float_rect_[0], fy = float_rect_[1], fw = float_rect_[2], fh = float_rect_[3];
            if (floating_mode && fw > 0 && fy < gfx2_.row + gfx2_.rows && fy + fh > gfx2_.row && fx < gfx2_.col + gfx2_.cols && fx + fw > gfx2_.col)
                gfx2_.crop = fx <= gfx2_.col ? std::clamp(fx + fw - gfx2_.col, 0, gfx2_.cols) : gfx2_.cols;
            if (gfx2_.crop != gfx2_last_crop_) gfx2_.fresh = true;
            gfx2_last_crop_ = gfx2_.crop;
        } else gfx2_last_crop_ = -1;
    } else { gfx_last_crop_ = -1; gfx2_last_crop_ = -1; }
    gfx_ok_ = gfx_.active;
    return out;
}

} // namespace muisc
