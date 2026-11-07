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
#include "scope_big.h"

#include <algorithm>
#include <chrono>
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
    auto why_no_video = [&]() {   // for the log: why this track has no video view
        if (cover_track_video_opts(path_utf8(current_path_)).on == 0) return std::string("\"Video of this track\" is off (Style Option)");
        if (VideoLinks::get().is_none(path_utf8(current_path_))) return std::string("the connection to its video was removed (Manually Couple Local Files)");
        return std::string("no video file found for it");
    };
    if (audiobook_ || !settings_.video_mode || !settings_.video_auto || !current_is_local_ || current_path_.empty()) return;
    if (!video_shown_for(current_path_).empty()) {
        if (!settings_.video_view) {   // the view you were on (changed with "." / ",") comes back on a track without a video
            auto_prev_view_ = settings_.element_lyrics ? 3 : std::clamp(settings_.lyric_viz, 0, 2); auto_prev_known_ = true;
            auto_prev_set_ = true;
        }
        settings_.element_lyrics = true;
        settings_.video_view = true;
        return;
    }
    log_event("auto video: " + path_utf8(current_path_.filename()) + " has no video: " + why_no_video());
    if (!settings_.video_view) {   // a track without a video on another view: the view you chose stays (and is remembered for "last used")
        auto_prev_view_ = settings_.element_lyrics ? 3 : std::clamp(settings_.lyric_viz, 0, 2); auto_prev_known_ = true;
        return;
    }
    // The video had the field, this track has none. What comes now is decided by the Lyrics/Viz Default of Settings, as it says:
    //   a view (0 .. 3)  -> that view, whatever the field showed before the video
    //   4 last used      -> the view the field was on before the video took it
    //   5 video          -> stays on the video view: "When No Video Shows" (Settings) decides what the field shows
    // (It used to be the view from before the video whatever the default said, and "last used" / "video" always ended on the lyrics.)
    const int dflt = settings_.viz_default;
    auto_prev_set_ = false;
    if (dflt == 5 || (dflt == 4 && !auto_prev_known_)) return;   // (last used, but the field was already on the video when Mousiki started: nothing to go back to, "When No Video Shows" decides)
    settings_.video_view = false;
    const int back = dflt == 4 ? std::clamp(auto_prev_view_, 0, 3) : std::clamp(dflt, 0, 3);
    if (back <= 2) { settings_.element_lyrics = false; settings_.lyric_viz = back; }
    else settings_.element_lyrics = true;
}

void App::video_toggle() {
    settings_.video_mode = !settings_.video_mode;
    if (!settings_.video_mode) {
        settings_.video_view = false;
        video_.stop();
        if (local_sort_mode_ == 4) local_sort_mode_ = 0;
    }
    refresh_local_view();   // the video files join / leave the library list
    if (settings_.video_mode && settings_.video_auto && !audiobook_ && current_is_local_ && !current_path_.empty() && !video_shown_for(current_path_).empty()) {
        if (!settings_.video_view) auto_prev_view_ = settings_.element_lyrics ? 3 : std::clamp(settings_.lyric_viz, 0, 2); auto_prev_known_ = true;
        settings_.element_lyrics = true;   // turned on while a track with a video plays: the field jumps to the video at once
        settings_.video_view = true;
    }
    status_line_ = settings_.video_mode
        ? "video mode: on (video files are listed, " + hotkey_text("HKeyToggleLyrics", ".") + " cycles to the video view, " + hotkey_text("HKeyCycleSortMode", "T") + " shows only the videos)"
        : "video mode: off";
    log_event(status_line_);
}

namespace {

// A string with escape sequences as tokens: an escape sequence (ESC [ ... letter) is one, every other UTF-8 character is one.
std::vector<std::pair<bool, std::string>> ansi_tokens(const std::string& s) {
    std::vector<std::pair<bool, std::string>> out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b') {
            size_t j = i + 1;
            if (j < s.size() && s[j] == '[') { ++j; while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) ++j; if (j < s.size()) ++j; }
            out.emplace_back(true, s.substr(i, j - i));
            i = j;
        } else {
            size_t j = i + 1;
            while (j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) ++j;
            out.emplace_back(false, s.substr(i, j - i));
            i = j;
        }
    }
    return out;
}

// The padding spaces (outside of escape sequences) a rendered lyric row has at its left / right.
void ansi_pad(const std::string& s, int& lead, int& trail) {
    const auto t = ansi_tokens(s);
    lead = trail = 0;
    for (const auto& tk : t) { if (tk.first) continue; if (tk.second == " ") ++lead; else break; }
    for (size_t k = t.size(); k-- > 0;) { if (t[k].first) continue; if (t[k].second == " ") ++trail; else break; }
}

// The row without `lead` spaces at the left and `trail` at the right (escape sequences stay), on a black background: every reset
// of the row's own colours gives the black back.
std::string ansi_black_box_row(const std::string& s, int lead, int trail) {
    auto t = ansi_tokens(s);
    int cut = lead;
    for (auto& tk : t) { if (cut <= 0) break; if (!tk.first && tk.second == " ") { tk.second.clear(); --cut; } }
    cut = trail;
    for (size_t k = t.size(); k-- > 0 && cut > 0;) { if (!t[k].first && t[k].second == " ") { t[k].second.clear(); --cut; } }
    std::string o = "\x1b[48;2;0;0;0m ";   // (true black, not the terminal theme's "black")
    for (const auto& tk : t) {
        o += tk.second;
        if (tk.first && (tk.second == "\x1b[0m" || tk.second == "\x1b[m" || tk.second == "\x1b[49m")) o += "\x1b[48;2;0;0;0m";
    }
    return o + " \x1b[0m";
}

}  // namespace


std::string App::video_shown_for(const fs::path& track) const {
    if (track.empty()) return std::string();
    if (cover_track_video_opts(path_utf8(track)).on == 0) return std::string();   // hidden for this track (Style Option)
    return video_source_for(track);
}

int App::video_size_for(const fs::path& track) const {
    const int own = track.empty() ? -1 : cover_track_video_opts(path_utf8(track)).size;
    return std::clamp(own >= 0 ? own : settings_.video_size, 0, 3);
}

bool App::video_lyrics_for(const fs::path& track) const {
    const int own = track.empty() ? -1 : cover_track_video_opts(path_utf8(track)).lyrics;
    return own >= 0 ? own != 0 : settings_.video_lyrics;
}

bool App::video_round_for(const fs::path& track) const {
    const int own = track.empty() ? -1 : cover_track_video_opts(path_utf8(track)).round;
    return own >= 0 ? own != 0 : settings_.video_round;
}

int App::video_res_for() const {
    const int own = has_track_ && !current_path_.empty() ? cover_track_video_opts(path_utf8(current_path_)).res : -1;
    return std::clamp(own >= 0 ? own : settings_.video_res, 0, 2);
}

int App::video_pic_width() const {
    const int res = video_res_for();
    int w = video_res_width(res, gfx_proto_ == GfxProto::Sixel);
    if (res > 0 && video_.has_video()) w = std::min(w, std::max(640, video_.source_width()));   // (no point in decoding wider than the video itself)
    return w;
}

VideoLook App::video_look_for(const fs::path& track) const {
    const VideoTrackOpts o = track.empty() ? VideoTrackOpts() : cover_track_video_opts(path_utf8(track));
    VideoLook l;
    l.fx = video_fx_canon(std::clamp(o.fx >= 0 ? o.fx : settings_.video_fx, 0, kVideoFxCount - 1));
    l.contrast = o.contrast >= 0 ? o.contrast : settings_.video_contrast;
    l.colors = o.colors >= 0 ? o.colors : settings_.video_colors;
    l.chroma = o.chroma >= 0 ? o.chroma : settings_.video_chroma;
    if (video_fx_uses_gradient(l.fx)) l.grad = video_fx_gradient(settings_);
    return l;
}

int App::video_fx_for(const fs::path& track) const {
    const int own = track.empty() ? -1 : cover_track_video_opts(path_utf8(track)).fx;
    return video_fx_canon(std::clamp(own >= 0 ? own : settings_.video_fx, 0, kVideoFxCount - 1));
}

