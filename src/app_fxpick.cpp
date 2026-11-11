// app_fxpick.cpp
// Part of class App (src/app.h): the VIDEO EFFECT picker. ENTER on the Video Effect row of any menu (Settings, the VIDEO overlay, the VIDEO
// OPTIONS panel of the big video, the Style Option overlay) opens a panel beside it (so the menu it came from stays visible) with every effect
// in two columns and a preview column: the video (a test picture when none plays) with the highlighted effect on it. The arrow keys go through
// the list, ENTER takes the effect, ESC leaves it as it was. In the two per-track menus the first entry is "as in Settings" (-1), as it is the
// first stop of the left / right keys there.
#include "app.h"
#include "app_internal.h"
#include "cover_store.h"
#include "meta_state.h"
#include "overlay_rows.h"
#include "path_utf8.h"
#include "terminal_ui.h"
#include "video_fx.h"
#include "video_player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace muisc {

namespace {
constexpr int kFxPickCols = 2;
}

// the entries: (with `with_default`) -1 = as in Settings, then the effects in the order of the menus
std::vector<int> App::fxpick_entries() const {
    std::vector<int> e;
    if (fxpick_.with_default) e.push_back(-1);
    for (int i = 0; i < kVideoFxShown; ++i) e.push_back(kVideoFxOrder[i]);
    return e;
}

void App::fxpick_open(int target) {
    fxpick_ = FxPick{};
    fxpick_.target = target;
    fxpick_.with_default = target == 2 || target == 3;
    int cur = settings_.video_fx;
    if (target == 2) cur = cover_track_video_opts(path_utf8(current_.path)).fx;
    else if (target == 3 && meta_) cur = cover_track_video_opts(meta_->sty_track).fx;
    fxpick_.current = cur < 0 ? -1 : video_fx_canon(cur);
    const std::vector<int> e = fxpick_entries();
    fxpick_.cursor = 0;
    for (size_t i = 0; i < e.size(); ++i) if (e[i] == fxpick_.current) fxpick_.cursor = static_cast<int>(i);
    fxpick_.open = true;
    force_redraw_ = true;
}

void App::fxpick_apply(int fx) {
    const int target = fxpick_.target;
    if (target == 0) {   // Settings: the same route as a value typed into the row
        color_edit_buffer_ = video_fx_name(fx);
        edit_caret_ = edit_anchor_ = color_edit_buffer_.size();
        settings_commit_edit();
        status_line_ = std::string("video effect: ") + video_fx_name(settings_.video_fx);
    } else if (target == 1) {
        video_set_fx(fx);
    } else if (target == 2) {
        VideoTrackOpts o = cover_track_video_opts(path_utf8(current_.path));
        o.fx = fx;
        set_cover_track_video_opts(path_utf8(current_.path), o);
        video_pic_key_.clear();
        status_line_ = fx < 0 ? "video effect of this track: as in Settings" : std::string("video effect of this track: ") + video_fx_name(fx);
    } else if (target == 3 && meta_) {
        VideoTrackOpts o = cover_track_video_opts(meta_->sty_track);
        o.fx = fx;
        set_cover_track_video_opts(meta_->sty_track, o);
        video_pic_key_.clear();
        meta_->sty_msg = "saved";
    }
    force_redraw_ = true;
}

// true = the key was taken (the picker is open: it has every key)
bool App::fxpick_key(int key, bool arrow) {
    if (!fxpick_.open) return false;
    const std::vector<int> e = fxpick_entries();
    const int n = static_cast<int>(e.size());
    const int rows = (n + kFxPickCols - 1) / kFxPickCols;
    int& c = fxpick_.cursor;
    c = std::clamp(c, 0, n - 1);
    if (arrow) {
        if (key == 'A') c = (c + n - 1) % n;                                     // up: the entry above; from the top of a column to the bottom of the one before
        else if (key == 'B') c = (c + 1) % n;
        else if (key == 'C') { const int t = c + rows; c = t < n ? t : c % rows; }   // right: the next column (the same row), round to the first
        else if (key == 'D') { const int t = c - rows; c = t >= 0 ? t : std::min(n - 1, c + rows * (kFxPickCols - 1)); }   // left: the column before, round to the last
        else if (key == kKeyHome) c = 0;
        else if (key == kKeyEnd) c = n - 1;
    } else if (key == kKeyHome) c = 0;
    else if (key == kKeyEnd) c = n - 1;
    else if (key == '\r' || key == '\n') {
        const int fx = e[static_cast<size_t>(c)];
        fxpick_.open = false;
        fxpick_prev_key_.clear();
        fxpick_apply(fx);
    } else if (key == 27 || key == 'q' || key == 'Q') {
        fxpick_.open = false;
        fxpick_prev_key_.clear();
        video_pic_key_.clear();   // (a picture that was cut for the panel is sent whole again)
    }
    force_redraw_ = true;
    return true;
}

