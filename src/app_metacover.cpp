// The cover in the meta data editor: the picture under the fields and the "Cover Options" overlay (choose a picture for the track,
// "no cover", back to automatic, delete a fetched / folder picture, search pictures by file name). Member functions of App.
#include "app.h"
#include "meta_state.h"
#include "coverfetch_state.h"
#include "overlay_rows.h"
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
    rq.title = meta_->hover_values[static_cast<size_t>(MetaField::Title)];   // (like the disk: the tag title, else the file name)
    if (rq.title.empty() || rq.title == "-") rq.title = path_utf8(path_from_utf8(path).stem());
    rq.album = meta_->hover_values[static_cast<size_t>(MetaField::Album)];
    if (rq.album == "-") rq.album.clear();
    rq.search = settings_.cover_search;
    rq.store_rev = cover_store_revision();
    rq.pair_path = paired_song_for_video(rq.path);
    // what the disk shows of this track (Cover Options / Style Option / VIDEO SETTINGS): the cover, a frame of its video, or the cover with the frame as fallback
    const int own = cover_track_video_mode(path);
    const bool has_video = !video_source_for(path_from_utf8(path)).empty();
    const int vm = own == 1 ? 0 : own == 2 ? 1 : own == 3 ? 2 : own == 4 ? 3 : settings_.video_mode ? settings_.video_cover : 0;   // 0 cover, 1 thumbnail, 2 disk, 3 thumbnail fallback
    const bool plain_disk = own == 3 || (own == 0 && vm == 2 && has_video);
    const bool overlay_up0 = meta_->choice_open || meta_->cov_open || meta_->sty_open || meta_->save.active;
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
    if ((vm == 1 || vm == 3) && has_video && !(vm == 3 && cover_track_choice(path) == kCoverNone)) {   // thumbnail: a frame of the video is the current picture (fallback: when there is no cover)
        CoverRequest tq = rq;
        tq.thumb_fallback = vm == 3;
        tq.snapshot_video = video_source_for(path_from_utf8(path));
        if (const CoverImage* img = track_cover(tq, true)) {
            if (cf_->full_res && gfxs_.proto != GfxProto::None && !overlay_up0 &&
                cover_full_picture_img(img, inner, rows, meta_->pane_col + 2, 4 + kMetaFieldCount + 3)) {
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
    const bool overlay_up = meta_->choice_open || meta_->cov_open || meta_->sty_open || meta_->save.active;   // (a picture would be drawn over a floating panel)
    if (cf_->full_res && gfxs_.proto != GfxProto::None && !overlay_up && !src.none && !src.file.empty()) {
        if (cover_full_picture(src.file, src.embedded, inner, rows, meta_->pane_col + 2, 4 + kMetaFieldCount + 3)) {
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
    if (gfxs_.proto == GfxProto::None) { meta_->status = "this terminal shows no pictures (no Kitty / Sixel graphics): the cover stays ASCII"; return; }
    cf_->full_res = !cf_->full_res;
    cf_->pic_key.clear();
    meta_->status = cf_->full_res ? "cover: full resolution" : "cover: ASCII";
}

void App::meta_cov_open() {
    const std::string path = meta_hovering_path();
    if (path.empty()) { meta_->status = "no title selected"; return; }
    meta_->cov_track = path;
    meta_->cov_open = true;
    meta_->cov_search = false;
    meta_->cov_ask_delete = false;
    meta_->cov_query.clear();
    meta_->cov_msg.clear();
    meta_->cov_sel = 0;
    text_disengage();
    meta_cov_rebuild();
    // start on the picture the track uses now
    const std::string cur = cover_track_choice(path);
    for (size_t i = 0; i < meta_->cov_items.size(); ++i) {
        const auto& it = meta_->cov_items[i];
        if ((cur.empty() && it.special == 1) || (cur == kCoverNone && it.special == 2) || (it.special == 0 && choice_of(it.c) == cur)) { meta_->cov_sel = static_cast<int>(i); break; }
    }
}

void App::meta_cov_rebuild() {
    const fs::path track = path_from_utf8(meta_->cov_track);
    const std::string q = ascii_lower_str(meta_->cov_query);
    meta_->cov_items.clear();
    if (q.empty()) {
        for (int m = 10; m <= 14; ++m) meta_->cov_items.push_back({m, {}});   // what the disk shows of a track with a video (a mode of its own)
        meta_->cov_items.push_back({1, {}});
    }
    std::vector<fs::path> seen;
    auto add = [&](const CoverCandidate& c) {
        if (!q.empty() && ascii_lower_str(c.label).find(q) == std::string::npos) return;
        for (const auto& s : seen) if (s == c.file) return;
        seen.push_back(c.file);
        meta_->cov_items.push_back({0, c});
    };
    for (const auto& c : cover_candidates(track, false)) add(c);
    const std::string pair_song = paired_song_for_video(meta_->cov_track);   // a video: the pictures around its song and the song's fetched covers too
    if (!pair_song.empty()) for (const auto& c : cover_candidates(path_from_utf8(pair_song), false)) add(c);
    if (q.empty() ? has_embedded_picture(track) : (std::string("inside the file").find(q) != std::string::npos && has_embedded_picture(track))) {
        CoverCandidate e;
        e.kind = CoverCandidate::Embedded;
        e.file = track;
        e.label = "inside the file";
        meta_->cov_items.insert(meta_->cov_items.begin() + (q.empty() ? 5 : 0), MetaCovItem{0, e});
    }
    // the covers of the track's album (its fetched covers in other folders, the pictures chosen for it)
    const std::string album = meta_->hover_values[static_cast<size_t>(MetaField::Album)];
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
    // the full path of a picture file typed (or pasted) into the search: that file is a candidate, wherever it lives
    if (!q.empty()) {
        std::string typed = meta_->cov_query;
        while (!typed.empty() && (typed.back() == ' ' || typed.back() == '"')) typed.pop_back();
        while (!typed.empty() && (typed.front() == ' ' || typed.front() == '"')) typed.erase(typed.begin());
        const fs::path tp = path_from_utf8(typed);
        std::error_code ec;
        if (!typed.empty() && tp.is_absolute() && is_picture_name(tp) && fs::is_regular_file(tp, ec)) {
            bool dup = false;
            for (const auto& s : seen) if (s == tp) { dup = true; break; }
            if (!dup) {
                seen.push_back(tp);
                CoverCandidate c;
                c.file = tp;
                c.kind = CoverCandidate::Folder;
                c.label = path_utf8(tp.parent_path().filename()) + "/" + path_utf8(tp.filename());
                meta_->cov_items.push_back({0, c});
            }
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
            meta_->cov_items.push_back({0, c});
            ++count;
        }
    }
    const bool shows_none = q.empty() && cover_track_choice(meta_->cov_track) == kCoverNone;
    if (shows_none) meta_->cov_items.insert(meta_->cov_items.begin() + 5, MetaCovItem{2, {}});   // (listed only while it is the choice: [DEL] on Automatic sets it)
    {   // the picture in use goes to the top of the pictures (right under "Automatic"): the way to the options above it is short
        const std::string use = cover_track_choice(meta_->cov_track);
        const size_t top = q.empty() ? (shows_none ? 6 : 5) : 0;
        if (!use.empty()) {
            for (size_t i = top + 1; i < meta_->cov_items.size(); ++i) {
                const MetaCovItem& it = meta_->cov_items[i];
                if (it.special == 0 && choice_of(it.c) == use) { std::rotate(meta_->cov_items.begin() + static_cast<long>(top), meta_->cov_items.begin() + static_cast<long>(i), meta_->cov_items.begin() + static_cast<long>(i) + 1); break; }
            }
        }
    }
    if (q.empty()) {   // nothing to pick from: say how to get a picture in
        bool any = false;
        for (const MetaCovItem& it : meta_->cov_items) if (it.special == 0) { any = true; break; }
        if (!any) meta_->cov_msg = "no picture found near this track: [/] and type the full path of a picture file (it is listed, ENTER picks it)";
    }
    meta_->cov_sel = std::clamp(meta_->cov_sel, 0, std::max(0, static_cast<int>(meta_->cov_items.size()) - 1));
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
    if (path.empty()) { meta_->status = "no title selected"; return; }
    meta_->sty_track = path;
    meta_->sty_open = true;
    meta_->sty_sel = 0;
    meta_->sty_pair = song_paired_with_video(path_from_utf8(path));
    meta_->sty_msg.clear();
    text_disengage();
}

// The Style Option overlay of the track that plays, straight from the main UI (HKeyStyleOptions): it opens the meta editor on that track
// with the overlay alone on the screen; ESC closes it, [m] goes on to the meta editor on that track.
void App::meta_open_style_playing() {
    if (audiobook_) { status_line_ = "style options: music player only"; return; }
    if (!has_track_ || !current_.is_local || current_.path.empty()) { status_line_ = "style options: play a track from your library first"; return; }
    meta_open();
    const std::string cur = path_utf8(current_.path);
    bool found = false;
    for (size_t i = 0; i < meta_->lib_view.size(); ++i)
        if (path_utf8(meta_->lib_view[i].path) == cur) { meta_->lib_selected = static_cast<int>(i); found = true; break; }
    if (!found) { mode_ = Mode::Browse; status_line_ = "style options: the playing track is not in the library"; return; }
    meta_sty_open();
    if (meta_->sty_open) meta_->sty_from_main = true;
    else mode_ = Mode::Browse;
}

bool App::handle_meta_sty_key(int key) {
    if (!meta_->sty_open) return false;
    const bool arrow = last_key_was_arrow();
    if (key == 27) {
        meta_->sty_open = false;
        if (meta_->sty_from_main) { meta_->sty_from_main = false; mode_ = Mode::Browse; }   // opened with the main UI's key: back there
        return true;
    }
    if (meta_->sty_from_main && !arrow && (key == 'm' || key == 'M')) {   // opened from the main UI: on to the meta data of this very song
        meta_->sty_open = false;
        meta_->sty_from_main = false;
        meta_->focus = 2;   // the fields panel
        meta_->field = 0;
        return true;
    }
    if (arrow && (key == 'A' || key == 'B')) { const int n = static_cast<int>(style_rows(settings_.video_mode, meta_->sty_pair).size()); meta_->sty_sel = (meta_->sty_sel + (key == 'A' ? n - 1 : 1)) % n; return true; }
    const std::string album = meta_->hover_values[static_cast<size_t>(MetaField::Album)];
    const std::string folder = path_utf8(path_from_utf8(meta_->sty_track).parent_path());
    CoverStyle st = cover_track_style(meta_->sty_track);
    int dir = 0;
    bool reset = false;
    if (arrow && key == 'C') dir = 1;
    else if (arrow && key == 'D') dir = -1;
    else if (!arrow && (key == 't' || key == 'T')) {   // picture / braille, as [t] in the cover options
        meta_toggle_cover_res();
        meta_->sty_msg = meta_->status;
        return true;
    }
    else if (!arrow && (key == kKeyDelete || key == 127 || key == 'd' || key == 'r')) reset = true;
    else if (key == '\r' || key == '\n' || key == ' ') dir = 1;
    if (!dir && !reset) return true;
    const std::vector<StyleRow> list = style_rows(settings_.video_mode, meta_->sty_pair);
    const StyleRow sel = list[static_cast<size_t>(std::clamp(meta_->sty_sel, 0, static_cast<int>(list.size()) - 1))];
    switch (sel) {
        case kStLook: st.look = reset ? -1 : sty_step(st.look, dir, kCoverLookCount - 1, 1); break;
        case kStCoverContrast: st.contrast = reset ? -1 : sty_step(st.contrast, dir, 150, 5); break;
        case kStCoverBrightness: st.brightness = reset ? -1 : sty_step(st.brightness, dir, 100, 5); break;
        case kStCoverColors: st.colors = reset ? -1 : sty_step(st.colors, dir, 100, 5); break;
        case kStCoverGradSat: st.gsat = reset ? -1 : sty_step(st.gsat, dir, 100, 5); break;
        case kStCoverChroma: st.chroma = reset ? -1 : sty_step(st.chroma, dir, kChromaLevels - 1, 1); break;
        case kStCoverOrThumb: {   // the mode of the track (the same one as in COVER OPTIONS)
            int mode = cover_track_video_mode(meta_->sty_track);   // 0 as in the settings, 1 cover, 2 thumbnail, 3 plain disk, 4 thumbnail fallback
            mode = reset ? 0 : (mode + (dir > 0 ? 1 : 4)) % 5;
            set_cover_track_video_mode(meta_->sty_track, mode);
            ++video_rev_;
            snap_track_.clear();
            static const char* const names[5] = {"as in the settings", "the cover", "the video thumbnail", "the plain disk", "the cover, else the video thumbnail"};
            meta_->sty_msg = std::string("saved: ") + names[mode];
            return true;
        }
        case kStVideoOn: case kStVideoLyrics: case kStVideoSize: case kStVideoEffect: case kStVideoContrast: case kStVideoBrightness:
        case kStVideoColors: case kStVideoChroma: case kStVideoResolution: case kStVideoRounded: {   // the video of this track (-1 = as in the VIDEO overlay / in Settings)
            VideoTrackOpts o = cover_track_video_opts(meta_->sty_track);
            switch (sel) {
                case kStVideoOn: o.on = reset ? -1 : (o.on == 0 ? -1 : 0); break;
                case kStVideoLyrics: o.lyrics = reset ? -1 : sty_step(o.lyrics, dir, 3, 1); break;   // as in the VIDEO SETTINGS, none, lyrics, lyrics + translation, translation
                case kStVideoSize: o.size = reset ? -1 : (o.size + 1 + (dir > 0 ? 1 : 4)) % 5 - 1; break;   // -1, 0, 1, 2, 3 round
                case kStVideoEffect: o.fx = reset ? -1 : video_fx_step(o.fx, dir, true); break;   // "as in the VIDEO SETTINGS", then the effects in the order of the list
                case kStVideoContrast: o.contrast = reset ? -1 : sty_step(o.contrast, dir, 100, 5); break;
                case kStVideoBrightness: o.brightness = reset ? -1 : sty_step(o.brightness, dir, 100, 5); break;
                case kStVideoColors: o.colors = reset ? -1 : sty_step(o.colors, dir, 100, 5); break;
                case kStVideoChroma: o.chroma = reset ? -1 : sty_step(o.chroma, dir, kChromaLevels - 1, 1); break;
                case kStVideoResolution: o.res = reset ? -1 : sty_step(o.res, dir, 2, 1); break;
                default: o.round = reset ? -1 : sty_step(o.round, dir, 1, 1); break;
            }
            set_cover_track_video_opts(meta_->sty_track, o);
            video_pic_key_.clear();
            if (sel == kStVideoOn || sel == kStVideoResolution) { video_for_track_.clear(); ++video_rev_; force_redraw_ = true; }   // (video on / off, resolution: the video is set up again; a look only makes the picture again)
            else if (sel == kStVideoSize) force_redraw_ = true;   // (the size changes the layout)
            meta_->sty_msg = "saved";
            return true;
        }
        case kStMp4Audio: {   // true / false (false = the song's own audio, as before)
            VideoTrackOpts o = cover_track_video_opts(meta_->sty_track);
            const int before = o.mp4audio;
            o.mp4audio = reset ? -1 : (o.mp4audio == 1 ? -1 : 1);
            set_cover_track_video_opts(meta_->sty_track, o);
            if (o.mp4audio != before && has_track_ && meta_->sty_track == path_utf8(current_.path)) { reload_current_audio(); meta_->sty_msg = "saved: the audio is loaded again, at the same time"; }
            else meta_->sty_msg = "saved: applies when the song starts the next time";
            return true;
        }
        case kStAlbum: {
            const int now = cover_album_look(album, folder);
            set_cover_album_look(album, folder, reset ? -1 : sty_step(now, dir, kCoverLookCount - 1, 1));
            break;
        }
    }
    if (sel <= kStCoverChroma) set_cover_track_style(meta_->sty_track, st);
    meta_->sty_msg = "saved";
    return true;
}

// Wide enough for the preview (the disk is 30 cells wide) when the terminal has room for it.
int App::meta_sty_panel_width(int screen_w) const { return screen_w >= 100 && term_rows_ >= 24 ? kMetaStyPanelWidth + 32 : kMetaStyPanelWidth; }

// The cover of the track as it looks with the style set in the overlay (the plain disk where the look is the disk or the track has no
// cover), prev_w x 15 cells, braille dots like the disk of the main screen. The last row names what is shown.
// The Style preview as a real picture (Kitty / Sixel): like album_cover_gfx(), for any cover and style, at a place of the screen (0-based).
bool App::meta_sty_gfx(const CoverImage& cover, int mode, int contrast, int brightness, int colors, int chroma, double angle, int col, int row, int cols, int rows, int gsat) const {
    if (gfxs_.proto == GfxProto::None || cols < 8 || rows < 4) return false;
    mode = std::clamp(mode, 0, 4);
    if (gsat < 0) gsat = settings_.cover_grad_sat;
    int cw0 = 10, ch0 = 20;
    gfx_cell_pixels(settings_.cell_pixels, cw0, ch0);
    cw0 = std::max(4, cw0); ch0 = std::max(8, ch0);
    const int cws = std::min(cw0, std::max(2, 640 / cols));
    const int chs = std::min(ch0, std::max(4, 640 / rows));
    const int w = cols * cws, h = rows * chs;
    // Over the main screen (its own cover picture is in gfx_) the preview goes into the second slot, so both pictures are shown.
    const bool second = meta_->sty_from_main && mode_ == Mode::MetaEdit && gfx_.active;   // (only in that overlay: the flag can outlive it)
    GfxFrame& g = second ? gfx2_ : gfx_;
    const int want_id = second ? 1 : 0;
    std::string& pic_key = second ? sty2_key_ : cover_pic_key_;
    auto& pic_at = second ? sty2_at_ : cover_pic_at_;
    const std::string key = "sty|" + cover.key + "|" + std::to_string(mode) + "|" + std::to_string(contrast) + "|" + std::to_string(brightness) + "|" + std::to_string(colors) + "|" + std::to_string(gsat) + "|" + std::to_string(chroma) + "|" +
                            std::to_string(w) + "x" + std::to_string(h) + "|" + std::to_string(col) + "," + std::to_string(row) + "|" + settings_.disk_color + "|" +
                            (settings_.disk_color_gradient ? settings_.disk_color_end : std::string());
    const auto now = std::chrono::steady_clock::now();
    const bool resized = key != pic_key || !g.plain || g.w != w || g.h != h || g.level.size() != static_cast<size_t>(w) * h || g.id != want_id || g.col != col || g.cols != cols;
    const auto gap = std::chrono::milliseconds(gfxs_.proto == GfxProto::Sixel ? 110 : 40);
    const bool due = resized || (mode >= 1 && now - pic_at >= gap);
    cover_ovr_ = {true, contrast, brightness, colors, chroma, gsat};   // (cover_fill and cover_sixel_colours read the style through eff_cover_*)
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
    meta_->sty_pic = second ? 2 : 1;
    return true;
}

std::vector<std::string> App::meta_sty_preview(int prev_w, int rows, const CoverStyle& st, int album_look, int screen_w) const {
    std::vector<std::string> out;
    const int look = st.look >= 0 ? st.look : album_look >= 0 ? album_look : (settings_.album_cover ? 1 + std::clamp(settings_.cover_mode, 0, 6) : 0);
    const int contrast = st.contrast >= 0 ? st.contrast : settings_.cover_contrast;
    const int brightness = st.brightness >= 0 ? st.brightness : settings_.cover_brightness;
    const int colors = st.colors >= 0 ? st.colors : settings_.cover_colors;
    const int gsat = st.gsat >= 0 ? st.gsat : settings_.cover_grad_sat;
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
    // On a row of the video block the preview shows the video of the track instead: the same drawing as the video field (video_draw()), so the
    // track's effect, contrast, brightness, colours, chromatic aberration and rounded corners apply, and it plays. The playing track uses the
    // player of the field (in step with the audio; the field is not drawn while this overlay is open); another track uses a decoder of its
    // own that runs while the preview is shown. A track without a video, or with its video off: the disk.
    const std::vector<StyleRow> srows = style_rows(settings_.video_mode, meta_->sty_pair);
    const bool video_rows = settings_.video_mode && style_row_is_video(srows[static_cast<size_t>(std::clamp(meta_->sty_sel, 0, static_cast<int>(srows.size()) - 1))]);
    const fs::path trk = path_from_utf8(meta_->sty_track);
    const std::string vshown = video_rows ? video_shown_for(trk) : std::string();
    bool vdone = false;
    if (!vshown.empty()) {
        const bool is_playing = has_track_ && path_utf8(current_.path) == meta_->sty_track;
        const auto now = std::chrono::steady_clock::now();
        VideoPlayer* vp = nullptr;
        double vt = 0.0;
        if (is_playing) vt = player_.poll_elapsed();
        else {
            if (sty_video_src_ != vshown) { sty_video_.set_source(vshown); sty_video_src_ = vshown; sty_video_t0_ = now; video_pic_key_.clear(); }
            vp = &sty_video_;
            vt = std::chrono::duration<double>(now - sty_video_t0_).count();
        }
        sty_video_seen_ = now;
        const int lpad0 = std::max(0, (prev_w - dw) / 2);
        const int W = meta_sty_panel_width(screen_w), panel_h = rows + 2;
        const int start_col = 1 + std::max(0, (screen_w - W) / 2);
        const int start_row = std::clamp(1 + std::max(0, (term_rows_ - panel_h) / 2 - kFloatingPanelUpShift + 1), 1, std::max(1, term_rows_ - panel_h));
        const int top0 = std::max(0, (rows - 1 - dh) / 2);
        const bool second = meta_->sty_from_main && mode_ == Mode::MetaEdit && gfx_.active;   // (like the cover preview: the main screen's own picture stays in the first slot)
        std::vector<std::string> vr;
        if (video_draw(vr, dw, dh, (start_col - 1) + 1 + (kMetaStyPanelWidth - 2) + 1 + lpad0, start_row + top0, second, vt, 0, std::vector<std::string>(), 0, false, false, vp, &trk) &&
            static_cast<int>(vr.size()) == dh) {
            disk = std::move(vr);
            note = "the video of this track";
            if (gfxs_.proto != GfxProto::None && (second ? gfx2_ : gfx_).active) { meta_->sty_pic = second ? 2 : 1; note += "  (image)"; }
            vdone = true;
        }
    }
    if (vdone) {
        // (the rows are built below like those of the disk)
    } else if (look > 0) {
        CoverRequest rq;
        rq.path = meta_->sty_track;
        rq.title = meta_->hover_values[static_cast<size_t>(MetaField::Title)];   // (like the disk: the tag title, else the file name)
        if (rq.title.empty() || rq.title == "-") rq.title = path_utf8(path_from_utf8(meta_->sty_track).stem());
        rq.album = meta_->hover_values[static_cast<size_t>(MetaField::Album)];
        if (rq.album == "-") rq.album.clear();
        rq.search = settings_.cover_search;
        rq.store_rev = cover_store_revision();
        rq.pair_path = paired_song_for_video(rq.path);
        // what the disk shows of a track with a video: the cover, a frame of the video, or the cover with the frame as fallback (Style Option / VIDEO overlay)
        const int own = cover_track_video_mode(rq.path);
        const bool has_video = !video_source_for(path_from_utf8(meta_->sty_track)).empty();
        const int vm = own == 1 ? 0 : own == 2 ? 1 : own == 3 ? 2 : own == 4 ? 3 : settings_.video_mode ? settings_.video_cover : 0;   // 0 cover, 1 thumbnail, 2 disk, 3 thumbnail fallback
        const bool plain_video = own == 3 || (own == 0 && vm == 2 && has_video);
        const bool no_cover_chosen = cover_track_choice(rq.path) == kCoverNone;
        CoverSource src = resolve_cover_source(rq);
        const bool cover_less = src.none || src.file.empty();
        const bool thumb_mode = has_video && !plain_video && (vm == 1 || (vm == 3 && cover_less && !no_cover_chosen));
        if (thumb_mode) rq.snapshot_video = video_source_for(path_from_utf8(meta_->sty_track));
        const CoverImage* thumb = thumb_mode ? track_cover(rq, true) : nullptr;   // a frame of the video, as the disk shows it
        if (thumb_mode) src = CoverSource();
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
            if (cf_->full_res && m <= 4 && meta_sty_gfx(*img, m, contrast, brightness, colors, chroma, angle, (start_col - 1) + 1 + (kMetaStyPanelWidth - 2) + 1 + lpad0, start_row + top0, dw, dh, gsat)) {
                for (int r = 0; r < dh; ++r) disk.push_back(gfx_hole(dw));
                note += "  (image)";
            } else {
                disk = album_cover_lines_for(*img, m >= 5 ? m - 5 : m, contrast, brightness, colors, angle, dw, dh, chroma, gsat);
                if (disk.empty()) plain_disk();
                else if (gfxs_.proto != GfxProto::None) note += "  (braille)";
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
    const std::string album = meta_->hover_values[static_cast<size_t>(MetaField::Album)];
    const std::string folder = path_utf8(path_from_utf8(meta_->sty_track).parent_path());
    const CoverStyle st = cover_track_style(meta_->sty_track);
    const int album_look = cover_album_look(album, folder);
    auto pct = [&](int own, int global) {
        return own >= 0 ? std::to_string(own) + " %" : std::string("as in Settings (") + std::to_string(global) + " %)";
    };
    auto look_text = [&](int own) { return own >= 0 ? std::string(cover_look_name(own)) : std::string("as in Settings"); };
    struct Row { const char* label; std::string value; };
    const std::vector<StyleRow> list = style_rows(settings_.video_mode, meta_->sty_pair);
    const VideoTrackOpts vo = settings_.video_mode ? cover_track_video_opts(meta_->sty_track) : VideoTrackOpts();
    static const char* const size_names[4] = {"regular", "cropped", "full", "framed"};
    auto row_of = [&](StyleRow id) -> Row {
        switch (id) {
            case kStLook: return {"Look of this track", look_text(st.look)};
            case kStCoverContrast: return {"Cover Contrast", pct(st.contrast, settings_.cover_contrast)};
            case kStCoverBrightness: return {"Cover Brightness", pct(st.brightness, settings_.cover_brightness)};
            case kStCoverColors: return {"Cover Colors", pct(st.colors, settings_.cover_colors)};
            case kStCoverGradSat: return {"Cover Gradient Saturation", pct(st.gsat, settings_.cover_grad_sat)};
            case kStCoverChroma: return {"Cover Chromatic Aberration", st.chroma >= 0 ? std::string(chroma_name(st.chroma)) : std::string("as in Settings (") + chroma_name(settings_.cover_chroma) + ")"};
            case kStCoverOrThumb: {   // the same choice as COVER OPTIONS (this track) and the VIDEO overlay (the default of all tracks)
                const int own = cover_track_video_mode(meta_->sty_track);
                static const char* const dflt[4] = {"the cover", "the video thumbnail", "the plain disk", "the cover, else the thumbnail"};
                static const char* const names[5] = {"", "the cover", "the video thumbnail", "the plain disk", "the cover, else the thumbnail"};
                return {"Use Disk/Cover/Thumbnail", own == 0 ? std::string("as in the settings (") + dflt[std::clamp(settings_.video_cover, 0, 3)] + ")" : std::string(names[own])};
            }
            case kStVideoOn: return {"Video of this track", vo.on == 0 ? std::string("off") : std::string("on")};
            case kStVideoLyrics: return {"Use Lyrics on Video", vo.lyrics >= 0 ? std::string(lyr_mode_name(vo.lyrics)) : std::string("as in VIDEO SETTINGS (") + lyr_mode_name(settings_.video_lyrics) + ")"};
            case kStVideoSize: return {"Video Size", vo.size >= 0 ? std::string(size_names[vo.size]) : std::string("as in VIDEO SETTINGS (") + size_names[std::clamp(settings_.video_size, 0, 3)] + ")"};
            case kStVideoEffect: return {"Video Effect", vo.fx >= 0 ? std::string(video_fx_name(vo.fx)) : std::string("VIDEO SETTINGS (") + video_fx_name(settings_.video_fx) + ")"};
            case kStVideoContrast: return {"Video Contrast", pct(vo.contrast, settings_.video_contrast)};
            case kStVideoBrightness: return {"Video Brightness", pct(vo.brightness, settings_.video_brightness)};
            case kStVideoColors: return {"Video Colors", pct(vo.colors, settings_.video_colors)};
            case kStVideoChroma: return {"Video Chromatic Aberration", vo.chroma >= 0 ? std::string(chroma_name(vo.chroma)) : std::string("as in Settings (") + chroma_name(settings_.video_chroma) + ")"};
            case kStVideoResolution: return {"Video Resolution", vo.res >= 0 ? std::string(video_res_name(vo.res)) : std::string("as in Settings (") + video_res_name(settings_.video_res) + ")"};
            case kStVideoRounded: return {"Video Rounded Corners", vo.round >= 0 ? std::string(vo.round ? "on" : "off") : std::string("as in Settings (") + (settings_.video_round ? "on" : "off") + ")"};
            case kStMp4Audio: return {"Use mp4 audio for this song", vo.mp4audio == 1 ? std::string("true") : std::string("false")};
            case kStAlbum: break;
        }
        return {"Look of the whole album", look_text(album_look)};
    };
    std::vector<Row> rows;
    for (StyleRow id : list) rows.push_back(row_of(id));
    // the left column, one string per row (left_w columns each)
    std::vector<std::string> left;
    auto plain = [&](const std::string& t) { return pad_right(truncate_str(t, left_w), left_w); };
    {   // the note in the header colour (the empty row after it is gone)
        const std::string hc = ansi_for(settings_.header_color, false);
        left.push_back(hc + plain(" Only for this track (last row: its whole album).") + (hc.empty() ? "" : "\x1b[0m"));
    }
    auto head = [&](const char* name) {   // a category line (not a row of the list)
        std::string t = std::string(" \u2500\u2500 ") + name + " ";
        while (display_width(t) < left_w - 1) t += "\u2500";
        left.push_back(border + pad_right(truncate_str(t, left_w - 1), left_w - 1) + (border.empty() ? "" : "\x1b[0m") + " ");
    };
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const StyleRow id = list[static_cast<size_t>(i)];
        if (i == 0) head("Cover");
        else if (id == kStVideoOn) head("Video");
        else if (id == kStAlbum) head("Album");
        std::string t = " " + pad_right(rows[static_cast<size_t>(i)].label, 27) + " \u25c0 " + rows[static_cast<size_t>(i)].value + " \u25b6";
        t = pad_right(truncate_str(t, left_w - 1), left_w - 1) + " ";
        if (i == meta_->sty_sel) t = "\x1b[7m" + t + "\x1b[0m";
        left.push_back(t);
    }
    left.push_back(plain(meta_->sty_msg.empty() ? std::string() : " " + meta_->sty_msg));
    const int body_rows = with_prev ? std::max(15, static_cast<int>(left.size())) : static_cast<int>(left.size());
    while (static_cast<int>(left.size()) < body_rows) left.push_back(plain(""));
    std::vector<std::string> prev;
    if (with_prev) prev = meta_sty_preview(prev_w, body_rows, st, album_look, screen_w);

    std::vector<std::string> lines;
    lines.push_back(box_top("STYLE OPTION  " + truncate_str(apply_font_map(path_utf8(path_from_utf8(meta_->sty_track).filename()), settings_.font_map), 40), W, border));
    for (int r = 0; r < body_rows; ++r) {
        std::string row = obar + left[static_cast<size_t>(r)];
        if (with_prev) row += obar + prev[static_cast<size_t>(r)] + "\x1b[0m";
        lines.push_back(row + obar);
    }
    {   // the legend, centred in the bottom border
        const std::string foot = std::string(" ") + (meta_->sty_from_main ? "[\u2191\u2193] select  [\u2190\u2192] change  [DEL/d] reset  [t] image/braille  [m] meta data  [ESC] close"
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
    if (!meta_->cov_open) return false;
    const bool arrow = last_key_was_arrow();
    const int n = static_cast<int>(meta_->cov_items.size());
    if (meta_->cov_ask_delete) {
        if (key == 'y' || key == 'Y') {
            meta_->cov_ask_delete = false;
            if (meta_->cov_sel >= 0 && meta_->cov_sel < n) {
                std::string why;
                if (delete_cover_candidate(meta_->cov_items[static_cast<size_t>(meta_->cov_sel)].c, &why)) meta_->cov_msg = "picture deleted";
                else meta_->cov_msg = why.empty() ? "cannot be deleted" : why;
                meta_cov_rebuild();
            }
        } else if (key == 'n' || key == 'N' || key == 27) {
            meta_->cov_ask_delete = false;
        }
        return true;
    }
    if (key == 27) {
        if (meta_->cov_search) { meta_->cov_search = false; text_disengage(); return true; }
        if (!meta_->cov_query.empty()) { meta_->cov_query.clear(); edit_caret_ = edit_anchor_ = 0; meta_cov_rebuild(); return true; }
        meta_->cov_open = false;
        return true;
    }
    if (key == 9) {   // TAB: the disk options -> the pictures -> the search field -> ...
        const bool has_modes = n > 0 && meta_->cov_items[0].special >= 10;
        const bool in_modes = !meta_->cov_search && n > 0 && meta_->cov_items[static_cast<size_t>(std::clamp(meta_->cov_sel, 0, n - 1))].special >= 10;
        auto select_picture = [&]() {   // the picture in use (else Automatic, else the first)
            const std::string use = cover_track_choice(meta_->cov_track);
            int at = -1;
            for (int i = 0; i < n && at < 0; ++i) {
                const MetaCovItem& it = meta_->cov_items[static_cast<size_t>(i)];
                if ((it.special == 1 && use.empty()) || (it.special == 0 && !use.empty() && choice_of(it.c) == use)) at = i;
            }
            if (at < 0) for (int i = 0; i < n && at < 0; ++i) if (meta_->cov_items[static_cast<size_t>(i)].special < 10) at = i;
            if (at >= 0) meta_->cov_sel = at;
        };
        if (meta_->cov_search) {   // the search field -> the disk options (when listed), else the pictures
            meta_->cov_search = false;
            text_disengage();
            if (has_modes) meta_->cov_sel = std::clamp(cover_track_video_mode(meta_->cov_track), 0, 3); else select_picture();
        } else if (in_modes) {    // the disk options -> the pictures
            select_picture();
        } else {                  // the pictures -> the search field
            meta_->cov_search = true;
        }
        return true;
    }
    if (arrow && (key == 'A' || key == 'B')) {
        if (n > 0) meta_->cov_sel = std::clamp(meta_->cov_sel + (key == 'A' ? -1 : 1) * (last_key_was_shifted() ? 8 : 1), 0, n - 1);
        return true;
    }
    if (meta_->cov_search) {
        if (key == '\r' || key == '\n') { meta_->cov_search = false; text_disengage(); return true; }
        edit_focus("meta-cov-search", meta_->cov_query);
        text_engage("meta-cov-search");
        if (edit_text_key(meta_->cov_query, edit_caret_, edit_anchor_, key, 80, &meta_->cov_msg)) { meta_->cov_sel = 0; meta_cov_rebuild(); }
        return true;
    }
    if (key == '/') { meta_->cov_search = true; return true; }
    if (key == 't' && !arrow) { meta_toggle_cover_res(); meta_->cov_msg = meta_->status; return true; }   // the same switch as in the meta editor
    if ((key == '\r' || key == '\n') && n > 0) {
        const MetaCovItem& it = meta_->cov_items[static_cast<size_t>(meta_->cov_sel)];
        std::string choice;
        std::string what;
        if (it.special == 1) { choice = ""; what = "automatic: Settings -> Cover Search decides again"; }
        else if (it.special == 2) { choice = kCoverNone; what = "no cover (the disk is shown)"; }
        else { choice = choice_of(it.c); what = apply_font_map(it.c.label, settings_.font_map); }
        if (it.special >= 10) {   // Cover, Disk or Thumbnail of this track: a mode of its own, the picture choice stays as it is
            const int mode = it.special - 10;   // 0 as in the settings, 1 cover, 2 thumbnail, 3 plain disk, 4 thumbnail fallback
            set_cover_track_video_mode(meta_->cov_track, mode);
            ++video_rev_;
            snap_track_.clear();
            meta_->cov_open = false;
            static const char* const names[5] = {"as in the VIDEO SETTINGS", "the cover", "a frame of its video, else the cover", "the plain disk", "the cover, a frame of its video when it has no cover"};
            meta_->status = std::string("this track shows: ") + names[mode] + ((mode == 2 || mode == 4) && video_source_for(path_from_utf8(meta_->cov_track)).empty() ? " (no video yet: the cover is used)" : "");
            return true;
        }
        set_cover_track_choice(meta_->cov_track, choice);
        // the picture chosen for this track weighs most: the track now shows the cover (no thumbnail over it, whatever the settings say); Automatic gives the decision back to the settings
        set_cover_track_video_mode(meta_->cov_track, choice.empty() ? 0 : 1);
        ++video_rev_;
        snap_track_.clear();
        meta_->cov_open = false;
        meta_->status = "cover of this track: " + what;
        return true;
    }
    if ((key == kKeyDelete || key == 127 || key == 'd') && n > 0) {
        const MetaCovItem& it = meta_->cov_items[static_cast<size_t>(meta_->cov_sel)];
        if (it.special == 1 || (it.special == 0 && it.c.kind == CoverCandidate::Embedded)) {   // no picture to take away: this track gets no cover at all
            set_cover_track_choice(meta_->cov_track, kCoverNone);
            set_cover_track_video_mode(meta_->cov_track, 1);
            ++video_rev_;
            snap_track_.clear();
            meta_->cov_msg = "no cover for this track now: the disk is shown (pick Automatic to get a picture back)";
            meta_cov_rebuild();
            for (size_t i = 0; i < meta_->cov_items.size(); ++i) if (meta_->cov_items[i].special == 2) { meta_->cov_sel = static_cast<int>(i); break; }
            return true;
        }
        if (it.special != 0) { meta_->cov_msg = it.special == 2 ? "this track has no cover already" : "nothing to delete here"; return true; }
        if (!cover_deletable(it.c)) { meta_->cov_msg = "this picture cannot be deleted"; return true; }
        meta_->cov_ask_delete = true;
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
    const int n = static_cast<int>(meta_->cov_items.size());
    std::vector<std::string> lines;
    lines.push_back(box_top("COVER OPTIONS  " + truncate_str(apply_font_map(path_utf8(path_from_utf8(meta_->cov_track).filename()), settings_.font_map), 40), W, border));
    {   // search by file name
        std::string field;
        int cols = 0;
        if (meta_->cov_search) {
            EditPaint p = paint_edit_field(meta_->cov_query, edit_caret_, edit_anchor_, 40, search_sgr(settings_), true);
            field = search_run(settings_, p.s, true);
            cols = p.cols;
        } else {
            const std::string plain = truncate_str(meta_->cov_query.empty() ? std::string("(TAB or / : search pictures by file name)") : meta_->cov_query, 60);
            field = search_run(settings_, plain, !meta_->cov_query.empty());
            cols = display_width(plain);
        }
        const std::string pre = " File name: ";
        lines.push_back(obar + search_idle_sgr(settings_) + pre + field + "\x1b[0m" + std::string(static_cast<size_t>(std::max(0, inner - cols - static_cast<int>(pre.size()))), ' ') + obar);
    }
    const std::string cur = cover_track_choice(meta_->cov_track);
    // the preview of the highlighted picture
    std::vector<std::string> prev;
    {
        std::string file, msg;
        bool emb = false;
        std::string vframe;   // a video whose frame is the preview (the thumbnail choices)
        if (n > 0) {
            const MetaCovItem& it = meta_->cov_items[static_cast<size_t>(meta_->cov_sel)];
            if (it.special == 0) { file = path_utf8(it.c.file); emb = it.c.kind == CoverCandidate::Embedded; }
            else if (it.special == 13) msg = "the plain disk";
            else if (it.special == 14 && video_source_for(path_from_utf8(meta_->cov_track)).empty()) msg = "the cover; a frame of the video when there is none";
            else if (it.special == 2) msg = "no cover: the disk is shown";
            else if (it.special == 12 && !video_source_for(path_from_utf8(meta_->cov_track)).empty()) vframe = path_utf8(video_source_for(path_from_utf8(meta_->cov_track)));
            else {   // Automatic (and the disk options that show the cover): the picture the search finds for the track, without its own choice
                CoverRequest rq;
                rq.path = meta_->cov_track;
                rq.title = path_utf8(path_from_utf8(meta_->cov_track).stem());
                rq.album = meta_->hover_values[static_cast<size_t>(MetaField::Album)];
                rq.search = settings_.cover_search;
                rq.store_rev = cover_store_revision();
                rq.pair_path = paired_song_for_video(rq.path);
                rq.ignore_track_choice = true;
                const CoverSource src = resolve_cover_source(rq);
                if (!src.none && !src.file.empty()) { file = path_utf8(src.file); emb = src.embedded; }
                else if (it.special == 14 && !video_source_for(path_from_utf8(meta_->cov_track)).empty()) vframe = path_utf8(video_source_for(path_from_utf8(meta_->cov_track)));
                else msg = it.special == 12 ? "no video yet and no cover found: the disk" : "no cover found: the disk";
            }
        } else msg = "no picture found";
        // [t] (as in the meta editor and the cover fetch): the preview as a real picture (Kitty / Sixel) or as braille dots
        bool drawn = false;
        const CoverImage* vimg = vframe.empty() ? nullptr : preview_video_frame(vframe);
        if (!vframe.empty() && !vimg) msg = preview_video_frame_loading(vframe) ? "reading a frame of the video ..." : "(no frame of the video can be read)";
        if (cf_->full_res && gfxs_.proto != GfxProto::None && (!file.empty() || vimg)) {
            // where the floating panel stands (the same arithmetic as draw_floating_panel()); the preview is right of the list
            const int panel_h = rows + 4;
            const int start_col = 1 + std::max(0, (screen_w - W) / 2);
            const int start_row = std::clamp(1 + std::max(0, (term_rows_ - panel_h) / 2 - kFloatingPanelUpShift), 1, std::max(1, term_rows_ - panel_h));
            const int pcol = (start_col - 1) + 1 + list_w + 1, prow = (start_row - 1) + 2;
            if (vimg ? cover_full_picture_img(vimg, prev_w, rows, pcol, prow) : cover_full_picture(file, emb, prev_w, rows, pcol, prow)) {
                for (int r = 0; r < rows; ++r) prev.push_back(gfx_hole(prev_w));
                drawn = true;
            }
        }
        if (!drawn) prev = vimg ? cover_preview_lines_img(*vimg, prev_w, rows) : cover_preview_lines(file, emb, prev_w, rows, msg);
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
    const int vmode_now = cover_track_video_mode(meta_->cov_track);
    std::vector<Row> top, pics;
    for (int k = 0; k < n; ++k) {
        const MetaCovItem& it = meta_->cov_items[static_cast<size_t>(k)];
        if (it.special >= 10) {
            if (top.empty()) top.push_back(heading("The disk shows"));
            static const char* const labels[5] = {"As in the settings (default)", "The cover", "The video thumbnail, else the cover", "The plain disk", "The cover, else the video thumbnail"};
            top.push_back({k, std::string(it.special - 10 == vmode_now ? "(o) " : "( ) ") + labels[it.special - 10]});
        } else if (it.special == 1) {
            pics.push_back({k, std::string(cur.empty() ? "(o) " : "( ) ") + "Automatic (as in Settings)"});
        } else if (it.special == 2) {
            pics.push_back({k, "(o) No cover: the disk is shown"});
        } else {
            pics.push_back({k, std::string(choice_of(it.c) == cur ? "[x] " : "[ ] ") + apply_font_map(it.c.label, settings_.font_map)});
        }
    }
    const Row pics_head = heading(meta_->cov_query.empty() ? "Picture of this track" : "Pictures matching");
    std::vector<Row> disp = top;
    {
        const int room = std::max(1, rows - static_cast<int>(top.size()) - (pics.empty() ? 0 : 1));   // (the heading of the pictures takes a row)
        int sel_p = 0;
        for (size_t d = 0; d < pics.size(); ++d) if (pics[d].item == meta_->cov_sel) sel_p = static_cast<int>(d);
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
                if (dr.item == meta_->cov_sel) left = "\x1b[7m" + left + R;   // lit while the search field is in use too: [↑↓] pick while typing
            }
        } else {
            left = std::string(static_cast<size_t>(list_w), ' ');
        }
        const std::string right = r < static_cast<int>(prev.size()) ? prev[static_cast<size_t>(r)] : std::string(static_cast<size_t>(prev_w), ' ');
        lines.push_back(obar + left + obar + right + "\x1b[0m" + obar);
    }
    std::string msg = meta_->cov_ask_delete ? "Delete this picture from the disk?   [Y]es   [N]o" : meta_->cov_msg;
    lines.push_back(obar + pad_right(truncate_str(" " + msg, inner), inner) + obar);
    lines.push_back(box_bottom(W, "[↑↓] select  [ENTER] use  [TAB] disk/pictures/search  [t] ASCII/full  [DEL] delete picture / no cover  [ESC] close", border));
    return lines;
}

}  // namespace muisc