void App::video_ensure_source() const {
    const std::string cur = path_utf8(current_path_);
    if (cur != video_for_track_) {
        video_for_track_ = cur;
        video_.set_source(video_shown_for(current_path_));
        video_pic_key_.clear();
    }
}

// The current lyric line (wrapped to `width`, coloured, padded) for the video's views; empty when there are no lyrics.
// The saved translation (READ LYRICS) of the lyrics that play: one entry per line of lyrics_result_.lines (the caller holds lyrics_mutex_),
// empty when there is none. The lyrics are read the way READ LYRICS reads them (that is what the translation's fingerprint was made from).
const std::vector<std::string>& App::video_translation_lines() const {
    static const std::vector<std::string> none;
    if (!has_track_ || audiobook_ || lyrics_result_.lines.empty()) return none;
    const std::string key = path_utf8(current_path_) + "|" + settings_.translate_to + "|" + settings_.translate_service + "|" +
                            std::to_string(lyrics_result_.lines.size()) + "|" + (lyrics_result_.lines.front().full_text);
    const auto now = std::chrono::steady_clock::now();
    if (key == vtr_key_ && (!vtr_lines_.empty() || now - vtr_at_ < std::chrono::seconds(3))) return vtr_lines_;
    vtr_key_ = key;
    vtr_at_ = now;
    vtr_lines_.clear();
    std::vector<LyricsVersion> vs = list_lyrics_versions(current_path_);
    std::stable_sort(vs.begin(), vs.end(), [](const LyricsVersion& a, const LyricsVersion& b) { return a.active && !b.active; });
    for (const LyricsVersion& v : vs) {
        double delay = 0.0;
        const std::vector<LyricLine> timed = lyrics_read_lines(read_lyrics_version_text(current_path_, v), &delay);
        if (timed.empty()) continue;
        std::vector<std::string> plain;
        for (const LyricLine& l : timed) plain.push_back(l.full_text);
        const std::vector<TranslationInfo> tl = list_lyrics_translations(current_path_, v, plain);
        if (tl.empty()) continue;
        size_t pick = 0;
        for (size_t i = 0; i < tl.size(); ++i)
            if (tl[i].to == settings_.translate_to && (tl[pick].to != settings_.translate_to || tl[i].service == settings_.translate_service)) pick = i;
        LyricsTranslation t;
        if (!load_lyrics_translation(current_path_, v, tl[pick].to, plain, t, tl[pick].service) || t.same || t.lines.size() != plain.size()) continue;
        // line by line: the same time stamp (or, with equal counts, the same place)
        const auto& cur = lyrics_result_.lines;
        std::vector<std::string> out(cur.size());
        bool any = false;
        for (size_t k = 0; k < cur.size(); ++k) {
            int at = -1;
            if (cur.size() == timed.size()) at = static_cast<int>(k);
            else for (size_t m = 0; m < timed.size(); ++m) if (std::abs(timed[m].start_time - cur[k].start_time) < 0.02 && timed[m].full_text == cur[k].full_text) { at = static_cast<int>(m); break; }
            if (at >= 0) { out[k] = t.lines[static_cast<size_t>(at)]; if (!out[k].empty()) any = true; }
        }
        if (any) { vtr_lines_ = std::move(out); break; }
    }
    return vtr_lines_;
}

std::vector<std::string> App::video_lyric_rows(int width, double elapsed) const {
    std::vector<std::string> rows;
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    if (audiobook_ || !settings_.lyrics_on() || !lyrics_ready_ || lyrics_result_.lines.empty()) return rows;
    std::vector<LyricLine> lines = lyrics_result_.lines;
    latinize_lyrics(lines);
    const double lt = elapsed - lyrics_result_.delay;
    int act = 0;
    for (size_t i = 0; i < lines.size(); ++i) { if (lines[i].start_time <= lt) act = static_cast<int>(i); else break; }
    rows = render_lyric_line_wrapped(lines[static_cast<size_t>(act)], lt, width, true, settings_);
    if (settings_.video_full_translation) {   // the translation (if the lyrics have one) in the lines below
        const std::vector<std::string>& tr = video_translation_lines();
        if (act < static_cast<int>(tr.size()) && !tr[static_cast<size_t>(act)].empty() && tr[static_cast<size_t>(act)] != lyrics_result_.lines[static_cast<size_t>(act)].full_text) {
            LyricLine tl;
            tl.start_time = lines[static_cast<size_t>(act)].start_time;
            tl.full_text = tr[static_cast<size_t>(act)];
            for (const std::string& r : render_lyric_line_wrapped(tl, lt, width, false, settings_)) rows.push_back(r);
        }
    }
    return rows;
}

namespace {
std::vector<unsigned char> clapper_open_mask(int w, int h, int art);   // (below, with the clapperboard art)
}