// The look the preview shows for the entry `fx` (-1: the effect of Settings): the grade of the target (its track's own, else Settings) with the effect of the entry.
VideoLook App::fxpick_look(int fx) const {
    fs::path track;
    if (fxpick_.target == 2) track = current_.path;
    else if (fxpick_.target == 3 && meta_) track = path_from_utf8(meta_->sty_track);
    VideoLook l = video_look_for(track);
    l.fx = video_fx_canon(std::clamp(fx < 0 ? settings_.video_fx : fx, 0, kVideoFxCount - 1));
    if (video_fx_uses_gradient(l.fx)) {
        l.grad = video_fx_gradient(settings_);
        const int own = track.empty() ? -1 : cover_track_video_opts(path_utf8(track)).gsat;
        if (own >= 0) l.grad.sat = own;
    }
    return l;
}

// The preview column: `cols` x `rows` cells, every cell two pixels (an upper half block: the upper pixel is its colour, the lower one the background).
// The picture is the frame of the video that is on screen (cut to the box), or a test picture where the track has no video / is not the playing one.
// The effect works on the pixels as they are shown, so it looks as a small video would. Made at most 25 times a second.
std::vector<std::string> App::fxpick_preview(int fx, int cols, int rows) const {
    const double t = has_track_ ? player_.poll_elapsed() : std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const long tick = static_cast<long>(std::max(0.0, t) * 25.0);
    const std::string key = std::to_string(fx) + "|" + std::to_string(cols) + "x" + std::to_string(rows) + "|" + std::to_string(tick) + "|" + std::to_string(fxpick_.target) + "|" + std::to_string(settings_.video_base) + "|" + std::to_string(settings_.video_contrast) + "," + std::to_string(settings_.video_colors) + "," + std::to_string(settings_.video_brightness);
    if (key == fxpick_prev_key_ && static_cast<int>(fxpick_prev_.size()) == rows) return fxpick_prev_;
    const int pw = cols, ph = rows * 2;
    VideoFrame work;
    // (only the video of the playing track is on hand; another track's video is not decoded for this)
    const bool mine = has_track_ && video_for_track_ == path_utf8(current_.path) && video_.has_video() && (fxpick_.target != 3 || (meta_ && meta_->sty_track == path_utf8(current_.path)));
    const VideoFrame* cf = mine ? video_.current_frame() : nullptr;
    if (cf && cf->w >= 4 && cf->h >= 4 && cf->rgb.size() >= static_cast<size_t>(cf->w) * cf->h * 3) {
        const double ta = static_cast<double>(pw) / ph, sa = static_cast<double>(cf->w) / cf->h;
        int cw = cf->w, ch = cf->h;
        if (sa > ta) cw = std::max(2, static_cast<int>(cf->h * ta)); else ch = std::max(2, static_cast<int>(cf->w / ta));
        work = VideoPlayer::downscale(VideoPlayer::crop(*cf, (cf->w - cw) / 2, (cf->h - ch) / 2, cw, ch), pw, ph);
    } else work = VideoPlayer::test_card(pw, ph);
    const VideoLook look = fxpick_look(fx);
    if (look.active() && work.w == pw && work.h == ph) VideoPlayer::apply_look(work, look, t, false, nullptr);
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(rows));
    auto px = [&](int x, int y, int c) { return static_cast<int>(work.rgb[(static_cast<size_t>(std::clamp(y, 0, work.h - 1)) * work.w + static_cast<size_t>(std::clamp(x, 0, work.w - 1))) * 3 + static_cast<size_t>(c)]); };
    for (int r = 0; r < rows; ++r) {
        std::string line, last;
        for (int x = 0; x < pw; ++x) {
            char col[64];
            std::snprintf(col, sizeof col, "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm", px(x, 2 * r, 0), px(x, 2 * r, 1), px(x, 2 * r, 2), px(x, 2 * r + 1, 0), px(x, 2 * r + 1, 1), px(x, 2 * r + 1, 2));
            if (last != col) { line += col; last = col; }
            line += "\u2580";
        }
        out.push_back(line + "\x1b[0m");
    }
    fxpick_prev_ = out;
    fxpick_prev_key_ = key;
    return out;
}

