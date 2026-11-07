// The cover in the meta data editor: the picture under the fields and the "Cover Options" overlay (choose a picture for the track,
// "no cover", back to automatic, delete a fetched / folder picture, search pictures by file name). Member functions of App.
#include "app.h"
#include "utf8_util.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "path_utf8.h"
#include "picture_formats.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <system_error>
#include "app_internal.h"

namespace muisc {

namespace {


bool is_picture_name(const fs::path& p) {
    const std::string e = ascii_lower_str(path_utf8(p.extension()));
    for (const char* x : kCoverPictureExtensions) if (e == x) return true;
    return false;
}

std::string choice_of(const CoverCandidate& c) { return c.kind == CoverCandidate::Embedded ? std::string(kCoverEmbedded) : path_utf8(c.file); }

}  // namespace

// The cover the track has now, as braille text (the same cover the player would show, choices included).
std::vector<std::string> App::meta_cover_pane(int inner, int rows) const {
    const std::string path = meta_hovering_path();
    if (path.empty() || rows < 2) return std::vector<std::string>(static_cast<size_t>(std::max(rows, 0)), std::string(static_cast<size_t>(inner), ' '));
    CoverRequest rq;
    rq.path = path;
    rq.title = meta_hover_values_[static_cast<size_t>(MetaField::Title)];   // (like the disk: the tag title, else the file name)
    if (rq.title.empty() || rq.title == "-") rq.title = path_utf8(path_from_utf8(path).stem());
    rq.album = meta_hover_values_[static_cast<size_t>(MetaField::Album)];
    if (rq.album == "-") rq.album.clear();
    rq.search = settings_.cover_search;
    rq.store_rev = cover_store_revision();
    // what the disk shows of this track (Cover Options / Style Option / VIDEO SETTINGS): the cover, a frame of its video, or the plain disk
    const int own = cover_track_video_mode(path);
    const bool has_video = !video_source_for(path_from_utf8(path)).empty();
    const int vm = own == 1 ? 0 : own == 2 ? 1 : own == 3 ? 2 : settings_.video_mode ? settings_.video_cover : 0;   // 0 cover, 1 thumbnail, 2 plain disk
    const bool plain_disk = own == 3 || (own == 0 && vm == 2 && has_video);
    const bool overlay_up0 = meta_choice_open_ || meta_cov_open_ || meta_sty_open_ || meta_save_.active;
    if (plain_disk) {
        const auto lines = cover_preview_lines(std::string(), false, std::min(inner, rows * 2), rows, "plain disk (no cover)");
        std::vector<std::string> out;
        const int cols = std::min(inner, rows * 2), lpad = std::max(0, (inner - cols) / 2);
        for (int r = 0; r < rows; ++r) {
            const std::string row = r < static_cast<int>(lines.size()) ? lines[static_cast<size_t>(r)] : std::string(static_cast<size_t>(cols), ' ');
            out.push_back(std::string(static_cast<size_t>(lpad), ' ') + row + "\x1b[0m" + std::string(static_cast<size_t>(std::max(0, inner - lpad - cols)), ' '));
        }
        return out;
    }
    if ((own == 2 || (own == 0 && vm == 1)) && has_video) {   // thumbnail: a frame of the video is the current picture
        CoverRequest tq = rq;
        tq.snapshot_video = video_source_for(path_from_utf8(path));
        if (const CoverImage* img = track_cover(tq)) {
            if (cf_full_res_ && gfx_proto_ != GfxProto::None && !overlay_up0 &&
                cover_full_picture_img(img, inner, rows, meta_pane_col_ + 2, 4 + kMetaFieldCount + 3)) {
                std::vector<std::string> pic;
                for (int r = 0; r < rows; ++r) pic.push_back(gfx_hole(inner));
                return pic;
            }
            const int cols = std::min(inner, rows * 2), lpad = std::max(0, (inner - cols) / 2);
            const auto lines = album_cover_lines_for(*img, 0, settings_.cover_contrast, settings_.cover_brightness, settings_.cover_colors, 0.0, cols, rows, settings_.cover_chroma);
            std::vector<std::string> out;
            for (int r = 0; r < rows; ++r) {
                const std::string row = r < static_cast<int>(lines.size()) ? lines[static_cast<size_t>(r)] : std::string(static_cast<size_t>(cols), ' ');
                out.push_back(std::string(static_cast<size_t>(lpad), ' ') + row + "\x1b[0m" + std::string(static_cast<size_t>(std::max(0, inner - lpad - cols)), ' '));
            }
            return out;
        }
    }
    const CoverSource src = resolve_cover_source(rq);
    const bool overlay_up = meta_choice_open_ || meta_cov_open_ || meta_sty_open_ || meta_save_.active;   // (a picture would be drawn over a floating panel)
    if (cf_full_res_ && gfx_proto_ != GfxProto::None && !overlay_up && !src.none && !src.file.empty()) {
        if (cover_full_picture(src.file, src.embedded, inner, rows, meta_pane_col_ + 2, 4 + kMetaFieldCount + 3)) {
            std::vector<std::string> pic;
            for (int r = 0; r < rows; ++r) pic.push_back(gfx_hole(inner));
            return pic;
        }
    }
    const int cols = std::min(inner, rows * 2);
    const auto lines = cover_preview_lines(src.file, src.embedded, cols, rows, (src.none || src.file.empty()) ? std::string("no cover") : std::string());
    std::vector<std::string> out;
    const int lpad = std::max(0, (inner - cols) / 2);
    for (int r = 0; r < rows; ++r) {
        const std::string row = r < static_cast<int>(lines.size()) ? lines[static_cast<size_t>(r)] : std::string(static_cast<size_t>(cols), ' ');
        out.push_back(std::string(static_cast<size_t>(lpad), ' ') + row + "\x1b[0m" + std::string(static_cast<size_t>(std::max(0, inner - lpad - cols)), ' '));
    }
    return out;
}

void App::meta_toggle_cover_res() {
    if (gfx_proto_ == GfxProto::None) { meta_status_ = "this terminal shows no pictures (no Kitty / Sixel graphics): the cover stays ASCII"; return; }
    cf_full_res_ = !cf_full_res_;
    cf_pic_key_.clear();
    meta_status_ = cf_full_res_ ? "cover: full resolution" : "cover: ASCII";
}

void App::meta_cov_open() {
    const std::string path = meta_hovering_path();
    if (path.empty()) { meta_status_ = "no title selected"; return; }
    meta_cov_track_ = path;
    meta_cov_open_ = true;
    meta_cov_search_ = false;
    meta_cov_ask_delete_ = false;
    meta_cov_query_.clear();
    meta_cov_msg_.clear();
    meta_cov_sel_ = 0;
    text_disengage();
    meta_cov_rebuild();
    // start on the picture the track uses now
    const std::string cur = cover_track_choice(path);
    for (size_t i = 0; i < meta_cov_items_.size(); ++i) {
        const auto& it = meta_cov_items_[i];
        if ((cur.empty() && it.special == 1) || (cur == kCoverNone && it.special == 2) || (it.special == 0 && choice_of(it.c) == cur)) { meta_cov_sel_ = static_cast<int>(i); break; }
    }
}

void App::meta_cov_rebuild() {
    const fs::path track = path_from_utf8(meta_cov_track_);
    const std::string q = ascii_lower_str(meta_cov_query_);
    meta_cov_items_.clear();
    if (q.empty()) {
        for (int m = 10; m <= 13; ++m) meta_cov_items_.push_back({m, {}});   // what the disk shows of a track with a video (a mode of its own)
        meta_cov_items_.push_back({1, {}});
    }
    std::vector<fs::path> seen;
    auto add = [&](const CoverCandidate& c) {
        if (!q.empty() && ascii_lower_str(c.label).find(q) == std::string::npos) return;
        for (const auto& s : seen) if (s == c.file) return;
        seen.push_back(c.file);
        meta_cov_items_.push_back({0, c});
    };
    for (const auto& c : cover_candidates(track, false)) add(c);
    if (q.empty() ? has_embedded_picture(track) : (std::string("inside the file").find(q) != std::string::npos && has_embedded_picture(track))) {
        CoverCandidate e;
        e.kind = CoverCandidate::Embedded;
        e.file = track;
        e.label = "inside the file";
        meta_cov_items_.insert(meta_cov_items_.begin() + (q.empty() ? 5 : 0), MetaCovItem{0, e});
    }
    // the covers of the track's album (its fetched covers in other folders, the pictures chosen for it)
    const std::string album = meta_hover_values_[static_cast<size_t>(MetaField::Album)];
    if (!album.empty()) {
        const AlbumCovers ac = album_covers(track.parent_path(), album, track, false, false);
        for (const auto& f : ac.files) {
            CoverCandidate c;
            c.file = f;
            c.kind = path_utf8(f.parent_path().filename()) == kFetchedCoversDir ? CoverCandidate::Fetched : CoverCandidate::Folder;
            c.label = path_utf8(f.parent_path().filename()) + "/" + path_utf8(f.filename());
            add(c);
        }
    }
    // a search by file name also looks into the folders around the track (the folder above it, two levels down)
    if (!q.empty()) {
        std::error_code ec;
        const fs::path root = track.parent_path().has_parent_path() ? track.parent_path().parent_path() : track.parent_path();
        int count = 0;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && count < 300; it.increment(ec)) {
            if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
            std::error_code e2;
            if (!it->is_regular_file(e2) || !is_picture_name(it->path())) continue;
            const std::string name = path_utf8(it->path().filename());
            if (ascii_lower_str(name).find(q) == std::string::npos) continue;
            CoverCandidate c;
            c.file = it->path();
            c.kind = path_utf8(it->path().parent_path().filename()) == kFetchedCoversDir ? CoverCandidate::Fetched : CoverCandidate::Folder;
            c.label = path_utf8(it->path().parent_path().filename()) + "/" + name;
            bool dup = false;
            for (const auto& s : seen) if (s == c.file) { dup = true; break; }
            if (dup) continue;
            seen.push_back(c.file);
            meta_cov_items_.push_back({0, c});
            ++count;
        }
    }
    const bool shows_none = q.empty() && cover_track_choice(meta_cov_track_) == kCoverNone;
    if (shows_none) meta_cov_items_.insert(meta_cov_items_.begin() + 5, MetaCovItem{2, {}});   // (listed only while it is the choice: [DEL] on Automatic sets it)
    {   // the picture in use goes to the top of the pictures (right under "Automatic"): the way to the options above it is short
        const std::string use = cover_track_choice(meta_cov_track_);
        const size_t top = q.empty() ? (shows_none ? 6 : 5) : 0;
        if (!use.empty()) {
            for (size_t i = top + 1; i < meta_cov_items_.size(); ++i) {
                const MetaCovItem& it = meta_cov_items_[i];
                if (it.special == 0 && choice_of(it.c) == use) { std::rotate(meta_cov_items_.begin() + static_cast<long>(top), meta_cov_items_.begin() + static_cast<long>(i), meta_cov_items_.begin() + static_cast<long>(i) + 1); break; }
            }
        }
    }
    meta_cov_sel_ = std::clamp(meta_cov_sel_, 0, std::max(0, static_cast<int>(meta_cov_items_.size()) - 1));
}