// Draws the video's picture into a box of W x H cells (rows: H strings of W columns, the picture's cells as placeholders in a
// picture terminal or braille dots). mode 0: the whole picture, centred | 1: the same with two columns cut off each side | 2: fills
// the box, cropped to fit. `ov`: lyric rows (width ov_w) as a small black box over the bottom of the box (cut out of the picture).
// abs_col / abs_row: where the box starts on the screen. false: no video.
bool App::video_draw(std::vector<std::string>& rows, int W, int H, int abs_col, int abs_row, bool second_slot, double t, int mode,
                     const std::vector<std::string>& ov, int ov_w, bool flush_bottom, bool trim_right) const {
    video_ensure_source();
    rows.assign(static_cast<size_t>(std::max(0, H)), std::string(static_cast<size_t>(std::max(0, W)), ' '));
    if (!video_.has_video() || W < 4 || H < 2) return false;
    int cw0 = 10, ch0 = 20;
    gfx_cell_pixels(settings_.cell_pixels, cw0, ch0);
    cw0 = std::max(4, cw0); ch0 = std::max(8, ch0);
    const bool paused = player_.is_paused();
    const bool picture = gfx_proto_ != GfxProto::None;
    // --- geometry: dw x dh = what the decoder delivers; the visible part (outw x outh pixels, vcols x vrows cells) is cut from it
    int vcols = 0, vrows = 0, dw = 0, dh = 0, outw = 0, outh = 0, cx0 = 0, cy0 = 0;
    {
        int fw = 0, fh = 0;
        if (mode == 2) {
            vcols = W; vrows = H;
            if (picture) {
                const int maxw = video_pic_width();
                const double f = std::min(1.0, static_cast<double>(maxw) / (vcols * cw0));
                outw = std::max(2, static_cast<int>(vcols * cw0 * f) & ~1);
                outh = std::max(2, static_cast<int>(vrows * ch0 * f) & ~1);
                video_.fit_cover(outw, outh, 1.0, dw, dh);
            } else {
                outw = vcols * 2; outh = vrows * 4;
                video_.fit_cover(outw, outh, 2.0 * cw0 / ch0, dw, dh);
            }
            cx0 = (dw - outw) / 2; cy0 = (dh - outh) / 2;
        } else {
            if (picture) {
                video_.fit(W * cw0, H * ch0, 1.0, fw, fh);
                vcols = std::clamp((fw + cw0 / 2) / cw0, 1, W);
                vrows = std::clamp((fh + ch0 / 2) / ch0, 1, H);
                const int maxw = video_pic_width();
                const double f = std::min(1.0, static_cast<double>(maxw) / (vcols * cw0));
                dw = std::max(2, static_cast<int>(vcols * cw0 * f) & ~1);
                dh = std::max(2, static_cast<int>(vrows * ch0 * f) & ~1);
            } else {
                video_.fit(W * 2, H * 4, 2.0 * cw0 / ch0, fw, fh);   // a braille dot is cw0 / 2 wide and ch0 / 4 high
                vcols = std::clamp((fw + 1) / 2, 1, W);
                vrows = std::clamp((fh + 3) / 4, 1, H);
                dw = vcols * 2; dh = vrows * 4;
            }
            outw = dw; outh = dh;
            if (mode == 1 && vcols > 8) {   // two columns off each side
                const int px = static_cast<int>(std::lround(static_cast<double>(dw) * 2.0 / vcols));
                cx0 = px; outw = dw - 2 * px;
                vcols -= 4;
            }
        }
    }
    // A video shorter than the track plays again from its start for the rest of the time; a longer one is simply cut off with the track.
    const double vdur = video_.duration();
    if (vdur > 1.0 && t >= vdur) t = std::fmod(t, vdur);
    bool fresh = false;
    video_.set_buffer_mb(settings_.video_buffer);
    video_.set_sharp(video_res_for() > 0);
    // While the audio of a new track is still being handed to the device (launch_device_play_async()), the playback time is still the one of
    // the track before: a decoder started now would begin at that time and show a frame from the middle of the new video (or of the one
    // before, when [n] is hit fast), then restart at 0. So nothing is started until the new audio clock is in.
    const bool handoff = device_play_pending_gen_.load() != 0;
    const VideoFrame* frame = handoff ? nullptr : video_.frame_for(t, dw, dh, !paused, &fresh);
    VideoFrame scaled;   // (the decoder may still deliver the old size for a moment after a resize or a seek: scale what there is)
    if (frame && (frame->w != dw || frame->h != dh)) { scaled = VideoPlayer::resample(*frame, dw, dh); frame = &scaled; }
    const int lead = (W - vcols) / 2, top = (H - vrows) / 2;
    // --- the lyric box over the picture
    int oh = 0, orect_c = 0, orect_r = 0, orect_w = 0;
    std::vector<std::string> otext;
    if (!ov.empty() && ov_w > 0) {
        // The box is measured on the line with ALL its words shown (video_ov_ref_, the same wrap): a word that is not sung yet is blank in
        // `ov`, and measuring that made the box grow word by word -- every size change sends the picture again (a flicker).
        const bool ref_ok = video_ov_ref_.size() == ov.size();
        auto filled = [](const std::string& r) { return display_width(r) > 0 && r.find_first_not_of(' ') != std::string::npos; };
        std::vector<std::string> use, geom;
        for (size_t k = 0; k < ov.size(); ++k) {
            const std::string& g = ref_ok ? video_ov_ref_[k] : ov[k];
            if (filled(g)) { geom.push_back(g); use.push_back(ov[k]); }
        }
        if (use.size() > 2) { use.resize(2); geom.resize(2); }
        if (!use.empty()) {
            int lmin = ov_w, tmin = ov_w;
            for (const auto& r : geom) { int l = 0, tr = 0; ansi_pad(r, l, tr); lmin = std::min(lmin, l); tmin = std::min(tmin, tr); }
            oh = static_cast<int>(use.size());
            for (const auto& r : use) otext.push_back(ansi_black_box_row(r, lmin, tmin));
            orect_w = std::max(1, ov_w - lmin - tmin) + 2;
            if (orect_w > W) { oh = 0; otext.clear(); }
            else { orect_c = (W - orect_w) / 2; orect_r = std::max(0, H - oh - (H >= 8 && !flush_bottom ? 1 : 0)); }
        }
    }
    // the cells [a, b) of row r outside the lyric box: the picture's placeholders where the picture is, spaces elsewhere
    auto blank_cells = [&](int r, int a, int b) {
        std::string o;
        if (b <= a) return o;
        const bool in_rows = r >= top && r < top + vrows;
        const int p0 = std::clamp(lead, a, b), p1 = std::clamp(lead + vcols, a, b);
        o += std::string(static_cast<size_t>(p0 - a), ' ');
        o += in_rows ? gfx_hole(p1 - p0) : std::string(static_cast<size_t>(p1 - p0), ' ');
        o += std::string(static_cast<size_t>(b - p1), ' ');
        return o;
    };
    auto in_box_rows = [&](int r) { return oh > 0 && r >= orect_r && r < orect_r + oh; };
    const VideoLook look = video_look_for(current_path_);
    const int fx = look.fx;
    const bool round_corners = video_round_for(current_path_);
    // the interlace look combs against the frame before: cut and scaled like the frame itself
    VideoFrame prev_shown;
    const VideoFrame* prevp = nullptr;
    if (fx == 17 && frame) {
        if (const VideoFrame* pf = video_.previous_frame()) {
            prev_shown = (pf->w != dw || pf->h != dh) ? VideoPlayer::resample(*pf, dw, dh) : *pf;
            if (cx0 != 0 || cy0 != 0 || outw != dw || outh != dh) prev_shown = VideoPlayer::crop(prev_shown, cx0, cy0, outw, outh);
            prevp = &prev_shown;
        }
    }
    if (frame && picture) {
        GfxFrame& g = second_slot ? gfx2_ : gfx_;
        // the lyric box cuts a hole into the picture: its cells, relative to the picture
        int cut_c = 0, cut_r = 0, cut_w = 0, cut_h = 0;
        if (oh > 0) {
            const int c0 = std::max(orect_c, lead), c1 = std::min(orect_c + orect_w, lead + vcols);
            const int r0 = std::max(orect_r, top), r1 = std::min(orect_r + oh, top + vrows);
            if (c0 < c1 && r0 < r1) { cut_c = c0 - lead; cut_r = r0 - top; cut_w = c1 - c0; cut_h = r1 - r0; }
        }
        const int trim = trim_right ? std::max(1, outw / std::max(1, vcols) / 2) : 0;   // (half a cell of the picture is left out at the right: see video_field_rows)
        const std::string key = std::to_string(dw) + "x" + std::to_string(dh) + "|" + std::to_string(outw) + "x" + std::to_string(outh) + "|" + std::to_string(vcols) + "x" + std::to_string(vrows) + "|" +
                                std::to_string(abs_col + lead) + "|" + std::to_string(cx0) + "," + std::to_string(cy0) + "|" + std::to_string(cut_c) + "," + std::to_string(cut_r) + "," + std::to_string(cut_w) + "," + std::to_string(cut_h) + "|" + look.sig() + "|" + std::to_string(settings_.video_res) + "|" + (round_corners ? "r" : "-") + std::to_string(trim);
        const bool resized = key != video_pic_key_ || g.w != outw || g.h != outh || g.level.size() != static_cast<size_t>(outw) * outh;
        const bool tc = gfx_proto_ == GfxProto::Kitty && video_res_for() > 0;   // true colour (high / full resolution on Kitty)
        g.plain = true; g.direct = true; g.scale = 1; g.truecolor = tc;
        g.out_cw = cw0; g.out_ch = ch0;
        g.cols = vcols; g.rows = vrows; g.w = outw; g.h = outh;
        g.col = abs_col + lead; g.row = abs_row + top;
        g.id = second_slot ? 1 : 0; g.grid.clear(); g.crop = 0;
        g.cut_c = cut_c; g.cut_r = cut_r; g.cut_w = cut_w; g.cut_h = cut_h; g.cut2_w = 0;
        g.opaque = settings_.black_background;
        g.erase = !settings_.black_background;
        g.active = true;
        g.fresh = fresh || resized;
        if (g.fresh) {
            VideoFrame shown;
            const VideoFrame* src = frame;
            if (cx0 != 0 || cy0 != 0 || outw != dw || outh != dh) { shown = VideoPlayer::crop(*frame, cx0, cy0, outw, outh); src = &shown; }
            VideoFrame fxf;
            if (look.active()) { fxf = *src; VideoPlayer::apply_look(fxf, look, t, false, prevp); src = &fxf; }
            if (tc) { g.rgb = src->rgb; g.level.assign(static_cast<size_t>(g.w) * g.h, 255); g.hue.clear(); }
            else VideoPlayer::quantize(*src, g.hue, g.level);
            if (round_corners) {   // the corners cut round, like the corners of the UI: a quarter circle of about one cell row
                const int rp = std::clamp(g.w / std::max(1, vcols) * 11 / 20, 2, std::min(g.w, g.h) / 2);   // (the curve of a terminal corner such as "─╮": about half a cell width, a little more)
                const float rf = static_cast<float>(rp);
                for (int y = 0; y < rp; ++y)
                    for (int x = 0; x < rp; ++x) {
                        const float dx = rf - static_cast<float>(x) - 0.5f, dy = rf - static_cast<float>(y) - 0.5f;
                        if (dx * dx + dy * dy <= rf * rf) continue;
                        const int xs[2] = {x, g.w - trim - 1 - x}, ys[2] = {y, g.h - 1 - y};
                        for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b) g.level[static_cast<size_t>(ys[b]) * g.w + xs[a]] = 0;
                    }
            }
            if (trim > 0)
                for (int y = 0; y < g.h; ++y)
                    for (int x = g.w - trim; x < g.w; ++x) g.level[static_cast<size_t>(y) * g.w + x] = 0;
            if (cut_w > 0) {
                const int xa = cut_c * g.w / std::max(1, vcols), xb = (cut_c + cut_w) * g.w / std::max(1, vcols);
                const int ya = cut_r * g.h / std::max(1, vrows), yb = (cut_r + cut_h) * g.h / std::max(1, vrows);
                for (int y = ya; y < yb && y < g.h; ++y)
                    for (int x = xa; x < xb && x < g.w; ++x) g.level[static_cast<size_t>(y) * g.w + x] = 0;
            }
            if (!tc) { const auto& pal = VideoPlayer::palette(); for (size_t i = 0; i < pal.size() && i < g.pal.size(); ++i) g.pal[i] = pal[i]; }
            video_pic_key_ = key;
        }
        for (int r = 0; r < H; ++r) {
            if (in_box_rows(r)) rows[static_cast<size_t>(r)] = blank_cells(r, 0, orect_c) + otext[static_cast<size_t>(r - orect_r)] + blank_cells(r, orect_c + orect_w, W);
            else rows[static_cast<size_t>(r)] = blank_cells(r, 0, W);
        }
    } else if (frame) {
        VideoFrame shown;
        const VideoFrame* src = frame;
        if (cx0 != 0 || cy0 != 0 || outw != dw || outh != dh) { shown = VideoPlayer::crop(*frame, cx0, cy0, outw, outh); src = &shown; }
        VideoFrame fxf;
        if (look.active()) { fxf = *src; VideoPlayer::apply_look(fxf, look, t, true, prevp); src = &fxf; }
        const auto cells = VideoPlayer::braille(*src, vcols, vrows);
        auto dots = [&](int r, int c0, int c1) {
            std::string line, last_ansi;
            if (r < top || r >= top + vrows) return std::string(static_cast<size_t>(std::max(0, c1 - c0)), ' ');
            for (int c = c0; c < c1; ++c) {
                const int pc = c - lead;
                if (pc < 0 || pc >= vcols) { line += ' '; continue; }
                const auto& cell = cells[static_cast<size_t>(r - top)][static_cast<size_t>(pc)];
                if (cell.mask == 0 || (round_corners && vrows >= 3 && vcols >= 4 && (r - top == 0 || r - top == vrows - 1) && (pc == 0 || pc == vcols - 1))) { line += ' '; continue; }   // (rounded: the four corner cells stay empty)
                const float tt = vcols > 1 ? static_cast<float>(pc) / static_cast<float>(vcols - 1) : 0.0f;
                const float lvl = std::round(cell.level / 255.0f * 7.0f) / 7.0f;
                const std::string ansi = osci_dim_ansi(gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, tt), 0.30f + 0.70f * lvl);
                if (ansi != last_ansi) { line += ansi; last_ansi = ansi; }
                const int cp = 0x2800 + cell.mask;
                line += static_cast<char>(0xE0 | (cp >> 12));
                line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                line += static_cast<char>(0x80 | (cp & 0x3F));
            }
            return line + "\x1b[0m";
        };
        for (int r = 0; r < H; ++r) {
            if (in_box_rows(r)) rows[static_cast<size_t>(r)] = dots(r, 0, orect_c) + otext[static_cast<size_t>(r - orect_r)] + dots(r, orect_c + orect_w, W);
            else rows[static_cast<size_t>(r)] = dots(r, 0, W);
        }
    }
    if (!frame && settings_.video_clap && video_.status() == "video: starting ..." && H >= 4 && W >= 12) {
        // The video is loading: a small open clapperboard in the middle of the picture's place instead of the message (a real picture,
        // transparent round the board, where the terminal has one; else braille dots), in the colours of the border gradient.
        const int mrows = H >= 6 ? 4 : 3;
        int rc0 = 255, gc0 = 255, bc0 = 255, rc1 = 255, gc1 = 255, bc1 = 255;
        if (!try_parse_rgb(settings_.border_color, rc0, gc0, bc0)) { rc0 = gc0 = bc0 = 255; }
        rc1 = rc0; gc1 = gc0; bc1 = bc0;
        if (settings_.border_smooth && !try_parse_rgb(settings_.border_color_bottom, rc1, gc1, bc1)) { rc1 = rc0; gc1 = gc0; bc1 = bc0; }
        auto mix = [&](float u, int& r, int& g, int& b) {
            u = std::clamp(u, 0.0f, 1.0f);
            r = static_cast<int>((rc0 + (rc1 - rc0) * u) * 0.6f); g = static_cast<int>((gc0 + (gc1 - gc0) * u) * 0.6f); b = static_cast<int>((bc0 + (bc1 - bc0) * u) * 0.6f);   // (dimmed: it is only a placeholder)
        };
        if (picture) {
            const int S = mrows * ch0;                                        // the board is square: S x S pixels
            const int mcols = std::min(W, (S + cw0 - 1) / cw0);
            const int iw = mcols * cw0, ih = mrows * ch0;
            const int lead = (W - mcols) / 2, top = (H - mrows) / 2;
            const std::string key = "clap|" + std::to_string(iw) + "x" + std::to_string(ih) + "|" + std::to_string(abs_col + lead) + "," + std::to_string(abs_row + top) + "|" +
                                    settings_.border_color + settings_.border_color_bottom + (settings_.border_smooth ? "s" : "");
            GfxFrame& g = second_slot ? gfx2_ : gfx_;
            g.plain = true; g.direct = true; g.scale = 1; g.truecolor = false;
            g.out_cw = cw0; g.out_ch = ch0;
            g.cols = mcols; g.rows = mrows; g.w = iw; g.h = ih;
            g.col = abs_col + lead; g.row = abs_row + top;
            g.id = second_slot ? 1 : 0; g.grid.clear(); g.crop = 0;
            g.cut_c = g.cut_r = g.cut_w = g.cut_h = g.cut2_w = 0;
            g.opaque = settings_.black_background;
            g.erase = !settings_.black_background;
            g.active = true;
            const bool again = key != video_pic_key_ || g.hue.size() != static_cast<size_t>(iw) * ih;
            g.fresh = again;
            if (again) {
                const std::vector<unsigned char> m = clapper_open_mask(iw, ih, S);
                VideoFrame f;
                f.w = iw; f.h = ih; f.rgb.resize(static_cast<size_t>(iw) * ih * 3);
                for (int y = 0; y < ih; ++y) {
                    int r, gg, b;
                    mix(static_cast<float>(y) / static_cast<float>(std::max(1, ih - 1)), r, gg, b);
                    for (int x = 0; x < iw; ++x) {
                        const size_t i = (static_cast<size_t>(y) * iw + x) * 3;
                        f.rgb[i] = static_cast<uint8_t>(r); f.rgb[i + 1] = static_cast<uint8_t>(gg); f.rgb[i + 2] = static_cast<uint8_t>(b);
                    }
                }
                VideoPlayer::quantize(f, g.hue, g.level);
                for (size_t i = 0; i < m.size(); ++i) g.level[i] = m[i];   // (the opacity: 0 round the board)
                const auto& pal = VideoPlayer::palette();
                for (size_t i = 0; i < pal.size() && i < g.pal.size(); ++i) g.pal[i] = pal[i];
                video_pic_key_ = key;
            }
            for (int r = 0; r < H; ++r)
                rows[static_cast<size_t>(r)] = (r >= top && r < top + mrows)
                    ? std::string(static_cast<size_t>(lead), ' ') + gfx_hole(mcols) + std::string(static_cast<size_t>(std::max(0, W - lead - mcols)), ' ')
                    : std::string(static_cast<size_t>(W), ' ');
        } else {
            const int mcols = std::min(W, mrows * 2);                          // braille: 2 x 4 dots per cell, the dots are square
            const int top = (H - mrows) / 2, lead = (W - mcols) / 2;
            const std::vector<unsigned char> m = clapper_open_mask(mcols * 2, mrows * 4, mrows * 4);
            static const int dxs[8] = {0, 0, 0, 1, 1, 1, 0, 1}, dys[8] = {0, 1, 2, 0, 1, 2, 3, 3};
            for (int r = 0; r < mrows; ++r) {
                int cr, cg, cb;
                mix((static_cast<float>(r) + 0.5f) / static_cast<float>(mrows), cr, cg, cb);
                std::string line = std::string(static_cast<size_t>(lead), ' ') + "\x1b[38;2;" + std::to_string(cr) + ";" + std::to_string(cg) + ";" + std::to_string(cb) + "m";
                for (int c = 0; c < mcols; ++c) {
                    int v = 0;
                    for (int k = 0; k < 8; ++k)
                        if (m[static_cast<size_t>(r * 4 + dys[k]) * static_cast<size_t>(mcols * 2) + static_cast<size_t>(c * 2 + dxs[k])] >= 100) v |= 1 << k;
                    if (v == 0) { line += ' '; continue; }
                    const unsigned cp = 0x2800u + static_cast<unsigned>(v);
                    line += static_cast<char>(0xE0 | (cp >> 12));
                    line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    line += static_cast<char>(0x80 | (cp & 0x3F));
                }
                line += "\x1b[0m" + std::string(static_cast<size_t>(std::max(0, W - lead - mcols)), ' ');
                rows[static_cast<size_t>(top + r)] = line;
            }
        }
    } else if (!frame) {   // no picture (yet): say why, in the middle of the picture's place, instead of a mute empty field
        std::string msg = video_.status();
        if (msg.empty()) msg = "video: ...";
        msg = truncate_str(msg, std::max(1, W - 2));
        const int pad = std::max(0, (W - display_width(msg)) / 2);
        rows[static_cast<size_t>(H / 2)] = std::string(static_cast<size_t>(pad), ' ') + msg + std::string(static_cast<size_t>(std::max(0, W - pad - display_width(msg))), ' ');
    }
    return true;
}

