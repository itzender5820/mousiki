// The Video mode of the music player (HKeyVideo): video files in the library, and the "video" view of the Viz / Lyrics field: the
// picture of the track's video (a real picture in Kitty / Sixel terminals, braille dots elsewhere) with the current lyric line under it.
// The sound is the player's as always; the picture only follows its clock (video_player.h).
#include "app.h"
#include "app_internal.h"
#include "console_log.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "video_links.h"
#include "lyrics_translate.h"

#include <algorithm>
#include <cmath>

namespace muisc {

namespace {
std::string lower_stem(const fs::path& p) {
    std::string s = path_utf8(p.stem());
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

// The video file that belongs to a track: the one linked to it by hand; else the track itself when it is a video file; else a video file of the same name in the
// same folder; else any video file of the library with the same name (case does not matter).
std::string App::video_source_for(const fs::path& track) const {
    if (track.empty()) return std::string();
    if (LocalSource::is_video_file(track)) return path_utf8(track);
    if (VideoLinks::get().is_none(path_utf8(track))) return std::string();   // the connection was removed on purpose: no pairing by file name either
    {   // linked by hand ([ Video Option ] in the meta editor, a Search YT download): that one, when the file is there
        const std::string linked = VideoLinks::get().video_for(path_utf8(track));
        std::error_code lec;
        if (!linked.empty() && fs::exists(path_from_utf8(linked), lec)) return linked;
    }
    const std::string stem = lower_stem(track);
    std::error_code ec;
    for (const fs::path& dir : {track.parent_path()}) {
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            if (LocalSource::is_video_file(it->path()) && lower_stem(it->path()) == stem) return path_utf8(it->path());
        }
    }
    for (const auto& t : all_local_tracks_)
        if (LocalSource::is_video_file(t.path) && lower_stem(t.path) == stem) return path_utf8(t.path);
    return std::string();
}

// A video of the library that is paired with a track (by hand, a download, or the same name beside it) has no tags of its own most of
// the time: the lists show the tags of that track for it. Tags the video has itself win. Runs when tags arrived, a pairing changed or
// the library changed; takes the borrowed tags out again when the pairing is gone or the Video mode is off.
void App::borrow_video_tags() {
    const uint64_t ver = row_meta_tags_version_.load(std::memory_order_relaxed);
    if (ver == bv_ver_ && video_rev_ == bv_rev_ && all_local_tracks_.size() == bv_n_ && settings_.video_mode == bv_mode_) return;
    bv_ver_ = ver; bv_rev_ = video_rev_; bv_n_ = all_local_tracks_.size(); bv_mode_ = settings_.video_mode;
    auto lower = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    std::unordered_map<std::string, std::string> by_stem;   // folder + file name without the extension -> the audio track
    std::unordered_map<std::string, std::string> link_of;   // video -> a linked track
    std::vector<std::string> videos;
    if (settings_.video_mode) {
        for (const LocalTrack& t : all_local_tracks_) {
            if (LocalSource::is_video_file(t.path)) videos.push_back(path_utf8(t.path));
            else by_stem[lower(path_utf8(t.path.parent_path())) + "/" + lower(path_utf8(t.path.stem()))] = path_utf8(t.path);
        }
        for (const auto& kv : VideoLinks::get().all()) link_of[kv.second] = kv.first;
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        std::unordered_map<std::string, RowMeta> now;
        for (const std::string& v : videos) {
            std::string tr;
            const auto li = link_of.find(v);
            if (li != link_of.end()) tr = li->second;
            if (tr.empty()) {
                const fs::path vp = path_from_utf8(v);
                const auto si = by_stem.find(lower(path_utf8(vp.parent_path())) + "/" + lower(path_utf8(vp.stem())));
                if (si != by_stem.end()) tr = si->second;
            }
            if (tr.empty() || VideoLinks::get().is_none(tr)) continue;
            const auto vit = row_meta_cache_.find(v);
            const auto tit = row_meta_cache_.find(tr);
            if (vit == row_meta_cache_.end() || tit == row_meta_cache_.end() || !tit->second.tags_resolved) continue;
            const RowMeta& src = tit->second;
            if (src.artist.empty() && src.title.empty() && src.album.empty() && src.year.empty()) continue;
            const auto bi = borrowed_tags_.find(v);
            const bool is_borrowed = bi != borrowed_tags_.end() && bi->second.artist == vit->second.artist && bi->second.title == vit->second.title &&
                                     bi->second.album == vit->second.album && bi->second.year == vit->second.year;
            const RowMeta& own = vit->second;
            if (!is_borrowed && (!own.artist.empty() || !own.title.empty() || !own.album.empty())) continue;   // the video has tags of its own
            RowMeta nm = own;
            nm.artist = src.artist; nm.title = src.title; nm.album = src.album; nm.year = src.year;
            nm.tags_resolved = true;
            if (nm.artist != own.artist || nm.title != own.title || nm.album != own.album || nm.year != own.year || !own.tags_resolved) {
                vit->second = nm;
                changed = true;
            }
            now[v] = nm;
        }
        for (const auto& kv : borrowed_tags_) {   // no longer paired (or the mode is off): the video is read again
            if (now.count(kv.first)) continue;
            const auto vit = row_meta_cache_.find(kv.first);
            if (vit != row_meta_cache_.end() && vit->second.artist == kv.second.artist && vit->second.title == kv.second.title) {
                row_meta_cache_.erase(vit);
                changed = true;
            }
        }
        borrowed_tags_ = std::move(now);
    }
    if (changed) {
        search_idx_.clear();   // (rebuilt on the next search, with the tags the lists show now)
        bv_ver_ = row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
}

// The Format line of the metadata: a video file shows its own type ("MP4"); a track with a paired video (Video mode on) shows both
// ("MP3, MP4"); anything else what the probe found. Cached per track / pairing state.
std::string App::format_text() const {
    auto ext_up = [](const fs::path& p) {
        std::string e = path_utf8(p.extension());
        if (!e.empty() && e[0] == '.') e.erase(0, 1);
        for (auto& c : e) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return e;
    };
    if (current_path_.empty()) return metadata_.format;
    const std::string key = path_utf8(current_path_) + "|" + std::to_string(video_rev_) + (settings_.video_mode ? "1" : "0") + "|" + metadata_.format;
    if (key == fmt_key_) return fmt_val_;
    std::string out = metadata_.format;
    if (LocalSource::is_video_file(current_path_)) {
        const std::string e = ext_up(current_path_);
        if (!e.empty()) out = e;
    } else if (settings_.video_mode) {
        const std::string v = video_source_for(current_path_);
        const std::string ve = v.empty() ? std::string() : ext_up(path_from_utf8(v));
        const std::string te = ext_up(current_path_);
        if (!ve.empty()) out = (te.empty() ? metadata_.format : te) + ", " + ve;
    }
    fmt_key_ = key;
    fmt_val_ = out;
    return out;
}

// Auto Video: at every new track, a track with a video switches the field to the video view; one without it to the Lyrics/Viz Default.
void App::video_auto_apply() {
    if (audiobook_ || !settings_.video_mode || !settings_.video_auto || !current_is_local_ || current_path_.empty()) return;
    if (!video_source_for(current_path_).empty()) {
        if (!settings_.video_view) {   // the view you were on (changed with "." / ",") comes back on a track without a video
            auto_prev_view_ = settings_.element_lyrics ? 3 : std::clamp(settings_.lyric_viz, 0, 2);
            auto_prev_set_ = true;
        }
        settings_.element_lyrics = true;
        settings_.video_view = true;
        return;
    }
    if (!settings_.video_view) return;   // a track without a video on another view: the view you chose stays
    settings_.video_view = false;
    int back = auto_prev_set_ ? auto_prev_view_ : settings_.viz_default;   // 0..3 a view, 4 last used, 5 video (no video here: the lyrics)
    auto_prev_set_ = false;
    if (back <= 2) { settings_.element_lyrics = false; settings_.lyric_viz = back; }
    else settings_.element_lyrics = true;
}

void App::video_toggle() {
    settings_.video_mode = !settings_.video_mode;
    local_source_.set_include_video(settings_.video_mode);
    if (!settings_.video_mode) {
        settings_.video_view = false;
        video_.stop();
        if (local_sort_mode_ == 4) local_sort_mode_ = 0;
    }
    rescan_now();   // video files join / leave the library list
    if (settings_.video_mode && settings_.video_auto && !audiobook_ && current_is_local_ && !current_path_.empty() && !video_source_for(current_path_).empty()) {
        settings_.element_lyrics = true;   // turned on while a track with a video plays: the field jumps to the video at once
        settings_.video_view = true;
    }
    status_line_ = settings_.video_mode
        ? "video mode: on (video files are listed, " + hotkey_text("HKeyToggleLyrics", ".") + " cycles to the video view, " + hotkey_text("HKeyCycleSortMode", "T") + " shows only the videos)"
        : "video mode: off";
    log_event(status_line_);
}

// Builds the field's rows for the video view. `line_rows` = the current lyric line, already wrapped, coloured and centred (may be empty).
// false: there is no video for this track (the caller shows the lyrics as usual).
bool App::video_field_rows(std::vector<std::string>& rows, int W, int H, int area_col, bool second_slot, double t,
                           const std::vector<std::string>& line_rows) const {
    const std::string cur = path_utf8(current_path_);
    if (cur != video_for_track_) {
        video_for_track_ = cur;
        video_.set_source(video_source_for(current_path_));
        video_pic_key_.clear();
    }
    if (!video_.has_video()) return false;
    // Two rows under the picture are always reserved (the picture keeps its size from line to line): the lyric line sits on the
    // first one, the second is for the continuation when the line has to break. Longer breaks are cut after the second row.
    const int kReserve = settings_.video_lyrics ? 2 : 0;   // (Include Lyrics off: the picture gets the whole field)
    const int lrows = std::min<int>(kReserve, static_cast<int>(line_rows.size()));
    const int kPad = settings_.video_lyrics ? 1 : 0;   // one empty row above the picture while the lyrics are on
    const int vavail = H - kReserve - kPad;
    if (vavail < 3 || W < 8) return false;
    int cw0 = 10, ch0 = 20;
    gfx_cell_pixels(settings_.cell_pixels, cw0, ch0);
    cw0 = std::max(4, cw0); ch0 = std::max(8, ch0);
    const bool paused = player_.is_paused();
    const bool picture = gfx_proto_ != GfxProto::None;
    int vcols = 0, vrows = 0;
    bool fresh = false;
    const VideoFrame* frame = nullptr;
    std::vector<std::vector<VideoPlayer::Cell>> cells;
    int dw = 0, dh = 0;
    if (picture) {
        int fw = 0, fh = 0;
        video_.fit(W * cw0, vavail * ch0, 1.0, fw, fh);
        vcols = std::clamp((fw + cw0 / 2) / cw0, 1, W);
        vrows = std::clamp((fh + ch0 / 2) / ch0, 1, vavail);
        const int maxw = gfx_proto_ == GfxProto::Sixel ? 480 : 640;
        const double f = std::min(1.0, static_cast<double>(maxw) / (vcols * cw0));
        dw = std::max(2, static_cast<int>(vcols * cw0 * f) & ~1);
        dh = std::max(2, static_cast<int>(vrows * ch0 * f) & ~1);
    } else {
        int fw = 0, fh = 0;
        video_.fit(W * 2, vavail * 4, 2.0 * cw0 / ch0, fw, fh);   // a braille dot is cw0 / 2 wide and ch0 / 4 high
        vcols = std::clamp((fw + 1) / 2, 1, W);
        vrows = std::clamp((fh + 3) / 4, 1, vavail);
        dw = vcols * 2; dh = vrows * 4;
    }
    // A video shorter than the track plays again from its start for the rest of the time; a longer one is simply cut off with the track.
    const double vdur = video_.duration();
    if (vdur > 1.0 && t >= vdur) t = std::fmod(t, vdur);
    frame = video_.frame_for(t, dw, dh, !paused, &fresh);
    VideoFrame scaled;   // (the decoder may still deliver the old size for a moment after a resize or a seek: scale what there is)
    if (frame && (frame->w != dw || frame->h != dh)) { scaled = VideoPlayer::resample(*frame, dw, dh); frame = &scaled; }
    const int lead = (W - vcols) / 2, top = kPad + (vavail - vrows) / 2;
    rows.assign(static_cast<size_t>(H), std::string(static_cast<size_t>(W), ' '));
    if (frame && picture) {
        GfxFrame& g = second_slot ? gfx2_ : gfx_;
        const std::string key = std::to_string(dw) + "x" + std::to_string(dh) + "|" + std::to_string(vcols) + "x" + std::to_string(vrows) + "|" + std::to_string(area_col + lead);
        const bool resized = key != video_pic_key_ || g.w != dw || g.h != dh || g.level.size() != static_cast<size_t>(dw) * dh;
        g.plain = true; g.direct = true; g.scale = 1;
        g.out_cw = cw0; g.out_ch = ch0;
        g.cols = vcols; g.rows = vrows; g.w = dw; g.h = dh;
        g.col = area_col + lead; g.row = 1 + top;
        g.id = second_slot ? 1 : 0; g.grid.clear(); g.cut_w = 0; g.crop = 0;
        g.opaque = settings_.black_background;
        g.erase = !settings_.black_background;
        g.active = true;
        g.fresh = fresh || resized;
        if (g.fresh) {
            VideoPlayer::quantize(*frame, g.hue, g.level);
            const auto& pal = VideoPlayer::palette();
            for (size_t i = 0; i < pal.size() && i < g.pal.size(); ++i) g.pal[i] = pal[i];
            video_pic_key_ = key;
        }
        for (int r = 0; r < vrows; ++r)
            rows[static_cast<size_t>(top + r)] = std::string(static_cast<size_t>(lead), ' ') + gfx_hole(vcols) + std::string(static_cast<size_t>(W - lead - vcols), ' ');
    } else if (frame) {
        cells = VideoPlayer::braille(*frame, vcols, vrows);
        for (int r = 0; r < vrows; ++r) {
            std::string line(static_cast<size_t>(lead), ' '), last_ansi;
            for (int c = 0; c < vcols; ++c) {
                const auto& cell = cells[static_cast<size_t>(r)][static_cast<size_t>(c)];
                if (cell.mask == 0) { line += ' '; continue; }
                const float tt = vcols > 1 ? static_cast<float>(c) / static_cast<float>(vcols - 1) : 0.0f;
                const float lvl = std::round(cell.level / 255.0f * 7.0f) / 7.0f;
                const std::string ansi = osci_dim_ansi(gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, tt), 0.30f + 0.70f * lvl);
                if (ansi != last_ansi) { line += ansi; last_ansi = ansi; }
                const int cp = 0x2800 + cell.mask;
                line += static_cast<char>(0xE0 | (cp >> 12));
                line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                line += static_cast<char>(0x80 | (cp & 0x3F));
            }
            line += "\x1b[0m" + std::string(static_cast<size_t>(std::max(0, W - lead - vcols)), ' ');
            rows[static_cast<size_t>(top + r)] = line;
        }
    }
    if (!frame) {   // no picture (yet): say why, in the middle of the picture's place, instead of a mute empty field
        std::string msg = video_.status();
        if (msg.empty()) msg = "video: ...";
        msg = truncate_str(msg, std::max(1, W - 2));
        const int pad = std::max(0, (W - display_width(msg)) / 2);
        rows[static_cast<size_t>(kPad + vavail / 2)] = std::string(static_cast<size_t>(pad), ' ') + msg + std::string(static_cast<size_t>(std::max(0, W - pad - display_width(msg))), ' ');
    }
    for (int k = 0; k < lrows; ++k) rows[static_cast<size_t>(kPad + vavail + k)] = line_rows[static_cast<size_t>(k)];
    return true;
}

}  // namespace muisc