namespace {
// One value of the Style overlay: -1 = as in Settings, else 0..100 (steps of 5) or a look 0..7. dir +1 / -1 steps through the ring
// "as in Settings, 0, 5, ... 100" (looks: "as in Settings, 0 .. 7").
int sty_step(int cur, int dir, int max_value, int step) {
    const int count = max_value / step + 2;   // the values and "as in Settings"
    int idx = cur < 0 ? 0 : cur / step + 1;
    idx = ((idx + dir) % count + count) % count;
    return idx == 0 ? -1 : (idx - 1) * step;
}
}  // namespace

void App::meta_sty_open() {
    const std::string path = meta_hovering_path();
    if (path.empty()) { meta_status_ = "no title selected"; return; }
    meta_sty_track_ = path;
    meta_sty_open_ = true;
    meta_sty_sel_ = 0;
    meta_sty_msg_.clear();
    text_disengage();
}

// The Style Option overlay of the track that plays, straight from the main UI (HKeyStyleOptions): it opens the meta editor on that track
// with the overlay alone on the screen; ESC closes it, [m] goes on to the meta editor on that track.
void App::meta_open_style_playing() {
    if (audiobook_) { status_line_ = "style options: music player only"; return; }
    if (!has_track_ || !current_is_local_ || current_path_.empty()) { status_line_ = "style options: play a track from your library first"; return; }
    meta_open();
    const std::string cur = path_utf8(current_path_);
    bool found = false;
    for (size_t i = 0; i < meta_lib_view_.size(); ++i)
        if (path_utf8(meta_lib_view_[i].path) == cur) { meta_lib_selected_ = static_cast<int>(i); found = true; break; }
    if (!found) { mode_ = Mode::Browse; status_line_ = "style options: the playing track is not in the library"; return; }
    meta_sty_open();
    if (meta_sty_open_) meta_sty_from_main_ = true;
    else mode_ = Mode::Browse;
}