// Builds the field's rows for the video view. `line_rows` = the current lyric line, already wrapped, coloured and centred (may be empty).
// false: there is no video for this track (the caller shows the lyrics as usual).
bool App::video_field_rows(std::vector<std::string>& rows, int W, int H, int area_col, bool second_slot, double t,
                           const std::vector<std::string>& line_rows, bool trim_right) const {
    video_ensure_source();
    if (!video_.has_video()) return false;
    const int size = video_size_for(current_path_);
    if (size == 2) {   // the full field: the picture fills the place of the oscilloscope, the lyric line is a small box over it
        if (H < 3 || W < 8) return false;
        const int lw = std::max(12, W * 2 / 3);
        return video_draw(rows, W, H, area_col, 1, second_slot, t, 2, video_lyrics_for(current_path_) ? line_rows : std::vector<std::string>(), lw, false, trim_right);
    }
    if (size == 3) {   // framed: the same, one row and two columns smaller on every side (an empty frame round the picture; a cell is twice as tall as wide, so the frame looks equally wide all round)
        if (H < 5 || W < 12) return false;
        // The lyrics sit on the last two rows of the field: the line in a small box on the picture's last row, a continuation (when the
        // line has to break) on the frame's bottom row below it. (`line_rows` is wrapped to the same width, lw.)
        std::vector<std::string> inner;
        const int lw = std::max(12, (W - 4) * 2 / 3);
        std::vector<std::string> txt;
        if (video_lyrics_for(current_path_)) for (const auto& r : line_rows) if (display_width(r) > 0 && r.find_first_not_of(' ') != std::string::npos) txt.push_back(r);
        std::vector<std::string> in_pic;
        if (!txt.empty()) in_pic.push_back(txt[0]);
        if (!video_draw(inner, W - 4, H - 2, area_col + 2, 2, second_slot, t, 2, in_pic, lw, true)) return false;
        rows.assign(static_cast<size_t>(H), std::string(static_cast<size_t>(W), ' '));
        for (size_t i = 0; i < inner.size() && i + 2 < static_cast<size_t>(H); ++i) rows[i + 1] = "  " + inner[i] + "  ";
        if (txt.size() > 1) {   // (in a black box too, like the first line)
            int l2 = 0, t2 = 0;
            {   // (measured on the fully shown line, like the box in the picture)
                std::vector<std::string> g;
                if (video_ov_ref_.size() == line_rows.size()) for (const auto& r : video_ov_ref_) if (display_width(r) > 0 && r.find_first_not_of(' ') != std::string::npos) g.push_back(r);
                ansi_pad(g.size() > 1 ? g[1] : txt[1], l2, t2);
            }
            const std::string box = ansi_black_box_row(txt[1], l2, t2);
            const int w2 = display_width(strip_csi(box));   // (display_width does not skip escape sequences: counted with them, the row came out far too short and its border jumped to the left)
            const int lead = std::max(0, (W - w2) / 2);
            if (lead + w2 <= W) rows[static_cast<size_t>(H - 1)] = std::string(static_cast<size_t>(lead), ' ') + box + std::string(static_cast<size_t>(W - lead - w2), ' ');
        }
        return true;
    }
    // Two rows under the picture are always reserved (the picture keeps its size from line to line): the lyric line sits on the
    // first one, the second is for the continuation when the line has to break. Longer breaks are cut after the second row.
    const int kReserve = video_lyrics_for(current_path_) ? 2 : 0;   // (Include Lyrics off: the picture gets the whole field)
    const int lrows = std::min<int>(kReserve, static_cast<int>(line_rows.size()));
    const int kPad = video_lyrics_for(current_path_) ? 1 : 0;   // one empty row above the picture while the lyrics are on
    const int vavail = H - kReserve - kPad;
    if (vavail < 3 || W < 8) return false;
    std::vector<std::string> box;
    video_draw(box, W, vavail, area_col, 1 + kPad, second_slot, std::max(0.0, t), size == 1 ? 1 : 0, {}, 0);
    rows.assign(static_cast<size_t>(H), std::string(static_cast<size_t>(W), ' '));
    for (int r = 0; r < vavail && r < static_cast<int>(box.size()); ++r) rows[static_cast<size_t>(kPad + r)] = box[static_cast<size_t>(r)];
    for (int k = 0; k < lrows; ++k) rows[static_cast<size_t>(kPad + vavail + k)] = line_rows[static_cast<size_t>(k)];
    return true;
}

