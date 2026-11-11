// app_fxpick.cpp
// Part of class App (src/app.h): the VIDEO EFFECT picker. ENTER on the Video Effect row of any menu (Settings, the VIDEO overlay, the VIDEO
// OPTIONS panel of the big video, the Style Option overlay) opens a small panel on top of it that lists every effect in three columns:
// the arrow keys go through the list, ENTER takes the effect, ESC leaves it as it was. In the two per-track menus the first entry is "as in
// Settings" (-1), as it is the first stop of the left / right keys there.
#include "app.h"
#include "meta_state.h"
#include "overlay_rows.h"
#include "path_utf8.h"
#include "terminal_ui.h"
#include "video_fx.h"

#include <algorithm>
#include <sstream>

namespace muisc {

namespace {
constexpr int kFxPickCols = 3;
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
        else if (key == 'C') { const int t = c + rows; c = t < n ? t : (c % rows < rows ? c % rows : 0); }   // right: the next column, round to the first
        else if (key == 'D') { const int t = c - rows; if (t >= 0) c = t; else { int last = c % rows + rows * (kFxPickCols - 1); while (last >= n) last -= rows; c = std::max(0, last); } }
        else if (key == kKeyHome) c = 0;
        else if (key == kKeyEnd) c = n - 1;
    } else if (key == kKeyHome) c = 0;
    else if (key == kKeyEnd) c = n - 1;
    else if (key == '\r' || key == '\n') {
        const int fx = e[static_cast<size_t>(c)];
        fxpick_.open = false;
        fxpick_apply(fx);
    } else if (key == 27 || key == 'q' || key == 'Q') {
        fxpick_.open = false;
        video_pic_key_.clear();   // (a picture that was cut for the panel is sent whole again)
    }
    force_redraw_ = true;
    return true;
}

std::vector<std::string> App::build_fxpick_panel(int W, int& panel_w) const {
    const std::vector<int> e = fxpick_entries();
    const int n = static_cast<int>(e.size());
    const int rows = (n + kFxPickCols - 1) / kFxPickCols;
    // the longest name decides the column width, but three columns must fit in the window
    int name_w = 0;
    for (int fx : e) name_w = std::max(name_w, static_cast<int>((fx < 0 ? std::string("as in Settings") : std::string(video_fx_name(fx))).size()));
    const int fit = std::max(10, (W - 4 - 2) / kFxPickCols - 2);
    const int cw = std::min(name_w, fit) + 2;   // "› " + the name
    panel_w = std::min(W, cw * kFxPickCols + 2 + 2);
    const int inner = panel_w - 2;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string bar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
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
                std::string name = fx < 0 ? std::string("as in Settings") : std::string(video_fx_name(fx));
                name = truncate_str(name, cw - 2);
                cell = (fx == fxpick_.current ? "• " : "  ") + pad_right(name, cw - 2);
                if (i == fxpick_.cursor) cell = "\x1b[7m" + cell + "\x1b[0m";
            } else cell = std::string(static_cast<size_t>(cw), ' ');
            row += cell;
            used += cw;
        }
        row += std::string(static_cast<size_t>(std::max(0, inner - used)), ' ');
        lines.push_back(bar + row + bar);
    }
    lines.push_back(box_bottom(panel_w, "[↑↓←→] move  [ENTER] choose  [ESC] back", settings_.border_color_bottom.empty() ? border : ansi_for(settings_.border_color_bottom, false)));
    return lines;
}

void App::draw_fxpick(std::ostringstream& frame, int W) const {
    int pw = 0;
    const std::vector<std::string> lines = build_fxpick_panel(W, pw);
    draw_floating_panel(frame, lines, pw, W, 0, true);
}

}  // namespace muisc
