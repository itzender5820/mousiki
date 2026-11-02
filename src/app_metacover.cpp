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
    rq.title = path_utf8(path_from_utf8(path).stem());
    rq.album = meta_hover_values_[static_cast<size_t>(MetaField::Album)];
    rq.search = settings_.cover_search;
    rq.store_rev = cover_store_revision();
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
        meta_cov_items_.push_back({1, {}});
        meta_cov_items_.push_back({2, {}});
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
        meta_cov_items_.insert(meta_cov_items_.begin() + (q.empty() ? 2 : 0), MetaCovItem{0, e});
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
    if (arrow && (key == 'A' || key == 'B')) { meta_sty_sel_ = (meta_sty_sel_ + (key == 'A' ? 4 : 1)) % 5; return true; }
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
    switch (meta_sty_sel_) {
        case 0: st.look = reset ? -1 : sty_step(st.look, dir, kCoverLookCount - 1, 1); break;
        case 1: st.contrast = reset ? -1 : sty_step(st.contrast, dir, 100, 5); break;
        case 2: st.brightness = reset ? -1 : sty_step(st.brightness, dir, 100, 5); break;
        case 3: st.colors = reset ? -1 : sty_step(st.colors, dir, 100, 5); break;
        default: {
            const int now = cover_album_look(album, folder);
            set_cover_album_look(album, folder, reset ? -1 : sty_step(now, dir, kCoverLookCount - 1, 1));
            break;
        }
    }
    if (meta_sty_sel_ < 4) set_cover_track_style(meta_sty_track_, st);
    meta_sty_msg_ = "saved";
    return true;
}

// Wide enough for the preview (the disk is 30 cells wide) when the terminal has room for it.
int App::meta_sty_panel_width(int screen_w) const { return screen_w >= 100 && term_rows_ >= 24 ? kMetaStyPanelWidth + 32 : kMetaStyPanelWidth; }