VizPrefs App::layout_osci_view() const {
    VizPrefs p = settings_;
    if (settings_.vl_osci_style >= 0) p.osci_style = settings_.vl_osci_style;
    if (settings_.vl_osci_mode >= 0) p.osci_mode = settings_.vl_osci_mode;
    return p;
}

SpectroSettings App::layout_spectro_view() const {
    SpectroSettings sp = settings_.spectro;
    if (settings_.vl_spec_style >= 0) sp.style = settings_.vl_spec_style;
    if (settings_.vl_spec_view >= 0) sp.view = settings_.vl_spec_view;
    return sp;
}

std::vector<int> App::osci_menu_rows() const {
    if (!video_layout_menu()) return osci_visible_rows(settings_);
    std::vector<int> r = osci_visible_rows(layout_osci_view());
    return r;
}

// HKeyVideoFull: the video over the whole screen (a frame like the spectrogram's full screen): the picture fits the screen, the lyric
// line sits as a small black box over its bottom. Keys: UP / DOWN change the effect, SHIFT+UP / DOWN the layout (video, + oscilloscope,
// + spectrogram, + both; they sit in one column right of the video), TAB the lyrics, ESC closes.
// The "no video for this track" picture: a film clapperboard (braille art, 40 x 20 cells). Its clapper arm opens and
// snaps shut again while the whole board drifts a little around the middle of the video field.
namespace {
const char* const kClapperArt[20] = {
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠠⣴⣶⣶⡄⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠠⣤⣶⣶⣷⣄⠀⠀⠀⠀⠈⠻⣿⣷⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠠⣤⣴⣶⣶⣄⠀⠀⠀⠀⠈⢻⣿⣿⣿⣷⣄⠀⠀⠀⠀⠙⠉⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠠⣤⣤⣶⣶⣄⠀⠀⠀⠀⠈⢿⣿⣿⣿⣷⣄⠀⠀⠀⠀⠙⠋⠉⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⣴⣦⣤⣄⠀⠀⠈⠻⣿⣿⣿⣷⣄⠀⠀⠀⠀⠙⠛⠉⠉⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⢸⣿⣿⣿⣧⡀⠀⠀⠈⠛⠋⠉⠉⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⢸⣿⣿⣿⣿⣿⣆⠀⣠⣤⣤⣤⡤⠀⠀⠀⠀⢠⣤⣤⣤⣤⠀⠀⠀⠀⢀⣤⣤⣤⣤⠀⠀⠀⠀⢀⣤⣤⠀",
    "⠀⢸⣿⣿⣿⣿⣿⣿⣷⣿⣿⣿⡿⠁⠀⠀⠀⣠⣿⣿⣿⡿⠃⠀⠀⠀⢠⣿⣿⣿⡿⠃⠀⠀⠀⢠⣾⣿⣿⠀",
    "⠀⠘⢿⣟⣛⣛⣛⣿⣿⣿⣿⣟⣀⣀⣀⣀⣰⣿⣿⣿⣿⣁⣀⣀⣀⣠⣿⣿⣿⣿⣁⣀⣀⣀⣠⣿⣿⣿⠃⠀",
    "⠀⠀⢸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀",
    "⠀⠀⢸⣿⣧⣭⣧⣥⣼⣽⣧⣾⣿⣿⣿⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀",
    "⠀⠀⢸⣿⣿⣷⣵⣶⣿⣿⣿⣿⣿⣿⣿⢸⣧⣤⣽⣿⣿⣿⣿⣿⣿⣿⡇⣿⣦⣤⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀",
    "⠀⠀⢸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⢸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡇⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀",
    "⠀⠀⢸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⢸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡇⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀",
    "⠀⠀⢸⣿⣿⣯⣯⣽⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⢸⣯⣫⣯⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀",
    "⠀⠀⢸⣿⡏⠉⡍⠉⢩⢩⡍⠍⠩⠉⠉⣭⣭⣭⣭⣭⣭⣭⣭⣭⣭⢸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀",
    "⠀⠀⢸⣿⡟⡛⣛⠛⠛⣛⣛⣛⣛⣛⣛⣛⣛⣛⣛⣛⣛⣛⣛⣛⣛⣘⣛⣛⣛⣛⣛⣛⣛⣛⣛⣻⣿⣿⠀⠀",
    "⠀⠀⢸⣿⡷⠷⠿⠶⠶⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⢿⣿⣿⠀⠀",
    "⠀⠀⢸⣿⣧⣥⣼⣧⣤⣤⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀",
    "⠀⠀⠸⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿⠀⠀",
};

struct ClapperBits {
    static constexpr int W = 40 * 2, H = 20 * 4;
    unsigned char body[H][W] = {}, arm[H][W] = {};
    ClapperBits() {
        for (int r = 0; r < 20; ++r) {
            const std::string line = kClapperArt[r];
            size_t i = 0;
            int c = 0;
            while (i < line.size() && c < 40) {
                const unsigned char b0 = static_cast<unsigned char>(line[i]);
                if (b0 != 0xE2 || i + 2 >= line.size()) { ++i; continue; }
                const int v = ((line[i + 1] & 0x3F) << 6 | (line[i + 2] & 0x3F)) & 0xFF;   // U+2800 + dots
                i += 3;
                static const int dx[8] = {0, 0, 0, 1, 1, 1, 0, 1}, dy[8] = {0, 1, 2, 0, 1, 2, 3, 3};
                for (int k = 0; k < 8; ++k) {
                    if (!(v & (1 << k))) continue;
                    const int x = c * 2 + dx[k], y = r * 4 + dy[k];
                    const bool is_arm = y < 20 || (y < 24 && x >= 16);
                    (is_arm ? arm : body)[y][x] = 1;
                }
                ++c;
            }
        }
    }
};

// The open board (arm up as drawn) scaled into a w x h mask (0..255 opacity, the art centred, `art` pixels square): the loading
// picture of the small video field. Box filter, so the edges stay smooth at any size.
std::vector<unsigned char> clapper_open_mask(int w, int h, int art) {
    static const ClapperBits bits;
    std::vector<unsigned char> m(static_cast<size_t>(std::max(0, w)) * static_cast<size_t>(std::max(0, h)), 0);
    if (w < 1 || h < 1 || art < 1) return m;
    const int ox = (w - art) / 2, oy = (h - art) / 2;
    const double k = static_cast<double>(ClapperBits::W) / art;   // source dots per output pixel (the art is 80 x 80 dots)
    for (int y = 0; y < art; ++y) {
        const int y0 = static_cast<int>(std::floor(y * k)), y1 = std::min(ClapperBits::H, std::max(y0 + 1, static_cast<int>(std::ceil((y + 1) * k))));
        for (int x = 0; x < art; ++x) {
            const int x0 = static_cast<int>(std::floor(x * k)), x1 = std::min(ClapperBits::W, std::max(x0 + 1, static_cast<int>(std::ceil((x + 1) * k))));
            int on = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) { ++n; if (bits.body[yy][xx] || bits.arm[yy][xx]) ++on; }
            const int px = x + ox, py = y + oy;
            if (n > 0 && px >= 0 && px < w && py >= 0 && py < h) m[static_cast<size_t>(py) * static_cast<size_t>(w) + static_cast<size_t>(px)] = static_cast<unsigned char>(on * 255 / n);
        }
    }
    return m;
}