bool App::handle_meta_sty_key(int key) {
    if (!meta_sty_open_) return false;
    const bool arrow = last_key_was_arrow();
    if (key == 27) {
        meta_sty_open_ = false;
        if (meta_sty_from_main_) { meta_sty_from_main_ = false; mode_ = Mode::Browse; }   // opened with the main UI's key: back there
        return true;
    }
    if (meta_sty_from_main_ && !arrow && (key == 'm' || key == 'M')) {   // opened from the main UI: on to the meta data of this very song
        meta_sty_open_ = false;
        meta_sty_from_main_ = false;
        meta_focus_ = 2;   // the fields panel
        meta_field_ = 0;
        return true;
    }
    if (arrow && (key == 'A' || key == 'B')) { { const int n = settings_.video_mode ? 16 : 6; meta_sty_sel_ = (meta_sty_sel_ + (key == 'A' ? n - 1 : 1)) % n; } return true; }
    const std::string album = meta_hover_values_[static_cast<size_t>(MetaField::Album)];
    const std::string folder = path_utf8(path_from_utf8(meta_sty_track_).parent_path());
    CoverStyle st = cover_track_style(meta_sty_track_);
    int dir = 0;
    bool reset = false;
    if (arrow && key == 'C') dir = 1;
    else if (arrow && key == 'D') dir = -1;
    else if (!arrow && (key == 't' || key == 'T')) {   // picture / braille, as [t] in the cover options
        meta_toggle_cover_res();
        meta_sty_msg_ = meta_status_;
        return true;
    }
    else if (!arrow && (key == kKeyDelete || key == 127 || key == 'd' || key == 'r')) reset = true;
    else if (key == '\r' || key == '\n' || key == ' ') dir = 1;
    if (!dir && !reset) return true;
    // 4 Cover Chromatic Aberration; with the Video mode on: 5 Use Cover or Thumbnail, 6 Video, 7 Use Lyrics on Video, 8 Video Size, 9 Video Effect,
    // 10 Video Contrast, 11 Video Colors, 12 Video Chromatic Aberration, 13 Video Resolution, 14 Video Rounded Corners; 15 the album (the last row)
    const int sel = (!settings_.video_mode && meta_sty_sel_ >= 5) ? 15 : meta_sty_sel_;
    switch (sel) {
        case 0: st.look = reset ? -1 : sty_step(st.look, dir, kCoverLookCount - 1, 1); break;
        case 1: st.contrast = reset ? -1 : sty_step(st.contrast, dir, 100, 5); break;
        case 2: st.brightness = reset ? -1 : sty_step(st.brightness, dir, 100, 5); break;
        case 3: st.colors = reset ? -1 : sty_step(st.colors, dir, 100, 5); break;
        case 4: st.chroma = reset ? -1 : sty_step(st.chroma, dir, kChromaLevels - 1, 1); break;
        case 5: {   // Use Cover or Thumbnail: the mode of the track (the same one as in COVER OPTIONS)
            int mode = cover_track_video_mode(meta_sty_track_);   // 0 as in the VIDEO SETTINGS, 1 cover, 2 thumbnail, 3 plain disk
            mode = reset ? 0 : (mode + (dir > 0 ? 1 : 3)) % 4;
            set_cover_track_video_mode(meta_sty_track_, mode);
            ++video_rev_;
            snap_track_.clear();
            static const char* const names[4] = {"as in the VIDEO SETTINGS", "the cover", "the video thumbnail", "the plain disk"};
            meta_sty_msg_ = std::string("saved: ") + names[mode];
            return true;
        }
        case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 14: {   // the video of this track (-1 = as in the VIDEO overlay / in Settings)
            VideoTrackOpts o = cover_track_video_opts(meta_sty_track_);
            if (sel == 6) o.on = reset ? -1 : (o.on == 0 ? -1 : 0);
            else if (sel == 7) o.lyrics = reset ? -1 : sty_step(o.lyrics, dir, 1, 1);   // as in the VIDEO SETTINGS, off, on
            else if (sel == 8) o.size = reset ? -1 : (o.size + 1 + (dir > 0 ? 1 : 4)) % 5 - 1;   // -1, 0, 1, 2, 3 round
            else if (sel == 9) o.fx = reset ? -1 : video_fx_step(o.fx, dir, true);   // "as in the VIDEO SETTINGS", then the effects in the order of the list
            else if (sel == 10) o.contrast = reset ? -1 : sty_step(o.contrast, dir, 100, 5);
            else if (sel == 11) o.colors = reset ? -1 : sty_step(o.colors, dir, 100, 5);
            else if (sel == 12) o.chroma = reset ? -1 : sty_step(o.chroma, dir, kChromaLevels - 1, 1);
            else if (sel == 13) o.res = reset ? -1 : sty_step(o.res, dir, 2, 1);
            else o.round = reset ? -1 : sty_step(o.round, dir, 1, 1);
            set_cover_track_video_opts(meta_sty_track_, o);
            video_pic_key_.clear();
            if (sel == 6 || sel == 13) { video_for_track_.clear(); ++video_rev_; force_redraw_ = true; }   // (video on / off, resolution: the video is set up again; a look only makes the picture again)
            else if (sel == 8) force_redraw_ = true;   // (the size changes the layout)
            meta_sty_msg_ = "saved";
            return true;
        }
        default: {
            const int now = cover_album_look(album, folder);
            set_cover_album_look(album, folder, reset ? -1 : sty_step(now, dir, kCoverLookCount - 1, 1));
            break;
        }
    }
    if (sel < 5) set_cover_track_style(meta_sty_track_, st);
    meta_sty_msg_ = "saved";
    return true;
}