// The cover of the track as it looks with the style set in the overlay (the plain disk where the look is the disk or the track has no
// cover), prev_w x 15 cells, braille dots like the disk of the main screen. The last row names what is shown.
// The Style preview as a real picture (Kitty / Sixel): like album_cover_gfx(), for any cover and style, at a place of the screen (0-based).
bool App::meta_sty_gfx(const CoverImage& cover, int mode, int contrast, int brightness, int colors, double angle, int col, int row, int cols, int rows) const {
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
    const std::string key = "sty|" + cover.key + "|" + std::to_string(mode) + "|" + std::to_string(contrast) + "|" + std::to_string(brightness) + "|" + std::to_string(colors) + "|" +
                            std::to_string(w) + "x" + std::to_string(h) + "|" + std::to_string(col) + "," + std::to_string(row) + "|" + settings_.disk_color + "|" +
                            (settings_.disk_color_gradient ? settings_.disk_color_end : std::string());
    const auto now = std::chrono::steady_clock::now();
    const bool resized = key != pic_key || !g.plain || g.w != w || g.h != h || g.level.size() != static_cast<size_t>(w) * h || g.id != want_id || g.col != col || g.cols != cols;
    const auto gap = std::chrono::milliseconds(gfx_proto_ == GfxProto::Sixel ? 110 : 40);
    const bool due = resized || (mode >= 1 && now - pic_at >= gap);
    cover_ovr_ = {true, contrast, brightness, colors};   // (cover_fill and cover_sixel_colours read the style through eff_cover_*)
    g.plain = true;
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
        rq.title = path_utf8(path_from_utf8(meta_sty_track_).stem());
        rq.album = meta_hover_values_[static_cast<size_t>(MetaField::Album)];
        rq.search = settings_.cover_search;
        rq.store_rev = cover_store_revision();
        const CoverSource src = resolve_cover_source(rq);
        if (src.none || src.file.empty()) { plain_disk(); note = "no cover: the disk is shown"; }
        else if (const CoverImage* img = preview_cover(src.file, src.embedded)) {
            const int m = look - 1;
            const int lpad0 = std::max(0, (prev_w - dw) / 2);
            // where the panel stands on the screen (the same arithmetic as draw_floating_panel()): the picture goes into the preview column
            const int W = meta_sty_panel_width(screen_w), panel_h = rows + 2;
            const int start_col = 1 + std::max(0, (screen_w - W) / 2);
            const int start_row = std::clamp(1 + std::max(0, (term_rows_ - panel_h) / 2 - kFloatingPanelUpShift), 1, std::max(1, term_rows_ - panel_h));
            if (cf_full_res_ && m <= 4 && meta_sty_gfx(*img, m, contrast, brightness, colors, angle, (start_col - 1) + 1 + (kMetaStyPanelWidth - 2) + 1 + lpad0, start_row, dw, dh)) {
                for (int r = 0; r < dh; ++r) disk.push_back(gfx_hole(dw));
                note += "  (image)";
            } else {
                disk = album_cover_lines_for(*img, m >= 5 ? m - 5 : m, contrast, brightness, colors, angle, dw, dh);
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
    for (int r = 0; r < rows - 1; ++r) {
        const std::string row = r < static_cast<int>(disk.size()) ? disk[static_cast<size_t>(r)] : std::string(static_cast<size_t>(dw), ' ');
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
    const Row rows[5] = {
        {"Look of this track", look_text(st.look)},
        {"Cover Contrast", pct(st.contrast, settings_.cover_contrast)},
        {"Cover Brightness", pct(st.brightness, settings_.cover_brightness)},
        {"Cover Colors", pct(st.colors, settings_.cover_colors)},
        {"Look of the whole album", look_text(album_look)},
    };
    // the left column, one string per row (left_w columns each)
    std::vector<std::string> left;
    auto plain = [&](const std::string& t) { return pad_right(truncate_str(t, left_w), left_w); };
    left.push_back(plain(" Only for this track (last row: its whole album)."));
    left.push_back(plain(""));
    for (int i = 0; i < 5; ++i) {
        std::string t = " " + pad_right(rows[i].label, 24) + " \u25c0 " + rows[i].value + " \u25b6";
        t = pad_right(truncate_str(t, left_w - 1), left_w - 1) + " ";
        if (i == meta_sty_sel_) t = "\x1b[7m" + t + "\x1b[0m";
        left.push_back(t);
    }
    left.push_back(plain(""));
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
    if (key == 9) { meta_cov_search_ = !meta_cov_search_; if (!meta_cov_search_) text_disengage(); return true; }
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
        set_cover_track_choice(meta_cov_track_, choice);
        meta_cov_open_ = false;
        meta_status_ = "cover of this track: " + what;
        return true;
    }
    if ((key == kKeyDelete || key == 127 || key == 'd') && n > 0) {
        const MetaCovItem& it = meta_cov_items_[static_cast<size_t>(meta_cov_sel_)];
        if (it.special != 0) { meta_cov_msg_ = "nothing to delete here"; return true; }
        if (!cover_deletable(it.c)) { meta_cov_msg_ = "the picture inside a file cannot be deleted"; return true; }
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
            else if (it.special == 2) msg = "no cover";
            else msg = "automatic";
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
    const int first = std::clamp(meta_cov_sel_ - rows / 2, 0, std::max(0, n - rows));
    for (int r = 0; r < rows; ++r) {
        const int k = first + r;
        std::string left;
        if (k < n) {
            const MetaCovItem& it = meta_cov_items_[static_cast<size_t>(k)];
            std::string label, mark = "   ";
            if (it.special == 1) { label = "Automatic (Settings -> Cover Search)"; if (cur.empty()) mark = " * "; }
            else if (it.special == 2) { label = "No cover (show the disk)"; if (cur == kCoverNone) mark = " * "; }
            else { label = it.c.label; if (choice_of(it.c) == cur) mark = " * "; }
            left = pad_right(truncate_str(mark + label, list_w), list_w);
            if (k == meta_cov_sel_) left = "\x1b[7m" + left + "\x1b[0m";   // lit while the search field is in use too: [↑↓] pick while typing
        } else {
            left = std::string(static_cast<size_t>(list_w), ' ');
        }
        const std::string right = r < static_cast<int>(prev.size()) ? prev[static_cast<size_t>(r)] : std::string(static_cast<size_t>(prev_w), ' ');
        lines.push_back(obar + left + obar + right + "\x1b[0m" + obar);
    }
    std::string msg = meta_cov_ask_delete_ ? "Delete this picture from the disk?   [Y]es   [N]o" : meta_cov_msg_;
    lines.push_back(obar + pad_right(truncate_str(" " + msg, inner), inner) + obar);
    lines.push_back(box_bottom(W, "[↑↓] select  [ENTER] use  [t] ASCII/full  [DEL] delete  [TAB] search  [ESC] close", border));
    return lines;
}

}  // namespace muisc