// t: seconds. Returns rows lines of exactly cols cells (an empty vector when the field is too small for the picture).
std::vector<std::string> clapper_frame(int cols, int rows, double t) {
    if (cols < 50 || rows < 26) return {};
    static const ClapperBits bits;
    const double kClosed = 0.21;   // radians the arm turns down until it lies flat on the board (its tilt in the picture)
    const double p = std::fmod(t / 7.0, 1.0);   // one clap takes seven seconds
    double a = 0.0;   // 0 = open as drawn
    auto ease = [](double x) { return x * x * (3.0 - 2.0 * x); };
    if (p >= 0.25 && p < 0.42) a = kClosed * ease((p - 0.25) / 0.17);          // closes slowly
    else if (p >= 0.42 && p < 0.54) a = kClosed;                                // stays shut for a moment
    else if (p >= 0.54 && p < 0.86) a = kClosed * (1.0 - ease((p - 0.54) / 0.32));   // opens again, even slower
    const int W = cols * 2, H = rows * 4;
    std::vector<unsigned char> comp(static_cast<size_t>(W) * static_cast<size_t>(H), 0), img(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
    const double drift_x = 3.0 * std::sin(t * 0.45);
    const double drift_y = 2.0 * std::sin(t * 0.6 + 1.0);
    const int ox = (W - ClapperBits::W) / 2 + static_cast<int>(std::lround(drift_x));
    const int oy = (H - ClapperBits::H) / 2 + static_cast<int>(std::lround(drift_y));
    auto put = [&](int x, int y) {
        x += ox;
        y += oy;
        if (x >= 0 && x < W && y >= 0 && y < H) comp[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] = 1;
    };
    for (int y = 0; y < ClapperBits::H; ++y)
        for (int x = 0; x < ClapperBits::W; ++x)
            if (bits.body[y][x]) put(x, y);
    const double px = 4.0, py = 21.0, ca = std::cos(a), sa = std::sin(a);
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < ClapperBits::W; ++x) {
            const double ddx = x - px, ddy = y - py;
            const int sx = static_cast<int>(std::lround(px + ca * ddx + sa * ddy));
            const int sy = static_cast<int>(std::lround(py - sa * ddx + ca * ddy));
            if (sx >= 0 && sx < ClapperBits::W && sy >= 0 && sy < ClapperBits::H && bits.arm[sy][sx]) put(x, y);
        }
    {   // the whole board swings a little to both sides, like the radio's satellite but smaller and slower (+-7 degrees, 16 s)
        const double ang = 7.0 * std::sin(2.0 * 3.14159265358979 * t / 16.0) * 3.14159265358979 / 180.0;
        const double cr = std::cos(ang), sr = std::sin(ang), cx = W / 2.0, cy = H / 2.0;
        auto at = [&](int xx, int yy) { return (xx < 0 || yy < 0 || xx >= W || yy >= H) ? 0.0 : static_cast<double>(comp[static_cast<size_t>(yy) * static_cast<size_t>(W) + static_cast<size_t>(xx)]); };
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const double qx = cr * (x + 0.5 - cx) + sr * (y + 0.5 - cy) + cx - 0.5, qy = -sr * (x + 0.5 - cx) + cr * (y + 0.5 - cy) + cy - 0.5;
                const int x0 = static_cast<int>(std::floor(qx)), y0 = static_cast<int>(std::floor(qy));
                const double fx = qx - x0, fy = qy - y0;
                const double v = at(x0, y0) * (1 - fx) * (1 - fy) + at(x0 + 1, y0) * fx * (1 - fy) + at(x0, y0 + 1) * (1 - fx) * fy + at(x0 + 1, y0 + 1) * fx * fy;
                if (v >= 0.5) img[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] = 1;
            }
    }
    static const int dxs[8] = {0, 0, 0, 1, 1, 1, 0, 1}, dys[8] = {0, 1, 2, 0, 1, 2, 3, 3};
    std::vector<std::string> out;
    for (int r = 0; r < rows; ++r) {
        std::string line;
        for (int c = 0; c < cols; ++c) {
            int v = 0;
            for (int k = 0; k < 8; ++k)
                if (img[static_cast<size_t>(r * 4 + dys[k]) * static_cast<size_t>(W) + static_cast<size_t>(c * 2 + dxs[k])]) v |= 1 << k;
            if (v == 0) { line += ' '; continue; }
            const unsigned cp = 0x2800u + static_cast<unsigned>(v);
            line += static_cast<char>(0xE0 | (cp >> 12));
            line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            line += static_cast<char>(0x80 | (cp & 0x3F));
        }
        out.push_back(line);
    }
    return out;
}
}  // namespace

