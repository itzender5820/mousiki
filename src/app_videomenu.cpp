// app_videomenu.cpp
// Part of class App (src/app.h): the VIDEO overlay (HKeyVideo, "&" = SHIFT+6 on a German keyboard) and the VIDEO OPTION overlay in which
// a track and a video file are connected by hand (the [ Video Option ] button of the meta editor, or the VIDEO overlay).
//
// VIDEO (Mode::VideoMenu): a small floating panel -- Video Mode on / off, Include Lyrics, Cover, Disk or Thumbnail, and the doors to
// VIDEO OPTION and SEARCH YT. Every change is written to config.txt at once.
// VIDEO OPTION (Mode::VideoLink): two searchable lists (tracks / video files of the library), the track is locked in with ENTER, the video
// with ENTER; the pair is stored in video_links.txt (video_links.h) and follows the files when they are renamed or moved.
#include "app.h"
#include "app_internal.h"
#include "console_log.h"
#include "overlay_common.h"
#include "path_utf8.h"
#include "string_util.h"
#include "text_edit.h"
#include "terminal_ui.h"
#include "utf8_util.h"
#include "video_links.h"

#include <algorithm>
#include <sstream>

namespace muisc {

// ======================================================================================================================
// VIDEO (the & overlay)
// ======================================================================================================================

void App::video_menu_open() {
    if (audiobook_) { status_line_ = "video: not available in the audio book player"; return; }
    vmenu_sel_ = std::clamp(vmenu_sel_, 0, 6);
    mode_ = Mode::VideoMenu;
    force_redraw_ = true;
}

void App::video_menu_close() {
    mode_ = Mode::Browse;
    force_redraw_ = true;
}

void App::video_menu_change(int row, int dir) {
    switch (row) {
        case 0:
            video_toggle();
            break;
        case 1:
            settings_.video_lyrics = !settings_.video_lyrics;
            set_config_value("VideoLyrics", settings_.video_lyrics ? "true" : "false");
            status_line_ = settings_.video_lyrics ? "video: the current lyric line is shown under the picture" : "video: no lyric line, the picture gets the whole field";
            break;
        case 2:
            settings_.video_auto = !settings_.video_auto;
            set_config_value("VideoAuto", settings_.video_auto ? "true" : "false");
            status_line_ = settings_.video_auto ? "video: a track with a video shows it by itself, a track without one the Lyrics/Viz Default" : "video: auto video off";
            if (settings_.video_auto) video_auto_apply();
            force_redraw_ = true;
            break;
        case 3: {
            settings_.video_cover = (settings_.video_cover + (dir < 0 ? 2 : 1)) % 3;   // cover -> thumbnail -> disk
            set_config_value("VideoCover", settings_.video_cover == 1 ? "thumbnail" : settings_.video_cover == 2 ? "disk" : "cover");
            std::string note;
            if (settings_.video_cover == 1 && !settings_.album_cover) {   // the picture behind the disk is the Album Cover look
                settings_.album_cover = true;
                set_config_value("AlbumCover", "true");
                note = " (Album Cover turned on)";
            }
            if (settings_.video_cover != 0 && !settings_.video_mode) note += " -- needs the Video mode";
            status_line_ = settings_.video_cover == 1 ? "disk: a frame of the track's video (the thumbnail), else its cover, else the disk" + note
                         : settings_.video_cover == 2 ? "disk: the plain disk for a track with a video -- no cover, no thumbnail" + note
                                                      : "disk: the track's cover (else the disk)";
            ++video_rev_;
            snap_track_.clear();
            force_redraw_ = true;
            break;
        }
        default: break;
    }
}

void App::video_menu_key(int key) {
    const bool arrow = last_key_was_arrow();
    constexpr int kRows = 7;
    if (arrow && key == 'A') { vmenu_sel_ = (vmenu_sel_ + kRows - 1) % kRows; return; }
    if (arrow && key == 'B') { vmenu_sel_ = (vmenu_sel_ + 1) % kRows; return; }
    if (arrow && (key == 'C' || key == 'D')) { if (vmenu_sel_ <= 3) video_menu_change(vmenu_sel_, key == 'C' ? 1 : -1); return; }
    if (!arrow && (key == '\r' || key == '\n' || key == ' ')) {
        if (vmenu_sel_ <= 3) { video_menu_change(vmenu_sel_, 1); return; }
        if (vmenu_sel_ == 4) { video_menu_close(); video_link_open(has_track_ ? current_path_ : fs::path(), Mode::Browse); return; }
        if (vmenu_sel_ == 5) { video_menu_close(); video_search_open(); return; }
        if (vmenu_sel_ == 6) { video_menu_close(); video_fetch_open(); return; }
    }
    const std::string act = resolve_hotkey_action(key);
    if (!arrow && (key == 27 || act == "HKeyVideo")) video_menu_close();
}

std::vector<std::string> App::build_video_menu_panel() const {
    const int W = kVideoMenuPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    auto entry = [&](bool cursor, const std::string& name, const std::string& value) {
        const std::string l = std::string(cursor ? "> " : "  ") + name;
        const int gap = std::max(1, inner - display_width(l) - display_width(value));
        return l + std::string(static_cast<size_t>(gap), ' ') + value;
    };
    std::vector<std::string> lines;
    lines.push_back(box_top("VIDEO", W, border));
    lines.push_back(row("", false));
    lines.push_back(row(entry(vmenu_sel_ == 0, "Video Mode", settings_.video_mode ? "[x] on" : "[ ] off"), vmenu_sel_ == 0));
    lines.push_back(row(entry(vmenu_sel_ == 1, "Include Lyrics", settings_.video_lyrics ? "[x] yes" : "[ ] no"), vmenu_sel_ == 1));
    lines.push_back(row(entry(vmenu_sel_ == 2, "Auto Video (else Lyrics/Viz Default)", settings_.video_auto ? "[x] on" : "[ ] off"), vmenu_sel_ == 2));
    lines.push_back(row(entry(vmenu_sel_ == 3, "Cover, Disk or Thumbnail", settings_.video_cover == 1 ? "thumbnail" : settings_.video_cover == 2 ? "disk" : "cover"), vmenu_sel_ == 3));
    lines.push_back(row(entry(vmenu_sel_ == 4, "[ Video Option ]  connect a song and a video", ""), vmenu_sel_ == 4));
    lines.push_back(row(entry(vmenu_sel_ == 5, "[ Search YT ]  find and download a video", ""), vmenu_sel_ == 5));
    lines.push_back(row(entry(vmenu_sel_ == 6, "[ Fetch Videos ]  videos for all songs without one", ""), vmenu_sel_ == 6));
    lines.push_back(row("", false));
    const char* hint = vmenu_sel_ == 0 ? "Video files are listed and the video view joins the \".\" cycle."
                     : vmenu_sel_ == 1 ? "The current lyric line under the picture (two rows are kept free for it)."
                     : vmenu_sel_ == 2 ? "A track that has a video shows it at once; a track without one jumps to the Lyrics/Viz Default of Settings."
                     : vmenu_sel_ == 3 ? "Cover: the track's cover. Thumbnail: a frame of the track's video (else the cover). Disk: the plain disk when the track has a video."
                     : vmenu_sel_ == 4 ? "Choose a video file for a song by hand (they are paired by file name when they are named alike)."
                     : vmenu_sel_ == 5 ? "Search YouTube for a video of the playing song, with a preview, and download it."
                                       : "The overlay that finds and downloads videos for the songs that have none yet (also ALT+v).";
    for (const std::string& l : wrap_lines(hint, inner, 3)) lines.push_back(row(l, false));
    for (size_t pad = wrap_lines(hint, inner, 3).size(); pad < 3; ++pad) lines.push_back(row("", false));
    lines.push_back(row("", false));
    lines.push_back(centred_bottom(settings_, W, "[UP/DOWN] [ENTER] [←→] change  [ESC] close", border_bottom));
    return lines;
}

// ======================================================================================================================
// VIDEO OPTION (connect a song and a video file)
// ======================================================================================================================

int App::video_link_list_rows() const { return std::clamp(term_rows_ - 22, 3, 14); }

static std::string vl_hay(const std::string& label, const std::string& folder) { return ascii_lower_str(label + " " + folder); }

void App::video_link_rebuild() {
    std::vector<std::string> words;
    auto split = [](const std::string& q) {
        std::vector<std::string> w;
        std::istringstream iss(ascii_lower_str(q));
        std::string x;
        while (iss >> x) w.push_back(x);
        return w;
    };
    const std::vector<std::string> tw = split(vl_tq_), vw = split(vl_vq_);
    auto match = [](const std::string& hay, const std::vector<std::string>& w) {
        for (const std::string& x : w) if (hay.find(x) == std::string::npos) return false;
        return true;
    };
    vl_tview_.clear();
    for (size_t i = 0; i < vl_tracks_.size(); ++i) {
        const VLEntry& e = vl_tracks_[i];
        if (vl_filter_ == 1 && e.video) continue;
        if (vl_filter_ == 2 && !e.video) continue;
        if (vl_filter_ == 3 && (e.video || VideoLinks::get().has(path_utf8(e.path)))) continue;
        if (match(e.hay, tw)) vl_tview_.push_back(static_cast<int>(i));
    }
    vl_vview_.clear();
    for (size_t i = 0; i < vl_videos_.size(); ++i) if (match(vl_videos_[i].hay, vw)) vl_vview_.push_back(static_cast<int>(i));
    vl_tsel_ = std::clamp(vl_tsel_, 0, std::max(0, static_cast<int>(vl_tview_.size()) - 1));
    vl_vsel_ = std::clamp(vl_vsel_, 0, std::max(0, static_cast<int>(vl_vview_.size()) - 1));
}

// The video a song has now (a connection by hand or a download, else the one with the same name beside it) goes into the locked-in
// video field and the cursor of the video list stands on it, so the dialog opens showing what is paired at the moment.
void App::video_link_show_current(const fs::path& track) {
    vl_lock_video_.clear();
    if (track.empty() || LocalSource::is_video_file(track)) return;
    const std::string cur = video_source_for(track);
    if (cur.empty()) return;
    vl_lock_video_ = path_from_utf8(cur);
    for (size_t k = 0; k < vl_vview_.size(); ++k)
        if (vl_videos_[static_cast<size_t>(vl_vview_[k])].path == vl_lock_video_) { vl_vsel_ = static_cast<int>(k); break; }
}

void App::video_link_open(const fs::path& target, Mode return_mode) {
    vl_return_mode_ = return_mode;
    vl_tracks_.clear();
    vl_videos_.clear();
    std::vector<LocalTrack> scanned;
    const std::vector<LocalTrack>* src = &all_local_tracks_;
    if (!settings_.video_mode) {   // the library list holds no videos while the Video mode is off: look at the folders once for them
        LocalSource ls;
        ls.set_include_video(true);
        std::vector<std::string> diag;
        scanned = ls.scan(library_roots(), &diag);
        src = &scanned;
    }
    for (const LocalTrack& t : *src) {
        VLEntry e;
        e.path = t.path;
        e.video = LocalSource::is_video_file(t.path);
        const std::string folder = path_utf8(t.path.parent_path().filename());
        e.label = e.video ? path_utf8(t.path.filename()) : ((t.folder_artist.empty() || t.folder_artist == "-") ? t.title : t.folder_artist + " - " + t.title);
        e.folder = folder;
        e.hay = vl_hay(e.label + " " + path_utf8(t.path.filename()), folder);
        vl_tracks_.push_back(e);
        if (e.video) vl_videos_.push_back(e);
    }
    auto by_label = [](const VLEntry& a, const VLEntry& b) { return ascii_lower_str(a.label) < ascii_lower_str(b.label); };
    std::sort(vl_tracks_.begin(), vl_tracks_.end(), by_label);
    std::sort(vl_videos_.begin(), vl_videos_.end(), by_label);
    vl_tq_.clear(); vl_vq_.clear();
    vl_filter_ = 0;
    vl_tsel_ = vl_vsel_ = 0;
    vl_pane_ = 1;                 // the track is locked in already (the playing / hovered one): the video is what is missing
    vl_lock_track_ = target;
    vl_lock_video_.clear();
    vl_confirm_ = false;
    vl_warn_ = false;
    vl_msg_.clear();
    if (target.empty()) { vl_pane_ = 0; vl_msg_ = "pick the song first (ENTER locks it in)"; }
    video_link_rebuild();
    video_link_show_current(target);
    if (!target.empty()) {   // the cursor stands on the locked track
        for (size_t k = 0; k < vl_tview_.size(); ++k) if (vl_tracks_[static_cast<size_t>(vl_tview_[k])].path == target) { vl_tsel_ = static_cast<int>(k); break; }
    }
    mode_ = Mode::VideoLink;
    force_redraw_ = true;
}

void App::video_link_close() {
    mode_ = vl_return_mode_;
    force_redraw_ = true;
}

std::string App::video_link_label(const fs::path& p) const {
    if (p.empty()) return std::string();
    for (const VLEntry& e : vl_tracks_) if (e.path == p) return e.label;
    return path_utf8(p.filename());
}

void App::video_link_do() {
    vl_warn_ = true;
    vl_confirm_ = false;
    if (vl_lock_track_.empty()) { vl_msg_ = "no song locked in -- highlight one and press [ENTER]"; return; }
    if (vl_lock_video_.empty()) { vl_msg_ = "no video locked in -- highlight one and press [ENTER]"; return; }
    if (LocalSource::is_video_file(vl_lock_track_)) { vl_msg_ = "the locked-in song is a video file itself -- it shows its own picture"; return; }
    VideoLinks::get().set(path_utf8(vl_lock_track_), path_utf8(vl_lock_video_));
    ++video_rev_;
    video_for_track_.clear();
    vl_warn_ = false;
    vl_msg_ = "connected: " + video_link_label(vl_lock_track_) + "  <->  " + path_utf8(vl_lock_video_.filename());
    log_event(vl_msg_);
    status_line_ = vl_msg_;
}

void App::video_link_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (key == kKeyDelete) {   // DEL: take the connection of the locked-in song away (the second DEL confirms)
        vl_warn_ = true;
        if (vl_lock_track_.empty() || !VideoLinks::get().has(path_utf8(vl_lock_track_))) { vl_confirm_ = false; vl_msg_ = "the locked-in song has no connected video -- nothing to remove"; return; }
        if (!vl_confirm_) { vl_confirm_ = true; vl_msg_ = "remove the connection of this song? [DEL] again confirms, anything else cancels"; return; }
        VideoLinks::get().remove(path_utf8(vl_lock_track_));
        ++video_rev_;
        video_for_track_.clear();
        vl_confirm_ = false;
        vl_warn_ = false;
        vl_msg_ = "connection removed";
        return;
    }
    vl_confirm_ = false;
    if (key == 27) { video_link_close(); return; }
    if (!arrow && key == '\t') { vl_pane_ = (vl_pane_ + 1) % 4; vl_msg_.clear(); return; }
    if (key == '\r' || key == '\n') {
        vl_warn_ = false;
        if (vl_pane_ == 2) { video_link_do(); return; }
        if (vl_pane_ == 3) { vl_lock_track_.clear(); vl_lock_video_.clear(); vl_msg_ = "choice cleared"; return; }
        if (vl_pane_ == 0) {
            if (vl_tview_.empty()) return;
            const VLEntry& e = vl_tracks_[static_cast<size_t>(vl_tview_[static_cast<size_t>(vl_tsel_)])];
            vl_lock_track_ = e.path;
            video_link_show_current(e.path);
            vl_msg_ = "locked song: " + e.label + (vl_lock_video_.empty() ? std::string() : "  (its video is shown on the right)");
            vl_pane_ = 1;
        } else {
            if (vl_vview_.empty()) return;
            const VLEntry& e = vl_videos_[static_cast<size_t>(vl_vview_[static_cast<size_t>(vl_vsel_)])];
            vl_lock_video_ = e.path;
            vl_msg_ = "locked video: " + e.label;
            if (!vl_lock_track_.empty()) video_link_do();   // both are locked in: the pair is made
        }
        return;
    }
    if (key == kKeyCtrlC) { vl_lock_track_.clear(); vl_lock_video_.clear(); vl_msg_ = "choice cleared"; vl_warn_ = false; return; }
    if (key == kKeyCtrlF) {   // the song list: all files / audio only / videos only / audio without a video
        vl_filter_ = (vl_filter_ + 1) % 4;
        vl_tsel_ = 0;
        static const char* const names[4] = {"all files (audio and video)", "audio files only", "video files only", "audio files that have no video yet"};
        vl_msg_ = std::string("the song list shows: ") + names[vl_filter_];
        vl_warn_ = false;
        video_link_rebuild();
        return;
    }
    if (arrow && key == 'D' && vl_pane_ == 1) { vl_pane_ = 0; return; }
    if (arrow && key == 'C' && vl_pane_ == 0) { vl_pane_ = 1; return; }
    if (vl_pane_ >= 2) return;
    int& sel = vl_pane_ == 0 ? vl_tsel_ : vl_vsel_;
    std::string& q = vl_pane_ == 0 ? vl_tq_ : vl_vq_;
    const int n = vl_pane_ == 0 ? static_cast<int>(vl_tview_.size()) : static_cast<int>(vl_vview_.size());
    const int page = video_link_list_rows();
    if (arrow && key == 'A') { sel = std::max(0, sel - (last_key_was_shifted() ? page : 1)); return; }
    if (arrow && key == 'B') { sel = std::min(std::max(0, n - 1), sel + (last_key_was_shifted() ? page : 1)); return; }
    if (arrow) return;
    if (key == kKeyHome) { sel = 0; return; }
    if (key == kKeyEnd) { sel = std::max(0, n - 1); return; }
    if (key == 127 || key == 8) { pop_utf8_char(q); sel = 0; vl_msg_.clear(); video_link_rebuild(); return; }
    if (is_text_key(key) && q.size() < 300) {
        q += static_cast<char>(key);
        sel = 0;
        vl_msg_.clear();
        video_link_rebuild();
    }
}