// Wide enough for the preview (the disk is 30 cells wide) when the terminal has room for it.
int App::meta_sty_panel_width(int screen_w) const { return screen_w >= 100 && term_rows_ >= 24 ? kMetaStyPanelWidth + 32 : kMetaStyPanelWidth; }

// The cover of the track as it looks with the style set in the overlay (the plain disk where the look is the disk or the track has no
// cover), prev_w x 15 cells, braille dots like the disk of the main screen. The last row names what is shown.
// The Style preview as a real picture (Kitty / Sixel): like album_cover_gfx(), for any cover and style, at a place of the screen (0-based).
bool App::meta_sty_gfx(const CoverImage& cover, int mode, int contrast, int brightness, int colors, int chroma, double angle, int col, int row, int cols, int rows) const {
    if (gfx_proto_ == GfxProto::None || cols < 8 || rows < 4) return false;
    mode = std::clamp(mode, 0, 4);
    int cw0 = 10, ch0 = 20;
    gfx_cell_pixels(settings_.cell_pixels, cw0, ch0);
    cw0 = std::max(4, cw0); ch0 = std::max(8, ch0);
    const int cws = std::min(cw0, std::max(2, 640 / cols));
    const int chs = std::min(ch0, std::max(4, 640 / rows));
    const int w = cols * cws, h = rows * chs;
    // Over the main screen (its own cover picture is in gfx_) the preview goes into the second slot, so both pictures are shown.
    const bool second = meta_sty_from_main_ && mode_ == Mode::MetaEdit && gfx_.active;   // (only in that overlay: the flag can outlive it)
    GfxFrame& g = second ? gfx2_ : gfx_;
    const int want_id = second ? 1 : 0;
    std::string& pic_key = second ? sty2_key_ : cover_pic_key_;
    auto& pic_at = second ? sty2_at_ : cover_pic_at_;
    const std::string key = "sty|" + cover.key + "|" + std::to_string(mode) + "|" + std::to_string(contrast) + "|" + std::to_string(brightness) + "|" + std::to_string(colors) + "|" + std::to_string(chroma) + "|" +
                            std::to_string(w) + "x" + std::to_string(h) + "|" + std::to_string(col) + "," + std::to_string(row) + "|" + settings_.disk_color + "|" +
                            (settings_.disk_color_gradient ? settings_.disk_color_end : std::string());
    const auto now = std::chrono::steady_clock::now();
    const bool resized = key != pic_key || !g.plain || g.w != w || g.h != h || g.level.size() != static_cast<size_t>(w) * h || g.id != want_id || g.col != col || g.cols != cols;
    const auto gap = std::chrono::milliseconds(gfx_proto_ == GfxProto::Sixel ? 110 : 40);
    const bool due = resized || (mode >= 1 && now - pic_at >= gap);
    cover_ovr_ = {true, contrast, brightness, colors, chroma};   // (cover_fill and cover_sixel_colours read the style through eff_cover_*)
    g.plain = true; g.truecolor = false;
    g.direct = mode == 0 || cover_sixel_colours(mode);
    g.scale = 1;
    g.out_cw = cw0; g.out_ch = ch0;
    g.cols = cols; g.rows = rows; g.w = w; g.h = h;
    g.col = col; g.row = row;
    g.id = want_id; g.grid.clear(); g.cut_w = 0; g.crop = 0;
    g.opaque = settings_.black_background;
    g.erase = !settings_.black_background;
    g.active = true;
    g.fresh = due;
    if (due) {
        pic_key = key;
        pic_at = now;
        static const auto t0 = std::chrono::steady_clock::now();   // (the gradient wave of mode 1 moves at 0.15 cycles per second)
        cover_fill(g, cover, mode, mode == 1 ? std::chrono::duration<double>(now - t0).count() * 0.15 : angle, w, h, cws, chs);
    }
    cover_ovr_.on = false;
    meta_sty_pic_ = second ? 2 : 1;
    return true;
}