namespace {
constexpr int kFxPrevMin = 18, kFxPrevMax = 36;   // the preview column: at least / at most this many columns
}

int App::fxpick_names_w() const {
    const std::vector<int> e = fxpick_entries();
    const int n = static_cast<int>(e.size());
    const int rows = (n + kFxPickCols - 1) / kFxPickCols;
    int cw[kFxPickCols] = {0, 0};
    for (int i = 0; i < n; ++i) { const int fx = e[static_cast<size_t>(i)]; cw[i / rows] = std::max(cw[i / rows], display_width(fx < 0 ? std::string("as in Settings") : std::string(video_fx_name(fx))) + 3); }
    return cw[0] + cw[1];
}

// Where the menu is drawn so that the picker has room beside it: the two stand side by side, together in the middle of the window. 0 = not needed
// (no picker) or no room even for the smallest picker: the menu stays where it is, and the picker overlaps it.
int App::fxpick_menu_col(int menu_w, int W) const {
    if (!fxpick_.open || menu_w <= 0) return 0;
    const int need = fxpick_names_w() + 5 + kFxPrevMin;
    if (W - menu_w < need) return 0;
    const int pw = std::min(W - menu_w, fxpick_names_w() + 5 + kFxPrevMax);
    return 1 + (W - (menu_w + pw)) / 2;
}