std::string App::build_video_full(int W, const char* clear_prefix) const {
    const int H = std::max(6, term_rows_ - 1);
    const int cols = std::max(4, W - 2), rows_n = std::max(2, H - 2);
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string legend = ansi_for(settings_.legend_color, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + "\x1b[0m";
    std::string title = "VIDEO";
    std::string title_time;   // the elapsed and the total time stand next to the track name
    if (has_track_) {
        const TrackMetadata dm = display_metadata();
        title += " - " + (dm.artist.empty() || dm.artist == "-" ? dm.name : dm.artist + " - " + dm.name);
        title_time = "  [ " + fmt_mmss(player_.poll_elapsed()) + " / " + fmt_mmss(static_cast<double>(total_sec_)) + " ]";
    }

    // ---- the layout: what fits (a screen that is too small shows fewer parts) ----
    const int L = std::clamp(settings_.video_layout, 0, 3);
    bool has_o = vl_has_osci(L), has_s = vl_has_spectro(L);
    bool right = false;
    int vid_w = cols, vid_rows = rows_n, side_w = 0, osc_rows = 0, sp_rows = 0;
    {
        // Video on the left, one column of the full height on the right: the oscilloscope on top, the spectrogram below it (or one of
        // them alone). The video two thirds of the width (VVO / VVS), the column one third. Too small a screen: the video alone.
        const int parts = (has_o ? 1 : 0) + (has_s ? 1 : 0);
        if (parts == 2 && rows_n < 12) has_s = false;   // not both: the oscilloscope stays
        if (parts > 0 && (cols < 60 || rows_n < 6)) has_o = has_s = false;
        if (has_o || has_s) {
            right = true;
            side_w = std::max(24, (cols - 1) / 3);
            vid_w = cols - side_w - 1;
            if (has_o && has_s) { osc_rows = rows_n / 2; sp_rows = rows_n - osc_rows; }
            else if (has_o) osc_rows = rows_n;
            else sp_rows = rows_n;
        }
    }
    const bool scales = settings_.vl_scales;
    std::vector<std::string> pic;
    const double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    const int lw = std::max(12, vid_w * 2 / 3);
    const bool lyr = settings_.video_full_lyrics;
    const bool ok = video_draw(pic, vid_w, vid_rows, 1, 1, false, elapsed, 0, lyr ? video_lyric_rows(lw, elapsed) : std::vector<std::string>(), lw);

    // ---- the oscilloscope (one picture: XY or YT) in a box of `width` x `rows` cells whose first cell is at the screen row `row0`, column `col0` ----
    auto draw_osci = [&](int width, int rows, int row0, int col0) {
        std::vector<std::string> osc;
        const VizPrefs ov = layout_osci_view();
        const int kind = ov.osci_mode == 1 ? 1 : 0;
        const int lab = scales ? 7 : 0;
        const int oc = std::max(4, width - lab);
        const bool image = ov.osci_style == 1 && gfx_proto_ != GfxProto::None;
        OscilloscopeVisualizer& sc = kind == 1 ? scope_big_yt_ : scope_big_xy_;
        sc.copy_ring_from(scope_);
        OscilloscopeVisualizer::Params pp = osci_scope_params(settings_, viz_dt_, image ? 1 : 0);
        pp.mode = kind;
        const OsciSet& os = settings_.osci_set[image ? 1 : 0];
        const int pal = os.palette;
        const ScopeMetrics& sm = settings_.scope_metrics;
        const int xdiv = kind == 1 ? sm.yt_time_div : sm.xy_div, ydiv = kind == 1 ? sm.yt_value_div : sm.xy_div;
        const std::string grid_ansi = ansi_for(settings_.scope_grid_color.empty() ? settings_.list_color : settings_.scope_grid_color, false);
        std::vector<std::string> pane;
        if (image) {
            GfxFrame& F = gfx_.active ? gfx2_ : gfx_;   // (the video's picture, when it is one, has the first slot)
            gfx_cell_pixels(settings_.cell_pixels, cell_w_, cell_h_);
            const int cw0 = std::max(4, cell_w_), ch0 = std::max(8, cell_h_);
            const int cwq = std::clamp(osci_detail_cap(settings_.image_detail, true) / oc, 4, cw0);
            const int scale = std::clamp(settings_.image_scale, 1, 3);
            const int cws = std::max(2, std::min(cwq, cw0 / scale));
            const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
            int gr = 128, gg2 = 128, gb = 128;
            try_parse_rgb(settings_.scope_grid_color.empty() ? settings_.list_color : settings_.scope_grid_color, gr, gg2, gb);
            F.id = &F == &gfx_ ? 0 : 1; F.scale = scale; F.out_cw = cw0; F.out_ch = ch0; F.crop = 0; F.cut_w = 0;
            F.cols = oc; F.rows = rows; F.w = oc * cws; F.h = rows * chs;
            F.row = row0;
            F.col = col0 + lab;
            sc.render_image(F.w, F.h, pp, F.level, F.hue);
            if (pal == 0) {
                for (int y = 0; y < F.h; ++y)
                    for (int x = 0; x < F.w; ++x) F.hue[static_cast<size_t>(y) * F.w + x] = static_cast<uint8_t>(x * 255 / std::max(1, F.w - 1));
            }
            for (int h2 = 0; h2 < 256; ++h2) {
                int r = 255, g3 = 255, b = 255;
                osci_palette_rgb(settings_, pal, static_cast<float>(h2) / 255.0f, r, g3, b);
                F.pal[static_cast<size_t>(h2)] = {static_cast<uint8_t>(r), static_cast<uint8_t>(g3), static_cast<uint8_t>(b)};
            }
            if (scales) F.grid = scope_grid_pixels(F.w, F.h, oc, rows, kind, xdiv, ydiv);
            else F.grid.clear();
            F.grid_rgb = {static_cast<uint8_t>(gr), static_cast<uint8_t>(gg2), static_cast<uint8_t>(gb)};
            F.active = true;
            F.fresh = gfx_due_;
            pane.assign(static_cast<size_t>(rows), gfx_hole(oc));
        } else {
            auto color_of = [&](int tx, int cc, uint8_t hue, float k) {
                if (pal == 0) {
                    const float t = cc > 1 ? static_cast<float>(tx) / static_cast<float>(cc - 1) : 0.0f;
                    return osci_dim_ansi(gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t), k);
                }
                int r = 255, gg = 255, b = 255;
                osci_palette_rgb(settings_, pal, hue / 255.0f, r, gg, b);
                return osci_rgb_seq(r, gg, b, k);
            };
            pane = scope_big_pane(sc, pp, oc, rows, scales, kind, grid_ansi, color_of, xdiv, ydiv);
        }
        std::vector<std::string> yl;
        if (scales) yl = scope_ylabels(rows, lab, ydiv);
        for (int r = 0; r < rows; ++r) {
            std::string line = scales ? grid_ansi + yl[static_cast<size_t>(r)] + R : std::string();
            line += r < static_cast<int>(pane.size()) ? pane[static_cast<size_t>(r)] : std::string(static_cast<size_t>(oc), ' ');
            osc.push_back(line);
        }
        return osc;
    };

    // ---- the spectrogram ----
    auto draw_spectro = [&](int width, int rows, int row0, int col0) {
        std::vector<std::string> spl;
        spectro_used_ = true;
        spectro_gradient_from(settings_);
        SpectroAnalyzer& a = spectro();
        layout_spectro_ = layout_spectro_view();
        layout_spectro_on_ = true;
        const SpectroSettings& sp = layout_spectro_;
        a.update(sp);
        const int label_w = scales ? 6 : 0, gap = scales ? 1 : 0;
        const int sc_cols = std::max(4, width - label_w - gap);
        const bool image = sp.style == 1 && gfx_proto_ != GfxProto::None;
        int cw = 10, chh = 20;
        if (image) gfx_cell_pixels(settings_.cell_pixels, cw, chh);
        const int unit = image ? std::max(1, chh) : 4;
        const bool wf = sp.view == 1;
        // the waterfall has no time axis: its scale is the frequencies of the front graph, in a ruler row on top
        const bool ruler = scales && wf && rows >= 4;
        const int prow = ruler ? 1 : 0, brows = rows - prow;
        std::vector<std::string> lab(static_cast<size_t>(rows));
        if (scales && !wf)
            for (const auto& l : spectro_axis(sp, a.rate(), rows * unit, unit)) if (l.first >= 0 && l.first < rows) lab[static_cast<size_t>(l.first)] = l.second;
        std::vector<std::string> body;
        if (image) {
            spectro_area_[0] = row0 + prow;
            spectro_area_[1] = col0 + label_w + gap;
            spectro_area_[2] = sc_cols;
            spectro_area_[3] = brows;
        } else body = a.braille(sp, sc_cols, brows);
        if (ruler) {
            std::string rl(static_cast<size_t>(sc_cols), ' ');
            for (const auto& l : spectro_axis_x(sp, a.rate(), sc_cols * (image ? cw : 2), image ? cw : 2))
                for (size_t k = 0; k < l.second.size() && l.first >= 0 && static_cast<size_t>(l.first) + k < rl.size(); ++k) rl[static_cast<size_t>(l.first) + k] = l.second[k];
            spl.push_back(std::string(static_cast<size_t>(label_w + gap), ' ') + legend + rl + R);
        }
        for (int r = 0; r < brows; ++r) {
            std::string line = scales ? legend + pad_left(lab[static_cast<size_t>(r)], label_w) + R + " " : std::string();
            if (image) line += gfx_hole(sc_cols);
            else line += r < static_cast<int>(body.size()) ? body[static_cast<size_t>(r)] : std::string(static_cast<size_t>(sc_cols), ' ');
            spl.push_back(line);
        }
        return spl;
    };

    std::vector<std::string> osc, spl;
    if (has_o) osc = draw_osci(side_w, osc_rows, 1, 1 + vid_w + 1);
    if (has_s) spl = draw_spectro(side_w, sp_rows, 1 + osc_rows, 1 + vid_w + 1);

    std::vector<std::string> clap;
    const int note_rows = 3;   // under the clapperboard: the note (2 lines) and a free line
    const std::string note[2] = {"No video for this track.",
                                 "Fetch video via [" + hotkey_text("HKeyVideoFetch", "ALT+V") + "] or pair local video via [" + hotkey_text("HKeyMetaEditor", "M") + "]"};
    if (!ok) {
        static const auto t0 = std::chrono::steady_clock::now();
        clap = clapper_frame(vid_w, vid_rows - note_rows, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());   // (moved up to make room for the note)
    }
    auto centred_note = [&](const std::string& msg0) {
        const std::string msg = truncate_str(msg0, vid_w);
        const int pad = std::max(0, (vid_w - display_width(msg)) / 2);
        return std::string(static_cast<size_t>(pad), ' ') + msg + std::string(static_cast<size_t>(std::max(0, vid_w - pad - display_width(msg))), ' ');
    };
    std::ostringstream f;
    f << clear_prefix << "\x1b[?25l";
    f << box_top(truncate_str(title, std::max(8, W - 6 - display_width(title_time))) + title_time, W, border) << "\n";
    std::vector<std::string> side = osc;
    side.insert(side.end(), spl.begin(), spl.end());
    for (int r = 0; r < vid_rows; ++r) {
        std::string line = r < static_cast<int>(pic.size()) ? pic[static_cast<size_t>(r)] : std::string(static_cast<size_t>(vid_w), ' ');
        if (!ok && !clap.empty() && r >= vid_rows - note_rows) {
            line = r - (vid_rows - note_rows) < 2 ? centred_note(note[r - (vid_rows - note_rows)]) : std::string(static_cast<size_t>(vid_w), ' ');
        } else if (!ok && !clap.empty()) {
            const float gt = static_cast<float>(r + 1) / static_cast<float>(std::max(1, vid_rows + 1));   // the colour the border has on this row
            line = (settings_.border_smooth ? gradient_ansi(settings_.border_color, settings_.border_color_bottom, gt, false) : ansi_for(settings_.border_color, false)) +
                   clap[static_cast<size_t>(r)] + "\x1b[0m";
        } else if (!ok && (r == vid_rows / 2 - 1 || r == vid_rows / 2)) {   // (no room for the picture: the note alone)
            line = centred_note(note[r - (vid_rows / 2 - 1)]);
        }
        if (right) line += " " + (r < static_cast<int>(side.size()) ? side[static_cast<size_t>(r)] : std::string(static_cast<size_t>(side_w), ' '));
        f << bar << line << bar << "\n";
    }
    static const char* kLayoutName[4] = {"Video", "Video+Osci", "Video+Spectro", "Video+Osci+Spectro"};
    const std::string fx = std::string("Effect: ") + video_fx_name(video_fx_for(current_path_));
    const std::string keys = "[" + hotkey_text("HKeyVideoFull", "v") + " / ESC] Close";
    const std::string lay = std::string("[SHIFT+UP/DOWN] Layout: ") + kLayoutName[L];
    const std::string grid = "[SHIFT+g] Grid/Scale";
    const std::string opts = std::string("[S] Options") + (vl_has_osci(L) ? "  [" + hotkey_text("HKeyOscMenu", "O") + "] Osci" : "") +
                             (vl_has_spectro(L) ? "  [" + hotkey_text("HKeySpectroMenu", "I") + "] Spectro" : "");
    const std::string tab = "[TAB] Lyrics " + std::string(!lyr ? "off" : settings_.video_full_translation ? "+ translation" : "on");
    const std::string foot = scope_pick_head({"[UP/DOWN] " + fx + "  " + lay + "  " + tab + "  " + opts + "  " + grid + "  " + keys,
                                              "[UP/DOWN] " + fx + "  " + lay + "  " + tab + "  " + grid + "  " + keys,
                                              "[UP/DOWN] " + fx + "  " + lay + "  " + opts + "  " + grid + "  " + keys,
                                              "[↑↓] " + fx + "  [SHIFT+↑↓] Layout  [TAB] Lyrics  " + grid + "  " + keys,
                                              "[↑↓] " + fx + "  [SHIFT+↑↓] Layout  [TAB] Lyrics  [S] Options  " + keys, keys}, W - 6);
    f << centred_bottom(settings_, W, foot, border_bottom) << "\n";
    return clamp_output_rows(f.str(), term_rows_);
}

}  // namespace muisc