std::vector<std::string> App::meta_sty_preview(int prev_w, int rows, const CoverStyle& st, int album_look, int screen_w) const {
    std::vector<std::string> out;
    const int look = st.look >= 0 ? st.look : album_look >= 0 ? album_look : (settings_.album_cover ? 1 + std::clamp(settings_.cover_mode, 0, 6) : 0);
    const int contrast = st.contrast >= 0 ? st.contrast : settings_.cover_contrast;
    const int brightness = st.brightness >= 0 ? st.brightness : settings_.cover_brightness;
    const int colors = st.colors >= 0 ? st.colors : settings_.cover_colors;
    const int chroma = st.chroma >= 0 ? st.chroma : settings_.cover_chroma;
    const double angle = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() * 0.6;
    const int dw = disk_.width(), dh = disk_.height();
    std::string note = cover_look_name(look);
    std::vector<std::string> disk;
    auto plain_disk = [&]() {
        disk = disk_.frame(angle);
        for (size_t i = 0; i < disk.size(); ++i) {
            const float t = disk.size() > 1 ? static_cast<float>(i) / static_cast<float>(disk.size() - 1) : 0.0f;
            disk[i] = gradient_ansi(settings_.disk_color, settings_.disk_color_end, t) + disk[i] + "\x1b[0m";
        }
    };
    if (look > 0) {
        CoverRequest rq;
        rq.path = meta_sty_track_;
        rq.title = meta_hover_values_[static_cast<size_t>(MetaField::Title)];   // (like the disk: the tag title, else the file name)
        if (rq.title.empty() || rq.title == "-") rq.title = path_utf8(path_from_utf8(meta_sty_track_).stem());
        rq.album = meta_hover_values_[static_cast<size_t>(MetaField::Album)];
        if (rq.album == "-") rq.album.clear();
        rq.search = settings_.cover_search;
        rq.store_rev = cover_store_revision();
        // what the disk shows of a track with a video: the cover, a frame of the video or the plain disk (Style Option / VIDEO overlay)
        const int own = cover_track_video_mode(rq.path);
        const bool has_video = !video_source_for(path_from_utf8(meta_sty_track_)).empty();
        const int vm = own == 1 ? 0 : own == 2 ? 1 : own == 3 ? 2 : settings_.video_mode ? settings_.video_cover : 0;
        const bool plain_video = own == 3 || (own == 0 && vm == 2 && has_video);
        if (vm == 1 && has_video) rq.snapshot_video = video_source_for(path_from_utf8(meta_sty_track_));
        const bool thumb_mode = vm == 1 && has_video && !plain_video;
        const CoverImage* thumb = thumb_mode ? track_cover(rq) : nullptr;   // a frame of the video, as the disk shows it
        const CoverSource src = plain_video || thumb_mode ? CoverSource() : resolve_cover_source(rq);
        if (plain_video) { plain_disk(); note = "plain disk (the Video setting of this track)"; }
        else if (thumb_mode && !thumb) { plain_disk(); note = "reading a frame of the video ..."; }
        else if (!thumb_mode && (src.none || src.file.empty())) { plain_disk(); note = "no cover: the disk is shown"; }
        else if (const CoverImage* img = thumb_mode ? thumb : preview_cover(src.file, src.embedded)) {
            const int m = look - 1;
            const int lpad0 = std::max(0, (prev_w - dw) / 2);
            // where the panel stands on the screen (the same arithmetic as draw_floating_panel()): the picture goes into the preview column
            const int W = meta_sty_panel_width(screen_w), panel_h = rows + 2;
            const int start_col = 1 + std::max(0, (screen_w - W) / 2);
            const int start_row = std::clamp(1 + std::max(0, (term_rows_ - panel_h) / 2 - kFloatingPanelUpShift + 1), 1, std::max(1, term_rows_ - panel_h));   // (the panel stands one row lower than the other floating panels)
            const int top0 = std::max(0, (rows - 1 - dh) / 2);   // the disk is centred in the rows above the note
            if (cf_full_res_ && m <= 4 && meta_sty_gfx(*img, m, contrast, brightness, colors, chroma, angle, (start_col - 1) + 1 + (kMetaStyPanelWidth - 2) + 1 + lpad0, start_row + top0, dw, dh)) {
                for (int r = 0; r < dh; ++r) disk.push_back(gfx_hole(dw));
                note += "  (image)";
            } else {
                disk = album_cover_lines_for(*img, m >= 5 ? m - 5 : m, contrast, brightness, colors, angle, dw, dh, chroma);
                if (disk.empty()) plain_disk();
                else if (gfx_proto_ != GfxProto::None) note += "  (braille)";
            }
        } else {
            plain_disk();
            note = preview_cover_loading(src.file, src.embedded) ? "reading the cover ..." : "the cover cannot be read";
        }
    } else {
        plain_disk();
    }
    const int lpad = std::max(0, (prev_w - dw) / 2);
    const int top = std::max(0, (rows - 1 - dh) / 2);
    for (int r = 0; r < rows - 1; ++r) {
        const int dr = r - top;
        const std::string row = dr >= 0 && dr < static_cast<int>(disk.size()) ? disk[static_cast<size_t>(dr)] : std::string(static_cast<size_t>(dw), ' ');
        out.push_back(std::string(static_cast<size_t>(lpad), ' ') + row + "\x1b[0m" + std::string(static_cast<size_t>(std::max(0, prev_w - lpad - dw)), ' '));
    }
    const std::string t = truncate_str(note, prev_w);
    const int tl = std::max(0, (prev_w - display_width(t)) / 2);
    out.push_back(legend_sgr(settings_) + std::string(static_cast<size_t>(tl), ' ') + t + "\x1b[0m" + std::string(static_cast<size_t>(std::max(0, prev_w - tl - display_width(t))), ' '));
    return out;
}