std::vector<std::string> App::build_video_link_panel(int W) const {
    const int PW = std::clamp(W - 2, 70, 112);
    const int pw_l = (PW - 3) / 2, pw_r = PW - 3 - pw_l;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const std::string hdr = header_sgr(settings_);
    const std::string leg = legend_sgr(settings_);
    std::vector<std::string> out;
    out.push_back(box_top("VIDEO OPTION", PW, border));
    auto pline = [&](const std::string& content, int used, int w) {
        return bar + " " + content + std::string(static_cast<size_t>(std::max(0, w - 4 - used)), ' ') + " " + bar;
    };
    auto both = [&](const std::string& l, const std::string& r) { out.push_back(bar + l + " " + r + bar); };
    auto cut = [&](const std::string& t, int w, const std::string& sgr = std::string()) {
        const std::string c = truncate_str(t, w - 4);
        return std::make_pair(sgr.empty() ? c : sgr + c + R, display_width(c));
    };
    auto mq_key = [](const std::string& t) { return static_cast<int>(std::hash<std::string>{}(t) & 0x3fffffff); };   // (the marquee starts over when the text changes)
    // a line of a locked-in box: scrolls when too long
    auto mq = [&](int slot, const std::string& t, int w, const std::string& sgr) {
        const std::string c = t.empty() ? std::string(static_cast<size_t>(w - 4), ' ') : marquee_or_truncate(t, w - 4, mq_key(t), vl_marq_[slot], vl_marq_since_[slot]);
        return std::make_pair(sgr.empty() ? c : sgr + c + R, w - 4);
    };
    auto title = [&](int which, const std::string& t) { return t + (vl_pane_ == which ? focus_indicator(settings_, border) : std::string()); };
    auto search_line = [&](int which, const std::string& q, int w) {
        const bool focus = vl_pane_ == which;
        const int room = std::max(1, w - 4 - 1);
        std::string shown = q;
        while (display_width(shown) > room && !shown.empty()) {
            size_t i = 1;
            while (i < shown.size() && (static_cast<unsigned char>(shown[i]) & 0xC0) == 0x80) ++i;
            shown.erase(0, i);
        }
        return pline(search_idle_sgr(settings_) + search_run(settings_, shown, !shown.empty()) + R + (focus ? "\x1b[7m \x1b[0m" : " "), display_width(shown) + 1, w);
    };
    both(box_top(title(0, "SEARCH SONGS"), pw_l, border), box_top(title(1, "SEARCH VIDEOS"), pw_r, border));
    both(search_line(0, vl_tq_, pw_l), search_line(1, vl_vq_, pw_r));
    both(box_bottom(pw_l, "", border), box_bottom(pw_r, "", border));
    const int list_rows = video_link_list_rows();
    const int nt = static_cast<int>(vl_tview_.size());
    const int nv = static_cast<int>(vl_vview_.size());
    auto cell = [&](int which, int w, int r, int sel, int n, const std::function<std::string(int)>& text, const std::function<bool(int)>& locked, const std::string& empty_text) {
        const bool focus = vl_pane_ == which;
        const int cw = w - 4;
        int first = 0;
        if (sel >= list_rows) first = sel - list_rows + 1;
        const int i = first + r;
        if (i < n) {
            const bool lk = locked(i);
            const std::string full = (lk ? "\xC2\xBB " : "  ") + text(i);
            // the highlighted row scrolls as a marquee when it does not fit (the other rows are cut)
            const std::string line = (i == sel && focus) ? marquee_or_truncate(full, cw, mq_key(full), vl_marq_[which], vl_marq_since_[which]) : pad_right(truncate_str(full, cw), cw);
            std::string painted;
            if (i == sel && focus) painted = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + line + R;
            else if (lk) painted = hdr + line + R;
            else painted = line;
            return pline(painted, cw, w);
        }
        if (r == 0 && n == 0) return pline(pad_right(truncate_str(empty_text, cw), cw), cw, w);
        return pline(std::string(), 0, w);
    };
    static const char* const fnames[4] = {"all files", "audio", "videos", "no video yet"};
    both(box_top(title(0, std::string("SONGS (") + fnames[vl_filter_] + ")"), pw_l, border), box_top(title(1, "VIDEO FILES"), pw_r, border));
    for (int r = 0; r < list_rows; ++r) {
        const std::string l = cell(0, pw_l, r, vl_tsel_, nt, [&](int i) {
            const VLEntry& e = vl_tracks_[static_cast<size_t>(vl_tview_[static_cast<size_t>(i)])];
            const bool linked = !e.video && VideoLinks::get().has(path_utf8(e.path));
            return std::string(e.video ? "[video] " : linked ? "[+] " : "") + e.label;
        }, [&](int i) { return vl_tracks_[static_cast<size_t>(vl_tview_[static_cast<size_t>(i)])].path == vl_lock_track_; }, "no match");
        const std::string rr = cell(1, pw_r, r, vl_vsel_, nv, [&](int i) {
            const VLEntry& e = vl_videos_[static_cast<size_t>(vl_vview_[static_cast<size_t>(i)])];
            return e.label + (e.folder.empty() ? std::string() : "  (" + e.folder + ")");
        }, [&](int i) { return vl_videos_[static_cast<size_t>(vl_vview_[static_cast<size_t>(i)])].path == vl_lock_video_; },
        vl_videos_.empty() ? "no video files found in the library folders" : "no match");
        both(l, rr);
    }
    {
        const auto l = cut(std::to_string(nt) + " of " + std::to_string(vl_tracks_.size()) + " files   [+] has a connected video", pw_l, leg);
        const auto r = cut(std::to_string(nv) + " of " + std::to_string(vl_videos_.size()) + " video files", pw_r, leg);
        both(pline(l.first, l.second, pw_l), pline(r.first, r.second, pw_r));
    }
    both(box_bottom(pw_l, "", border), box_bottom(pw_r, "", border));
    both(box_top("LOCKED IN: SONG", pw_l, border), box_top("LOCKED IN: VIDEO", pw_r, border));
    {
        const bool has_t = !vl_lock_track_.empty(), has_v = !vl_lock_video_.empty();
        const auto l1 = mq(2, has_t ? video_link_label(vl_lock_track_) : "(none yet -- highlight a song, press [ENTER])", pw_l, has_t ? hdr : std::string());
        const auto r1 = mq(3, has_v ? path_utf8(vl_lock_video_.filename()) : "(none yet -- highlight a video, press [ENTER])", pw_r, has_v ? hdr : std::string());
        both(pline(l1.first, l1.second, pw_l), pline(r1.first, r1.second, pw_r));
        std::string now;   // what the locked-in song has now
        if (has_t) {
            const std::string linked = VideoLinks::get().video_for(path_utf8(vl_lock_track_));
            std::error_code ec;
            if (!linked.empty()) {
                const std::string dl = path_utf8(video_down_dir());
                const bool from_dl = !dl.empty() && linked.compare(0, dl.size(), dl) == 0;
                now = std::string(from_dl ? "paired (download): " : "paired (by hand): ") + path_utf8(path_from_utf8(linked).filename()) + (fs::exists(path_from_utf8(linked), ec) ? "" : " (file is gone)");
            }
            else {
                const std::string auto_v = video_source_for(vl_lock_track_);
                now = auto_v.empty() ? "no video yet" : "paired (same name): " + path_utf8(path_from_utf8(auto_v).filename());
            }
        }
        const auto l2 = mq(4, now, pw_l, leg);
        const auto r2 = mq(5, has_v ? path_utf8(vl_lock_video_.parent_path()) : std::string(), pw_r, leg);
        both(pline(l2.first, l2.second, pw_l), pline(r2.first, r2.second, pw_r));
    }
    both(box_bottom(pw_l, "", border), box_bottom(pw_r, "", border));
    {
        const int inner = PW - 4, gap = 4;
        std::string b1 = "[ Connect song and video ]", b2 = "[CTRL+c] Clear choice";
        const int w1 = display_width(b1), w2 = display_width(b2);
        const int lead = std::max(0, (inner - w1 - gap - w2) / 2);
        const std::string p1 = vl_pane_ == 2 ? "\x1b[7m" + b1 + R : hdr + b1 + R;
        const std::string p2 = vl_pane_ == 3 ? "\x1b[7m" + b2 + R : hdr + b2 + R;
        out.push_back(bar + " " + std::string(static_cast<size_t>(lead), ' ') + p1 + std::string(static_cast<size_t>(gap), ' ') + p2 +
                      std::string(static_cast<size_t>(std::max(0, inner - lead - w1 - gap - w2)), ' ') + " " + bar);
    }
    out.push_back(bar + " " + (vl_warn_ && !vl_msg_.empty() ? std::string("\x1b[31m") : leg) + pad_right(truncate_str(vl_msg_, PW - 4), PW - 4) + R + " " + bar);
    out.push_back(centred_bottom(settings_, PW, "[TAB] pane  [ENTER] lock in  [CTRL+f] filter  [DEL] remove connection  [ESC] close", border_bottom));
    return out;
}

} // namespace muisc
