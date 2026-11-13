// app_fxpick.cpp
// Part of class App (src/app.h): the VIDEO EFFECT picker. ENTER on the Video Effect row of any menu (Settings, the VIDEO overlay, the VIDEO
// OPTIONS panel of the big video, the Style Option overlay) opens one panel in the middle of the window (the menu it came from is not drawn
// meanwhile) with every effect in two columns and a large preview: the video (a test picture when none plays) with the highlighted effect on it.
// The arrow keys go through the list, ENTER takes the effect, ESC leaves it as it was. In the two per-track menus the first entry is "as in
// Settings" (-1), as it is the first stop of the left / right keys there.
#include "app.h"
#include "app_internal.h"
#include "cover_store.h"
#include "meta_state.h"
#include "overlay_rows.h"
#include "path_utf8.h"
#include "terminal_gfx.h"
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
    fxpick_pic_ = 0;
    fxpick_pic_key_.clear();
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
        fxpick_pic_ = 0; fxpick_pic_key_.clear();
        fxpick_apply(fx);
    } else if (key == 27 || key == 'q' || key == 'Q') {
        fxpick_.open = false;
        fxpick_prev_key_.clear();
        fxpick_pic_ = 0; fxpick_pic_key_.clear();
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
    const std::string key = std::to_string(fx) + "|" + std::to_string(cols) + "x" + std::to_string(rows) + "|" + std::to_string(tick) + "|" + std::to_string(fxpick_.target) + "|" + std::to_string(settings_.video_base) + "," + std::to_string(settings_.video_fxc) + "|" + std::to_string(settings_.video_contrast) + "," + std::to_string(settings_.video_colors) + "," + std::to_string(settings_.video_brightness);
    if (key == fxpick_prev_key_ && static_cast<int>(fxpick_prev_.size()) == rows) return fxpick_prev_;
    const int pw = cols, ph = rows * 2;
    // The look is made at a size where it has its structure (cells, dots, characters ...): about 300 pixels across, then shrunk to the preview's pixels.
    const int K = std::clamp(300 / std::max(1, pw), 1, 12);
    const int vw = pw * K, vh = ph * K;
    VideoFrame work;
    // (only the video of the playing track is on hand; another track's video is not decoded for this)
    const bool mine = has_track_ && video_for_track_ == path_utf8(current_.path) && video_.has_video() && (fxpick_.target != 3 || (meta_ && meta_->sty_track == path_utf8(current_.path)));
    const VideoFrame* cf = mine ? video_.current_frame() : nullptr;
    if (cf && cf->w >= 4 && cf->h >= 4 && cf->rgb.size() >= static_cast<size_t>(cf->w) * cf->h * 3) {
        const double ta = static_cast<double>(pw) / ph, sa = static_cast<double>(cf->w) / cf->h;
        int cw = cf->w, ch = cf->h;
        if (sa > ta) cw = std::max(2, static_cast<int>(cf->h * ta)); else ch = std::max(2, static_cast<int>(cf->w / ta));
        work = VideoPlayer::downscale(VideoPlayer::crop(*cf, (cf->w - cw) / 2, (cf->h - ch) / 2, cw, ch), vw, vh);
    } else work = VideoPlayer::test_card(vw, vh);
    const VideoLook look = fxpick_look(fx);
    if (look.active() && work.w == vw && work.h == vh) VideoPlayer::apply_look(work, look, t, false, nullptr);
    if (work.w == vw && work.h == vh && K > 1) work = VideoPlayer::downscale(work, pw, ph);
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
constexpr int kFxPrevMin = 18;                            // the preview column: at least this many columns (below that it is left out) ...
constexpr int kFxPrevMaxText = 72, kFxPrevMaxPic = 100;   // ... and at most this many where the window has the room: three times the area it had (36 columns of half blocks, 56 columns of picture)
constexpr int kFxPrevPad = 2;                             // the room around the preview: two columns between the names and it, two before the border
constexpr int kFxFrame = 3 + 2 * kFxPrevPad;              // everything but the names and the preview itself: the borders (2), a space in front of the names, the two paddings
constexpr int kFxMargin = 1;                              // columns / rows of the window left free around the panel
}

// A real picture for the preview needs a picture terminal and a menu that has a picture behind it, or room for one: the VIDEO OPTIONS panel of the big
// video (the video is the first picture, the preview takes the second slot), the VIDEO SETTINGS overlay and the Style Option overlay over the main screen
// (the screen's picture, if it has one, keeps the first slot). Settings has no picture behind it and shows the preview as text.
bool App::fxpick_picture_possible() const {
    if (!fxpick_.open || gfxs_.proto == GfxProto::None) return false;
    if (fxpick_.target == 1) return mode_ == Mode::VideoMenu && !gfx2_.active;
    if (fxpick_.target == 2) return video_opts_open_ && gfx_.active && !gfx2_.active;   // (a layout with the oscilloscope / spectrogram beside the video has the second slot: then the preview stays text)
    if (fxpick_.target == 3) return meta_ && meta_->sty_from_main && mode_ == Mode::MetaEdit && !gfx2_.active;
    return false;
}