std::vector<std::string> App::build_meta_sty_panel(int screen_w) const {
    const int W = meta_sty_panel_width(screen_w);
    const bool with_prev = W > kMetaStyPanelWidth;
    const int left_w = kMetaStyPanelWidth - 2;
    const int prev_w = with_prev ? W - kMetaStyPanelWidth - 1 : 0;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    const std::string album = meta_hover_values_[static_cast<size_t>(MetaField::Album)];
    const std::string folder = path_utf8(path_from_utf8(meta_sty_track_).parent_path());
    const CoverStyle st = cover_track_style(meta_sty_track_);
    const int album_look = cover_album_look(album, folder);
    auto pct = [&](int own, int global) {
        return own >= 0 ? std::to_string(own) + " %" : std::string("as in Settings (") + std::to_string(global) + " %)";
    };
    auto look_text = [&](int own) { return own >= 0 ? std::string(cover_look_name(own)) : std::string("as in Settings"); };
    struct Row { const char* label; std::string value; };
    std::vector<Row> rows = {
        {"Look of this track", look_text(st.look)},
        {"Cover Contrast", pct(st.contrast, settings_.cover_contrast)},
        {"Cover Brightness", pct(st.brightness, settings_.cover_brightness)},
        {"Cover Colors", pct(st.colors, settings_.cover_colors)},
        {"Cover Chromatic Aberration", st.chroma >= 0 ? std::string(chroma_name(st.chroma)) : std::string("as in Settings (") + chroma_name(settings_.cover_chroma) + ")"},
    };
    if (settings_.video_mode) {   // the same choice as COVER OPTIONS (this track) and the VIDEO overlay (the default of all tracks)
        const int own = cover_track_video_mode(meta_sty_track_);
        static const char* const dflt[3] = {"the cover", "the video thumbnail", "the plain disk"};
        static const char* const names[4] = {"", "the cover", "the video thumbnail", "the plain disk"};
        rows.push_back({"Use Cover or Thumbnail", own == 0 ? std::string("as in VIDEO SETTINGS (") + dflt[std::clamp(settings_.video_cover, 0, 2)] + ")" : std::string(names[own])});
        const VideoTrackOpts vo = cover_track_video_opts(meta_sty_track_);
        static const char* const size_names[4] = {"regular", "cropped", "full", "framed"};
        rows.push_back({"Video of this track", vo.on == 0 ? std::string("off") : std::string("on")});
        rows.push_back({"Use Lyrics on Video", vo.lyrics >= 0 ? std::string(vo.lyrics ? "on" : "off") : std::string("as in VIDEO SETTINGS (") + (settings_.video_lyrics ? "on" : "off") + ")"});
        rows.push_back({"Video Size", vo.size >= 0 ? std::string(size_names[vo.size]) : std::string("as in VIDEO SETTINGS (") + size_names[std::clamp(settings_.video_size, 0, 3)] + ")"});
        rows.push_back({"Video Effect", vo.fx >= 0 ? std::string(video_fx_name(vo.fx)) : std::string("VIDEO SETTINGS (") + video_fx_name(settings_.video_fx) + ")"});
        rows.push_back({"Video Contrast", pct(vo.contrast, settings_.video_contrast)});
        rows.push_back({"Video Colors", pct(vo.colors, settings_.video_colors)});
        rows.push_back({"Video Chromatic Aberration", vo.chroma >= 0 ? std::string(chroma_name(vo.chroma)) : std::string("as in Settings (") + chroma_name(settings_.video_chroma) + ")"});
        rows.push_back({"Video Resolution", vo.res >= 0 ? std::string(video_res_name(vo.res)) : std::string("as in Settings (") + video_res_name(settings_.video_res) + ")"});
        rows.push_back({"Video Rounded Corners", vo.round >= 0 ? std::string(vo.round ? "on" : "off") : std::string("as in Settings (") + (settings_.video_round ? "on" : "off") + ")"});
    }
    rows.push_back({"Look of the whole album", look_text(album_look)});
    // the left column, one string per row (left_w columns each)
    std::vector<std::string> left;
    auto plain = [&](const std::string& t) { return pad_right(truncate_str(t, left_w), left_w); };
    {   // the note in the header colour (the empty row after it is gone)
        const std::string hc = ansi_for(settings_.header_color, false);
        left.push_back(hc + plain(" Only for this track (last row: its whole album).") + (hc.empty() ? "" : "\x1b[0m"));
    }
    const int first_video = settings_.video_mode ? 6 : -1;
    const int album_row = static_cast<int>(rows.size()) - 1;
    auto head = [&](const char* name) {   // a category line (not a row of the list)
        std::string t = std::string(" \u2500\u2500 ") + name + " ";
        while (display_width(t) < left_w - 1) t += "\u2500";
        left.push_back(border + pad_right(truncate_str(t, left_w - 1), left_w - 1) + (border.empty() ? "" : "\x1b[0m") + " ");
    };
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        if (i == 0) head("Cover");
        else if (i == first_video) head("Video");
        else if (i == album_row) head("Album");
        std::string t = " " + pad_right(rows[static_cast<size_t>(i)].label, 27) + " \u25c0 " + rows[static_cast<size_t>(i)].value + " \u25b6";
        t = pad_right(truncate_str(t, left_w - 1), left_w - 1) + " ";
        if (i == meta_sty_sel_) t = "\x1b[7m" + t + "\x1b[0m";
        left.push_back(t);
    }
    left.push_back(plain(meta_sty_msg_.empty() ? std::string() : " " + meta_sty_msg_));
    const int body_rows = with_prev ? std::max(15, static_cast<int>(left.size())) : static_cast<int>(left.size());
    while (static_cast<int>(left.size()) < body_rows) left.push_back(plain(""));
    std::vector<std::string> prev;
    if (with_prev) prev = meta_sty_preview(prev_w, body_rows, st, album_look, screen_w);

    std::vector<std::string> lines;
    lines.push_back(box_top("STYLE OPTION  " + truncate_str(path_utf8(path_from_utf8(meta_sty_track_).filename()), 40), W, border));
    for (int r = 0; r < body_rows; ++r) {
        std::string row = obar + left[static_cast<size_t>(r)];
        if (with_prev) row += obar + prev[static_cast<size_t>(r)] + "\x1b[0m";
        lines.push_back(row + obar);
    }
    {   // the legend, centred in the bottom border
        const std::string foot = std::string(" ") + (meta_sty_from_main_ ? "[\u2191\u2193] select  [\u2190\u2192] change  [DEL/d] reset  [t] image/braille  [m] meta data  [ESC] close"
                                                                         : "[\u2191\u2193] select  [\u2190\u2192] change  [DEL/d] as in Settings  [t] image/braille  [ESC] close") + " ";
        const int fw = display_width(foot);
        const int total = W - 2;
        const int l = std::max(0, (total - fw) / 2), r = std::max(0, total - fw - l);
        std::string b = settings_.box_lower_left;
        for (int i = 0; i < l; ++i) b += settings_.box_horizontal;
        b += foot;
        for (int i = 0; i < r; ++i) b += settings_.box_horizontal;
        b += settings_.box_lower_right;
        lines.push_back(border.empty() ? b : border + b + "\x1b[0m");
    }
    return lines;
}