std::vector<std::string> App::build_fxpick_panel(int W, int& panel_w, int menu_w) const {
    const std::vector<int> e = fxpick_entries();
    const int n = static_cast<int>(e.size());
    const int rows = (n + kFxPickCols - 1) / kFxPickCols;
    auto name_of = [&](int fx) { return fx < 0 ? std::string("as in Settings") : std::string(video_fx_name(fx)); };
    // every column is as wide as its longest name needs ("\u2022 " + the name + a space)
    int cw[kFxPickCols] = {0, 0};
    for (int i = 0; i < n; ++i) cw[i / rows] = std::max(cw[i / rows], display_width(name_of(e[static_cast<size_t>(i)])) + 3);
    int names_w = cw[0] + cw[1];
    // the preview takes what the window leaves (beside the menu: what the menu leaves; at most kFxPrevMax); with less than kFxPrevMin it is left out
    // and the names are squeezed to fit (where the picker has to overlap its menu the preview is the smallest one, so that most of the menu stays visible)
    const int room = (menu_w > 0 && W - menu_w >= names_w + 5 + kFxPrevMin ? W - menu_w : W) - names_w - 5;
    const int prev_w = room >= kFxPrevMin ? std::min(room, menu_w > 0 && W - menu_w < names_w + 5 + kFxPrevMin ? kFxPrevMin : kFxPrevMax) : 0;
    if (!prev_w && names_w + 3 > W) { const int each = std::max(8, (W - 3) / kFxPickCols); for (int k = 0; k < kFxPickCols; ++k) cw[k] = std::min(cw[k], each); names_w = cw[0] + cw[1]; }
    panel_w = std::min(W, 2 + 1 + names_w + (prev_w ? 1 + prev_w + 1 : 1));
    const int inner = panel_w - 2;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string bar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    const int cur_fx = e[static_cast<size_t>(std::clamp(fxpick_.cursor, 0, n - 1))];
    // the preview column: the picture, then the name of the highlighted entry and whether it is the one in use
    std::vector<std::string> prev;
    if (prev_w) {
        const int prow = std::max(2, rows - 2);
        prev = fxpick_preview(cur_fx, prev_w, prow);
        const std::string hc = ansi_for(settings_.header_color, false);
        auto centred = [&](const std::string& t0, const std::string& sgr) {
            const std::string t = truncate_str(t0, prev_w);
            const int l = std::max(0, (prev_w - display_width(t)) / 2);
            return std::string(static_cast<size_t>(l), ' ') + sgr + t + (sgr.empty() ? "" : "\x1b[0m") + std::string(static_cast<size_t>(std::max(0, prev_w - l - display_width(t))), ' ');
        };
        std::string nm = name_of(cur_fx);
        if (cur_fx < 0) nm += " (" + std::string(video_fx_name(settings_.video_fx)) + ")";
        prev.push_back(centred(nm, hc));
        prev.push_back(centred(cur_fx == fxpick_.current ? "(in use)" : "", ""));
        while (static_cast<int>(prev.size()) < rows) prev.push_back(std::string(static_cast<size_t>(prev_w), ' '));
    }
    std::vector<std::string> lines;
    lines.push_back(box_top("VIDEO EFFECT", panel_w, border));
    for (int r = 0; r < rows; ++r) {
        std::string row = " ";
        int used = 1;
        for (int k = 0; k < kFxPickCols; ++k) {
            const int i = k * rows + r;
            std::string cell;
            if (i < n) {
                const int fx = e[static_cast<size_t>(i)];
                const std::string name = truncate_str(name_of(fx), cw[k] - 3);
                cell = (fx == fxpick_.current ? "\u2022 " : "  ") + pad_right(name, cw[k] - 2);
                if (i == fxpick_.cursor) cell = "\x1b[7m" + cell + "\x1b[0m";
            } else cell = std::string(static_cast<size_t>(cw[k]), ' ');
            row += cell;
            used += cw[k];
        }
        if (prev_w) { row += " " + prev[static_cast<size_t>(r)] + " "; used += 1 + prev_w + 1; }
        row += std::string(static_cast<size_t>(std::max(0, inner - used)), ' ');
        lines.push_back(bar + row + bar);
    }
    lines.push_back(box_bottom(panel_w, "[\u2191\u2193\u2190\u2192] move  [ENTER] choose  [ESC] back", settings_.border_color_bottom.empty() ? border : ansi_for(settings_.border_color_bottom, false)));
    return lines;
}

// Drawn after the menu it was opened from, which stays on screen: where the window has room the menu was moved aside (fxpick_menu_col) and the picker
// stands right of it; where it has not, the picker overlaps the menu on the side that costs the least of it. With no menu under it (the Settings
// screen) it is centred.
void App::draw_fxpick(std::ostringstream& frame, int W) const {
    int pw = 0;
    const int ux = float_rect_[0], uy = float_rect_[1], uw = float_rect_[2], uh = float_rect_[3];   // (the menu that is drawn: 0-based; none = 0)
    const std::vector<std::string> lines = build_fxpick_panel(W, pw, uw > 0 ? uw : 0);
    const int ph = static_cast<int>(lines.size());
    int fixed_col = 0, fixed_row = 0;
    if (uw > 0 && uh > 0) {
        const int right = ux + uw, left = ux - pw;
        int col;
        if (right + pw <= W) col = right;
        else if (left >= 0) col = left;
        else col = (right + pw - W) <= -left ? std::max(0, W - pw) : 0;   // (the side where less of the menu is covered)
        fixed_col = col + 1;
        fixed_row = std::clamp(uy, 0, std::max(0, term_rows_ - ph)) + 1;   // (top to top with the menu)
    }
    draw_floating_panel(frame, lines, pw, W, fixed_col, true, fixed_row);
    if (uw > 0 && uh > 0) {   // the picture holes (image style) are cut for both: the rectangle that holds the menu and the picker
        const int x0 = std::min(ux, float_rect_[0]), y0 = std::min(uy, float_rect_[1]);
        const int x1 = std::max(ux + uw, float_rect_[0] + float_rect_[2]), y1 = std::max(uy + uh, float_rect_[1] + float_rect_[3]);
        float_rect_[0] = x0; float_rect_[1] = y0; float_rect_[2] = x1 - x0; float_rect_[3] = y1 - y0;
    }
}

}  // namespace muisc