// The preview as a picture: the video of the track (the whole frame, in its own proportions; decoded for the preview, see below), or a test picture,
// with the effect worked on the pixels it is shown in -- as many as the video has everywhere else. Made again 25 times a second at most.
bool App::fxpick_picture(int fx, int cols, int rows, int abs_col, int abs_row, std::vector<std::string>& out) const {
    out.assign(static_cast<size_t>(std::max(0, rows)), std::string(static_cast<size_t>(std::max(0, cols)), ' '));
    if (!fxpick_picture_possible() || cols < 6 || rows < 3 || abs_col < 0 || abs_row < 0) return false;
    const bool second = gfx_.active;   // (the screen's own picture keeps the first slot)
    GfxFrame& g = second ? gfx2_ : gfx_;
    int cw0 = 10, ch0 = 20;
    gfx_cell_pixels(settings_.cell_pixels, cw0, ch0);
    cw0 = std::max(4, cw0); ch0 = std::max(8, ch0);
    // The video: decoded for this preview by a decoder of its own (fxpick_video_), at the resolution the video has everywhere else (Settings -> Video
    // Resolution, or the track's own: 640 / 1280 / 1920 pixels across on Kitty, 480 / 720 / 960 on Sixel) and the size the preview is shown in. (It
    // used the frame of the video behind the list, which is decoded for another size -- the small field of the main screen -- and was then cut to 480
    // pixels: a blurred picture.) The playing track's video runs with the sound; another track's (Style Option) from the moment the list opened.
    const fs::path trk = fxpick_.target == 3 && meta_ ? path_from_utf8(meta_->sty_track) : current_.path;
    const std::string vsrc = trk.empty() ? std::string() : video_shown_for(trk);
    const bool playing_trk = has_track_ && !trk.empty() && path_utf8(trk) == path_utf8(current_.path);
    const auto now = std::chrono::steady_clock::now();
    if (vsrc != fxpick_video_src_) { fxpick_video_.set_source(vsrc); fxpick_video_src_ = vsrc; fxpick_video_t0_ = now; fxpick_pic_key_.clear(); }
    fxpick_video_seen_ = now;
    const bool vplay = playing_trk ? !player_.is_paused() : true;
    const double t = playing_trk ? player_.poll_elapsed() : !vsrc.empty() ? std::chrono::duration<double>(now - fxpick_video_t0_).count()
                                                                          : std::chrono::duration<double>(now.time_since_epoch()).count();
    const long tick = static_cast<long>(std::max(0.0, t) * 25.0);
    const bool have_video = !vsrc.empty() && fxpick_video_.has_video();
    const double sa = have_video && fxpick_video_.aspect() > 0.1 ? fxpick_video_.aspect() : 16.0 / 9.0;
    // the whole frame, fitted into the box of cols x rows cells
    const double bw = static_cast<double>(cols) * cw0, bh = static_cast<double>(rows) * ch0;
    double fw = bw, fh = bw / sa;
    if (fh > bh) { fh = bh; fw = bh * sa; }
    const int vcols = std::clamp(static_cast<int>((fw + cw0 / 2.0) / cw0), 1, cols), vrows = std::clamp(static_cast<int>((fh + ch0 / 2.0) / ch0), 1, rows);
    const int maxw = std::max(64, video_pic_width());
    const double f = std::min(1.0, static_cast<double>(maxw) / (vcols * cw0));
    const int dw = std::max(2, static_cast<int>(vcols * cw0 * f) & ~1), dh = std::max(2, static_cast<int>(vrows * ch0 * f) & ~1);
    const int lead = (cols - vcols) / 2, top = (rows - vrows) / 2;
    const VideoFrame* cf = nullptr;
    if (have_video) {
        fxpick_video_.set_buffer_mb(settings_.video_buffer);
        fxpick_video_.set_sharp(video_res_for() > 0);
        fxpick_video_.set_rate_mode(settings_.video_fps);
        cf = fxpick_video_.frame_for(t, dw, dh, vplay);
        if (!cf) cf = fxpick_video_.current_frame();   // (the decoder is starting: the last frame it had, else the test picture)
    }
    if (cf && !(cf->w >= 4 && cf->h >= 4 && cf->rgb.size() >= static_cast<size_t>(cf->w) * cf->h * 3)) cf = nullptr;
    const bool tc = gfxs_.proto == GfxProto::Kitty && video_res_for() > 0;   // true colour (high / full resolution on Kitty)
    const VideoLook look = fxpick_look(fx);
    const std::string key = std::to_string(dw) + "x" + std::to_string(dh) + "|" + std::to_string(vcols) + "x" + std::to_string(vrows) + "|" + std::to_string(tick) + "|" + look.sig() + "|" +
                            std::to_string(tc ? 1 : 0) + "|" + std::to_string(second ? 2 : 1) + "|" + (cf ? "v" : "c");
    const bool remake = key != fxpick_pic_key_ || g.w != dw || g.h != dh || g.level.size() != static_cast<size_t>(dw) * dh;
    if (remake) {
        VideoFrame work = cf ? VideoPlayer::resample(*cf, dw, dh) : VideoPlayer::test_card(dw, dh);
        VideoFrame prev_shown;
        const VideoFrame* prevp = nullptr;
        if (look.fx == 17 && cf) {   // (the interlace look combs against the frame before)
            if (const VideoFrame* pf = fxpick_video_.previous_frame()) { prev_shown = VideoPlayer::resample(*pf, dw, dh); prevp = &prev_shown; }
        }
        if (look.active() && work.w == dw && work.h == dh) VideoPlayer::apply_look(work, look, t, false, prevp);
        if (tc) { g.rgb = work.rgb; g.level.assign(static_cast<size_t>(dw) * dh, 255); g.hue.clear(); }
        else {
            VideoPlayer::quantize(work, g.hue, g.level, look.dither);
            const auto& pal = VideoPlayer::palette();
            for (size_t i = 0; i < pal.size() && i < g.pal.size(); ++i) g.pal[i] = pal[i];
        }
        fxpick_pic_key_ = key;
    }
    g.plain = true; g.direct = true; g.scale = 1; g.truecolor = tc;
    g.out_cw = cw0; g.out_ch = ch0;
    g.cols = vcols; g.rows = vrows; g.w = dw; g.h = dh;
    g.col = abs_col + lead; g.row = abs_row + top;
    g.id = second ? 1 : 0; g.grid.clear(); g.crop = 0;
    g.cut_c = g.cut_r = g.cut_w = g.cut_h = 0; g.cut2_w = 0;
    g.opaque = settings_.black_background;
    g.erase = !settings_.black_background;
    g.active = true;
    g.fresh = remake;
    fxpick_pic_ = second ? 2 : 1;
    for (int r = top; r < top + vrows; ++r)
        out[static_cast<size_t>(r)] = std::string(static_cast<size_t>(lead), ' ') + gfx_hole(vcols) + std::string(static_cast<size_t>(cols - lead - vcols), ' ');
    return true;
}