bool App::handle_meta_cov_key(int key) {
    if (!meta_cov_open_) return false;
    const bool arrow = last_key_was_arrow();
    const int n = static_cast<int>(meta_cov_items_.size());
    if (meta_cov_ask_delete_) {
        if (key == 'y' || key == 'Y') {
            meta_cov_ask_delete_ = false;
            if (meta_cov_sel_ >= 0 && meta_cov_sel_ < n) {
                std::string why;
                if (delete_cover_candidate(meta_cov_items_[static_cast<size_t>(meta_cov_sel_)].c, &why)) meta_cov_msg_ = "picture deleted";
                else meta_cov_msg_ = why.empty() ? "cannot be deleted" : why;
                meta_cov_rebuild();
            }
        } else if (key == 'n' || key == 'N' || key == 27) {
            meta_cov_ask_delete_ = false;
        }
        return true;
    }
    if (key == 27) {
        if (meta_cov_search_) { meta_cov_search_ = false; text_disengage(); return true; }
        if (!meta_cov_query_.empty()) { meta_cov_query_.clear(); edit_caret_ = edit_anchor_ = 0; meta_cov_rebuild(); return true; }
        meta_cov_open_ = false;
        return true;
    }
    if (key == 9) {   // TAB: the disk options -> the pictures -> the search field -> ...
        const bool has_modes = n > 0 && meta_cov_items_[0].special >= 10;
        const bool in_modes = !meta_cov_search_ && n > 0 && meta_cov_items_[static_cast<size_t>(std::clamp(meta_cov_sel_, 0, n - 1))].special >= 10;
        auto select_picture = [&]() {   // the picture in use (else Automatic, else the first)
            const std::string use = cover_track_choice(meta_cov_track_);
            int at = -1;
            for (int i = 0; i < n && at < 0; ++i) {
                const MetaCovItem& it = meta_cov_items_[static_cast<size_t>(i)];
                if ((it.special == 1 && use.empty()) || (it.special == 0 && !use.empty() && choice_of(it.c) == use)) at = i;
            }
            if (at < 0) for (int i = 0; i < n && at < 0; ++i) if (meta_cov_items_[static_cast<size_t>(i)].special < 10) at = i;
            if (at >= 0) meta_cov_sel_ = at;
        };
        if (meta_cov_search_) {   // the search field -> the disk options (when listed), else the pictures
            meta_cov_search_ = false;
            text_disengage();
            if (has_modes) meta_cov_sel_ = std::clamp(cover_track_video_mode(meta_cov_track_), 0, 3); else select_picture();
        } else if (in_modes) {    // the disk options -> the pictures
            select_picture();
        } else {                  // the pictures -> the search field
            meta_cov_search_ = true;
        }
        return true;
    }
    if (arrow && (key == 'A' || key == 'B')) {
        if (n > 0) meta_cov_sel_ = std::clamp(meta_cov_sel_ + (key == 'A' ? -1 : 1) * (last_key_was_shifted() ? 8 : 1), 0, n - 1);
        return true;
    }
    if (meta_cov_search_) {
        if (key == '\r' || key == '\n') { meta_cov_search_ = false; text_disengage(); return true; }
        edit_focus("meta-cov-search", meta_cov_query_);
        text_engage("meta-cov-search");
        if (edit_text_key(meta_cov_query_, edit_caret_, edit_anchor_, key, 80, &meta_cov_msg_)) { meta_cov_sel_ = 0; meta_cov_rebuild(); }
        return true;
    }
    if (key == '/') { meta_cov_search_ = true; return true; }
    if (key == 't' && !arrow) { meta_toggle_cover_res(); meta_cov_msg_ = meta_status_; return true; }   // the same switch as in the meta editor
    if ((key == '\r' || key == '\n') && n > 0) {
        const MetaCovItem& it = meta_cov_items_[static_cast<size_t>(meta_cov_sel_)];
        std::string choice;
        std::string what;
        if (it.special == 1) { choice = ""; what = "automatic: Settings -> Cover Search decides again"; }
        else if (it.special == 2) { choice = kCoverNone; what = "no cover (the disk is shown)"; }
        else { choice = choice_of(it.c); what = it.c.label; }
        if (it.special >= 10) {   // Cover, Disk or Thumbnail of this track: a mode of its own, the picture choice stays as it is
            const int mode = it.special - 10;
            set_cover_track_video_mode(meta_cov_track_, mode);
            ++video_rev_;
            snap_track_.clear();
            meta_cov_open_ = false;
            static const char* const names[4] = {"as in the VIDEO SETTINGS", "the cover", "a frame of its video, else the cover", "the plain disk"};
            meta_status_ = std::string("this track shows: ") + names[mode] + (mode == 2 && video_source_for(path_from_utf8(meta_cov_track_)).empty() ? " (no video yet: the cover is used)" : "");
            return true;
        }
        set_cover_track_choice(meta_cov_track_, choice);
        meta_cov_open_ = false;
        meta_status_ = "cover of this track: " + what;
        return true;
    }
    if ((key == kKeyDelete || key == 127 || key == 'd') && n > 0) {
        const MetaCovItem& it = meta_cov_items_[static_cast<size_t>(meta_cov_sel_)];
        if (it.special == 1 || (it.special == 0 && it.c.kind == CoverCandidate::Embedded)) {   // no picture to take away: this track gets no cover at all
            set_cover_track_choice(meta_cov_track_, kCoverNone);
            meta_cov_msg_ = "no cover for this track now: the disk is shown (pick Automatic to get a picture back)";
            meta_cov_rebuild();
            for (size_t i = 0; i < meta_cov_items_.size(); ++i) if (meta_cov_items_[i].special == 2) { meta_cov_sel_ = static_cast<int>(i); break; }
            return true;
        }
        if (it.special != 0) { meta_cov_msg_ = it.special == 2 ? "this track has no cover already" : "nothing to delete here"; return true; }
        if (!cover_deletable(it.c)) { meta_cov_msg_ = "this picture cannot be deleted"; return true; }
        meta_cov_ask_delete_ = true;
        return true;
    }
    return true;
}

std::vector<std::string> App::build_meta_cov_panel(int screen_w) const {
    const int W = kMetaCovPanelWidth;
    const int inner = W - 2;
    const int list_w = 40;
    const int prev_w = inner - list_w - 1;
    const int rows = 14;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    const int n = static_cast<int>(meta_cov_items_.size());
    std::vector<std::string> lines;
    lines.push_back(box_top("COVER OPTIONS  " + truncate_str(path_utf8(path_from_utf8(meta_cov_track_).filename()), 40), W, border));
    {   // search by file name
        std::string field;
        int cols = 0;
        if (meta_cov_search_) {
            EditPaint p = paint_edit_field(meta_cov_query_, edit_caret_, edit_anchor_, 40, search_sgr(settings_), true);
            field = search_run(settings_, p.s, true);
            cols = p.cols;
        } else {
            const std::string plain = truncate_str(meta_cov_query_.empty() ? std::string("(TAB or / : search pictures by file name)") : meta_cov_query_, 60);
            field = search_run(settings_, plain, !meta_cov_query_.empty());
            cols = display_width(plain);
        }
        const std::string pre = " File name: ";
        lines.push_back(obar + search_idle_sgr(settings_) + pre + field + "\x1b[0m" + std::string(static_cast<size_t>(std::max(0, inner - cols - static_cast<int>(pre.size()))), ' ') + obar);
    }
    const std::string cur = cover_track_choice(meta_cov_track_);
    // the preview of the highlighted picture
    std::vector<std::string> prev;
    {
        std::string file, msg;
        bool emb = false;
        if (n > 0) {
            const MetaCovItem& it = meta_cov_items_[static_cast<size_t>(meta_cov_sel_)];
            if (it.special == 0) { file = path_utf8(it.c.file); emb = it.c.kind == CoverCandidate::Embedded; }
            else if (it.special == 13) msg = "the plain disk";
            else if (it.special == 2) msg = "no cover: the disk is shown";
            else if (it.special == 12 && !video_source_for(path_from_utf8(meta_cov_track_)).empty()) msg = "a frame of the video";
            else {   // Automatic (and the disk options that show the cover): the picture the search finds for the track, without its own choice
                CoverRequest rq;
                rq.path = meta_cov_track_;
                rq.title = path_utf8(path_from_utf8(meta_cov_track_).stem());
                rq.album = meta_hover_values_[static_cast<size_t>(MetaField::Album)];
                rq.search = settings_.cover_search;
                rq.store_rev = cover_store_revision();
                rq.ignore_track_choice = true;
                const CoverSource src = resolve_cover_source(rq);
                if (!src.none && !src.file.empty()) { file = path_utf8(src.file); emb = src.embedded; }
                else msg = it.special == 12 ? "no video yet and no cover found: the disk" : "no cover found: the disk";
            }
        } else msg = "no picture found";
        // [t] (as in the meta editor and the cover fetch): the preview as a real picture (Kitty / Sixel) or as braille dots
        bool drawn = false;
        if (cf_full_res_ && gfx_proto_ != GfxProto::None && !file.empty()) {
            // where the floating panel stands (the same arithmetic as draw_floating_panel()); the preview is right of the list
            const int panel_h = rows + 4;
            const int start_col = 1 + std::max(0, (screen_w - W) / 2);
            const int start_row = std::clamp(1 + std::max(0, (term_rows_ - panel_h) / 2 - kFloatingPanelUpShift), 1, std::max(1, term_rows_ - panel_h));
            if (cover_full_picture(file, emb, prev_w, rows, (start_col - 1) + 1 + list_w + 1, (start_row - 1) + 2)) {
                for (int r = 0; r < rows; ++r) prev.push_back(gfx_hole(prev_w));
                drawn = true;
            }
        }
        if (!drawn) prev = cover_preview_lines(file, emb, prev_w, rows, msg);
    }
    // The list in two groups so the three "modes" are not mistaken for search results: first how THIS track gets its picture (a choice of
    // one of three, drawn as radio marks), then the pictures that were found (a mark on the one in use).
    struct Row { int item; std::string text; };   // item -1: a heading
    auto heading = [&](const std::string& t) {
        std::string h = "\u2500\u2500 " + t + " ";
        while (display_width(h) < list_w - 1) h += settings_.box_horizontal;
        return Row{-1, h};
    };
    // The first group (what the disk shows of this track when it has a video) always stays in view; the picture of the track (Automatic, No
    // cover, the pictures that were found) scrolls below it.
    const int vmode_now = cover_track_video_mode(meta_cov_track_);
    std::vector<Row> top, pics;
    for (int k = 0; k < n; ++k) {
        const MetaCovItem& it = meta_cov_items_[static_cast<size_t>(k)];
        if (it.special >= 10) {
            if (top.empty()) top.push_back(heading("The disk shows"));
            static const char* const labels[4] = {"As in the VIDEO SETTINGS (default)", "The cover", "The video thumbnail, else the cover", "The plain disk"};
            top.push_back({k, std::string(it.special - 10 == vmode_now ? "(o) " : "( ) ") + labels[it.special - 10]});
        } else if (it.special == 1) {
            pics.push_back({k, std::string(cur.empty() ? "(o) " : "( ) ") + "Automatic (as in Settings)"});
        } else if (it.special == 2) {
            pics.push_back({k, "(o) No cover: the disk is shown"});
        } else {
            pics.push_back({k, std::string(choice_of(it.c) == cur ? "[x] " : "[ ] ") + it.c.label});
        }
    }
    const Row pics_head = heading(meta_cov_query_.empty() ? "Picture of this track" : "Pictures matching");
    std::vector<Row> disp = top;
    {
        const int room = std::max(1, rows - static_cast<int>(top.size()) - (pics.empty() ? 0 : 1));   // (the heading of the pictures takes a row)
        int sel_p = 0;
        for (size_t d = 0; d < pics.size(); ++d) if (pics[d].item == meta_cov_sel_) sel_p = static_cast<int>(d);
        const int total_p = static_cast<int>(pics.size());
        const int first_p = std::clamp(sel_p - room / 2, 0, std::max(0, total_p - room));
        if (!pics.empty()) disp.push_back(pics_head);
        for (int d = first_p; d < total_p && d < first_p + room; ++d) disp.push_back(pics[static_cast<size_t>(d)]);
    }
    const int total = static_cast<int>(disp.size());
    const int first = 0;
    const std::string R = "\x1b[0m";
    for (int r = 0; r < rows; ++r) {
        const int k = first + r;
        std::string left;
        if (k < total) {
            const Row& dr = disp[static_cast<size_t>(k)];
            if (dr.item < 0) left = " " + legend_sgr(settings_) + pad_right(truncate_str(dr.text, list_w - 1), list_w - 1) + R;
            else {
                left = pad_right(truncate_str(" " + dr.text, list_w), list_w);
                if (dr.item == meta_cov_sel_) left = "\x1b[7m" + left + R;   // lit while the search field is in use too: [↑↓] pick while typing
            }
        } else {
            left = std::string(static_cast<size_t>(list_w), ' ');
        }
        const std::string right = r < static_cast<int>(prev.size()) ? prev[static_cast<size_t>(r)] : std::string(static_cast<size_t>(prev_w), ' ');
        lines.push_back(obar + left + obar + right + "\x1b[0m" + obar);
    }
    std::string msg = meta_cov_ask_delete_ ? "Delete this picture from the disk?   [Y]es   [N]o" : meta_cov_msg_;
    lines.push_back(obar + pad_right(truncate_str(" " + msg, inner), inner) + obar);
    lines.push_back(box_bottom(W, "[↑↓] select  [ENTER] use  [TAB] disk/pictures/search  [t] ASCII/full  [DEL] delete picture / no cover  [ESC] close", border));
    return lines;
}

}  // namespace muisc