// The list is one panel, exactly centred in the window (rows and columns), over everything else: the menu it came from is not drawn while it is open, and the
// pictures behind get a hole of the panel's size and nothing more. Two columns of names on the left; the preview takes what the window leaves, in the
// shape of a 16:9 picture, as large as kFxPrevMax* and the window allow (the preview was a narrow strip beside the menu before).
std::vector<std::string> App::build_fxpick_panel(int W, int& panel_w, int abs_col, int abs_row, bool layout_only) const {
    const std::vector<int> e = fxpick_entries();
    const int n = static_cast<int>(e.size());
    const int rows = (n + kFxPickCols - 1) / kFxPickCols;
    auto name_of = [&](int fx) { return fx < 0 ? std::string("as in Settings") : std::string(video_fx_name(fx)); };
    // every column is as wide as its longest name needs ("\u2022 " + the name + a space)
    int cw[kFxPickCols] = {0, 0};
    for (int i = 0; i < n; ++i) cw[i / rows] = std::max(cw[i / rows], display_width(name_of(e[static_cast<size_t>(i)])) + 3);
    int names_w = cw[0] + cw[1];
    // the preview: as wide as the window allows (at most kFxPrevMax*), its height from the 16:9 shape of the picture (a cell of the picture terminal in its real
    // pixels; half blocks are one pixel wide and two high), and no higher than the window leaves; with less than kFxPrevMin columns it is left out and the
    // names are squeezed to fit
    const bool pic = fxpick_picture_possible();
    double cell_ratio = 0.5;   // width / height of a cell
    if (pic) { int cw0 = 10, ch0 = 20; gfx_cell_pixels(settings_.cell_pixels, cw0, ch0); cell_ratio = static_cast<double>(std::max(4, cw0)) / std::max(8, ch0); }
    const double rows_per_col = cell_ratio * 9.0 / 16.0;
    const int vis = std::min(rows, std::max(5, term_rows_ - 4));   // (a short window shows only the rows of names that fit)
    const int room_h = std::max(2, term_rows_ - 2 * kFxMargin - 4);   // the picture's rows: the window less its margins, the two borders and the two caption lines
    int prev_w = std::min(W - 2 * kFxMargin - names_w - kFxFrame, pic ? kFxPrevMaxPic : kFxPrevMaxText);
    int prow = 0;
    if (prev_w >= kFxPrevMin) {
        prow = std::max(2, static_cast<int>(std::lround(prev_w * rows_per_col)));
        if (prow > room_h) { prow = room_h; prev_w = std::min(prev_w, std::max(1, static_cast<int>(std::lround(prow / rows_per_col)))); }
        if (prev_w < kFxPrevMin) { prev_w = 0; prow = 0; }
    } else prev_w = 0;
    if (!prev_w && names_w + 3 > W) { const int each = std::max(8, (W - 3) / kFxPickCols); for (int k = 0; k < kFxPickCols; ++k) cw[k] = std::min(cw[k], each); names_w = cw[0] + cw[1]; }
    panel_w = std::min(W, 2 + 1 + names_w + (prev_w ? kFxPrevPad + prev_w + kFxPrevPad : 1));
    const int inner = panel_w - 2;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string bar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    const int cur_fx = e[static_cast<size_t>(std::clamp(fxpick_.cursor, 0, n - 1))];
    const int top = rows > vis ? std::clamp(std::clamp(fxpick_.cursor, 0, n - 1) % rows - vis / 2, 0, rows - vis) : 0;
    // the body: the names and the preview column (the picture, then the name of the highlighted entry and whether it is the one in use), the shorter of the
    // two centred against the other
    const int block = prev_w ? prow + 2 : 0;
    const int body = std::max(vis, block);
    const int noff = (body - vis) / 2, poff = (body - block) / 2;
    std::vector<std::string> prev;
    if (prev_w) {
        if (layout_only) prev.assign(static_cast<size_t>(prow), std::string(static_cast<size_t>(prev_w), ' '));
        else if (!(abs_col >= 0 && abs_row >= 0 && fxpick_picture(cur_fx, prev_w, prow, abs_col + 1 + 1 + names_w + kFxPrevPad, abs_row + 1 + poff, prev)))
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
    }
    std::vector<std::string> lines;
    lines.push_back(box_top("VIDEO EFFECT", panel_w, border));
    for (int vr = 0; vr < body; ++vr) {
        const int r = top + (vr - noff);
        const bool names_row = vr >= noff && vr < noff + vis;
        std::string row = " ";
        int used = 1;
        for (int k = 0; k < kFxPickCols; ++k) {
            const int i = k * rows + r;
            std::string cell;
            if (names_row && i < n) {
                const int fx = e[static_cast<size_t>(i)];
                const std::string name = truncate_str(name_of(fx), cw[k] - 3);
                cell = (fx == fxpick_.current ? "\u2022 " : "  ") + pad_right(name, cw[k] - 2);
                if (i == fxpick_.cursor) cell = "\x1b[7m" + cell + "\x1b[0m";
            } else cell = std::string(static_cast<size_t>(cw[k]), ' ');
            row += cell;
            used += cw[k];
        }
        if (prev_w) {
            const int pr = vr - poff;
            const std::string pl = pr >= 0 && pr < block ? prev[static_cast<size_t>(pr)] : std::string(static_cast<size_t>(prev_w), ' ');
            row += std::string(static_cast<size_t>(kFxPrevPad), ' ') + pl + std::string(static_cast<size_t>(kFxPrevPad), ' ');
            used += kFxPrevPad + prev_w + kFxPrevPad;
        }
        row += std::string(static_cast<size_t>(std::max(0, inner - used)), ' ');
        lines.push_back(bar + row + bar);
    }
    lines.push_back(box_bottom(panel_w, std::string(rows > vis ? "[\u2191\u2193\u2190\u2192] move (more rows: scroll)  [ENTER] [ESC]" : "[\u2191\u2193\u2190\u2192] move  [ENTER] choose  [ESC] back"), settings_.border_color_bottom.empty() ? border : ansi_for(settings_.border_color_bottom, false)));
    return lines;
}

// Drawn after the menu it was opened from (which the callers leave out while the list is open): exactly in the middle of the window, rows and columns, the
// same rule as draw_floating_panel(exact_center). The panel's rectangle is the only floating rectangle of the frame, so the pictures behind get a hole of
// exactly its size.
void App::draw_fxpick(std::ostringstream& frame, int W) const {
    int pw = 0;
    fxpick_pic_ = 0;   // (set again below where the preview is a picture)
    const int ph = static_cast<int>(build_fxpick_panel(W, pw, -1, -1, true).size());
    const int col0 = std::max(0, (W - pw) / 2);   // 0-based, as draw_floating_panel puts it
    const int row1 = std::clamp(1 + std::max(0, (term_rows_ - ph) / 2), 1, std::max(1, term_rows_ - ph));
    const std::vector<std::string> lines = build_fxpick_panel(W, pw, col0, row1 - 1, false);
    draw_floating_panel(frame, lines, pw, W, 0, true);
}

}  // namespace muisc
