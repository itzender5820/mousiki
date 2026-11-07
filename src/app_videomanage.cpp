// The VIDEO FILES tab of the FETCH VIDEOS overlay (Mode::VideoFetch, vf_tab_ == 1), part of class App (src/app.h). It lists the video files of
// the library (and every video a song is connected to), shows which songs use the one under the cursor -- with a frame of the video on the right --
// and deletes one for good ([DEL], asked twice). Filters: all videos / videos coupled with a song / video only (no song uses it).
// Also here: video_delete_file(), shared with "Delete COMPLETE VIDEO FILE connected to track" of the VIDEO OPTION panel.
#include "app.h"
#include "app_internal.h"
#include "cover_art.h"
#include "console_log.h"
#include "overlay_common.h"
#include "path_utf8.h"
#include "string_util.h"
#include "terminal_ui.h"
#include "text_edit.h"
#include "utf8_util.h"
#include "video_links.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace muisc {

namespace {

std::string vm_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string vm_size_text(std::uintmax_t b) {
    char buf[32];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(b) / (1ull << 30));
    else if (b >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / (1ull << 20));
    else std::snprintf(buf, sizeof buf, "%.0f KB", static_cast<double>(b) / 1024.0);
    return buf;
}

}  // namespace

// ---- deleting ------------------------------------------------------------------------------------------------------------------------

fs::path App::video_trash_dir() const { return config_path().parent_path() / "video_trash"; }   // (next to config.txt: not a library folder, so the held files are not listed as videos)

void App::video_trash_purge() {
    std::error_code ec;
    fs::remove_all(video_trash_dir(), ec);
    vdel_undo_.clear();
}

// A video is held in video_trash (download folder) until Mousiki closes: [u] in the VIDEO FILES tab brings the last one back.
static bool vm_move_file(const fs::path& from, const fs::path& to, std::error_code& ec) {
    fs::rename(from, to, ec);
    if (!ec) return true;
    ec.clear();   // (another drive: copy, then remove)
    fs::copy_file(from, to, fs::copy_options::none, ec);
    if (ec) return false;
    fs::remove(from, ec);
    if (ec) { std::error_code e2; fs::remove(to, e2); return false; }
    return true;
}

bool App::video_undelete(std::string& msg) {
    if (vdel_undo_.empty()) { msg = "nothing to undo"; return false; }
    const VDelUndo u = vdel_undo_.back();
    const fs::path orig = path_from_utf8(u.orig), held = path_from_utf8(u.held);
    std::error_code ec;
    if (fs::exists(orig, ec)) { msg = "cannot restore: " + path_utf8(orig.filename()) + " exists again"; return false; }
    fs::create_directories(orig.parent_path(), ec);
    ec.clear();
    if (!fs::exists(held, ec)) { vdel_undo_.pop_back(); msg = "cannot restore: the held file is gone"; return false; }
    if (!vm_move_file(held, orig, ec)) { msg = "cannot restore " + path_utf8(orig.filename()) + ": " + ec.message(); return false; }
    vdel_undo_.pop_back();
    for (const std::string& t : u.tracks) VideoLinks::get().set(t, u.orig);
    ++video_rev_;
    video_for_track_.clear();
    video_pic_key_.clear();
    snap_track_.clear();
    cover_store_touch();
    if (settings_.video_mode) rescan_now();
    msg = "restored: " + path_utf8(orig.filename()) + (u.tracks.empty() ? std::string() : " (" + std::to_string(u.tracks.size()) + (u.tracks.size() == 1 ? " connection" : " connections") + " back)");
    log_event(msg);
    force_redraw_ = true;
    return true;
}

bool App::video_delete_file(const std::string& video, std::string& msg, bool undoable) {
    if (video.empty()) { msg = "no video"; return false; }
    const fs::path p = path_from_utf8(video);
    if (!LocalSource::is_video_file(p)) { msg = "not a video file: " + path_utf8(p.filename()); return false; }
    if (has_track_ && !current_path_.empty() && (current_path_ == p || video_source_for(current_path_) == video)) {
        video_.stop();   // (the player holds the file while its picture is shown)
        video_for_track_.clear();
    }
    std::error_code ec;
    const bool existed = fs::exists(p, ec);
    VDelUndo undo;
    if (existed && undoable) {   // the file is moved to the holding folder (not removed) so that it can be brought back
        static bool purged_old = false;
        if (!purged_old) { purged_old = true; fs::remove_all(video_trash_dir(), ec); ec.clear(); }   // (leftovers of a run that did not end cleanly)
        fs::create_directories(video_trash_dir(), ec);
        ec.clear();
        const fs::path held = video_trash_dir() / (std::to_string(vdel_undo_.size() + 1) + "_" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xffffff)) + "_" + path_utf8(p.filename()));
        if (vm_move_file(p, held, ec)) { undo.orig = video; undo.held = path_utf8(held); }
        else if (!fs::remove(p, ec) || ec) { msg = "cannot delete " + path_utf8(p.filename()) + ": " + (ec ? ec.message() : std::string("not allowed")); return false; }
    } else if (existed && (!fs::remove(p, ec) || ec)) { msg = "cannot delete " + path_utf8(p.filename()) + ": " + (ec ? ec.message() : std::string("not allowed")); return false; }
    // the connections of this file go (a link to a file that is not there would only be mended to another one of the same name)
    int links = 0;
    for (const std::string& tr : VideoLinks::get().tracks_for(video)) { undo.tracks.push_back(tr); VideoLinks::get().remove(tr); ++links; }
    if (!undo.held.empty()) { vdel_undo_.push_back(undo); if (vdel_undo_.size() > 30) { std::error_code e3; fs::remove(path_from_utf8(vdel_undo_.front().held), e3); vdel_undo_.erase(vdel_undo_.begin()); } }
    ++video_rev_;
    video_for_track_.clear();
    video_pic_key_.clear();
    snap_track_.clear();
    cover_store_touch();   // (a disk that showed a frame of it shows the cover again)
    if (settings_.video_mode) rescan_now();
    msg = "deleted: " + path_utf8(p.filename()) + (links > 0 ? " (" + std::to_string(links) + (links == 1 ? " connection" : " connections") + " removed)" : std::string());
    log_event(msg);
    force_redraw_ = true;
    return true;
}

// ---- the list ------------------------------------------------------------------------------------------------------------------------

void App::video_manage_rebuild() {
    std::vector<LocalTrack> scanned;
    const std::vector<LocalTrack>* src = &all_local_tracks_;
    if (!settings_.video_mode) {   // the library list holds no videos while the Video mode is off: look at the folders once for them
        LocalSource ls;
        ls.set_include_video(true);
        std::vector<std::string> diag;
        scanned = ls.scan(library_roots(), &diag);
        src = &scanned;
    }
    std::unordered_map<std::string, std::vector<std::string>> by_stem;   // folder + file name of a song -> the song (for the pairing by file name)
    std::unordered_map<std::string, std::string> song_label;
    std::vector<fs::path> videos;
    std::unordered_set<std::string> seen;
    for (const LocalTrack& t : *src) {
        if (LocalSource::is_video_file(t.path)) {
            if (seen.insert(path_utf8(t.path)).second) videos.push_back(t.path);
            continue;
        }
        const std::string key = path_utf8(t.path);
        by_stem[vm_lower(path_utf8(t.path.parent_path())) + "/" + vm_lower(path_utf8(t.path.stem()))].push_back(key);
        song_label[key] = (t.folder_artist.empty() || t.folder_artist == "-") ? t.title : t.folder_artist + " - " + t.title;
    }
    std::unordered_map<std::string, std::vector<std::string>> linked;   // video -> the songs connected by hand / by a download
    for (const auto& kv : VideoLinks::get().all()) {
        if (kv.second.empty() || kv.second == "-") continue;
        linked[kv.second].push_back(kv.first);
        std::error_code ec;
        if (fs::exists(path_from_utf8(kv.second), ec) && seen.insert(kv.second).second) videos.push_back(path_from_utf8(kv.second));   // (a video outside the library folders)
    }
    std::string keep;
    if (!vm_view_.empty() && vm_sel_ >= 0 && vm_sel_ < static_cast<int>(vm_view_.size())) keep = path_utf8(vm_all_[static_cast<size_t>(vm_view_[static_cast<size_t>(vm_sel_)])].path);
    vm_all_.clear();
    for (const fs::path& v : videos) {
        VMEntry e;
        e.path = v;
        const std::string vk = path_utf8(v);
        e.label = path_utf8(v.filename());
        e.folder = path_utf8(v.parent_path().filename());
        std::error_code ec;
        e.bytes = fs::file_size(v, ec);
        if (ec) e.bytes = 0;
        ec.clear();
        { const auto ft = fs::last_write_time(v, ec); e.mtime = ec ? 0 : static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(ft.time_since_epoch()).count()); }
        std::unordered_set<std::string> songs;
        const auto lit = linked.find(vk);
        if (lit != linked.end()) for (const std::string& s : lit->second) songs.insert(s);
        const auto sit = by_stem.find(vm_lower(path_utf8(v.parent_path())) + "/" + vm_lower(path_utf8(v.stem())));
        if (sit != by_stem.end())
            for (const std::string& s : sit->second) {
                if (VideoLinks::get().is_none(s)) continue;                              // (the connection was removed on purpose)
                const std::string other = VideoLinks::get().video_for(s);
                if (!other.empty() && other != vk) continue;                              // (the song is connected to another video)
                songs.insert(s);
            }
        for (const std::string& s : songs) e.songs.push_back(s);
        std::sort(e.songs.begin(), e.songs.end());
        e.coupled = !e.songs.empty();
        e.hay = vm_lower(with_latin_spelling(e.label) + " " + e.folder);
        for (const std::string& s : e.songs) { const auto l = song_label.find(s); e.hay += " " + vm_lower(l != song_label.end() ? l->second : path_utf8(path_from_utf8(s).stem())); }
        vm_all_.push_back(std::move(e));
    }
    std::sort(vm_all_.begin(), vm_all_.end(), [](const VMEntry& a, const VMEntry& b) { return vm_lower(a.label) < vm_lower(b.label); });
    video_manage_refilter();
    if (!keep.empty())
        for (size_t k = 0; k < vm_view_.size(); ++k)
            if (path_utf8(vm_all_[static_cast<size_t>(vm_view_[k])].path) == keep) { vm_sel_ = static_cast<int>(k); break; }
}

void App::video_manage_refilter() {
    std::vector<std::string> words;
    std::string cur;
    for (char c : vm_lower(vm_query_)) {
        if (c == ' ') { if (!cur.empty()) words.push_back(cur); cur.clear(); } else cur += c;
    }
    if (!cur.empty()) words.push_back(cur);
    vm_view_.clear();
    for (size_t i = 0; i < vm_all_.size(); ++i) {
        const VMEntry& e = vm_all_[i];
        if (vm_filter_ == 1 && !e.coupled) continue;
        if (vm_filter_ == 2 && e.coupled) continue;
        bool ok = true;
        for (const std::string& w : words) if (e.hay.find(w) == std::string::npos) { ok = false; break; }
        if (ok) vm_view_.push_back(static_cast<int>(i));
    }
    if (vm_filter_ == 3)   // newly added / downloaded files first
        std::stable_sort(vm_view_.begin(), vm_view_.end(), [&](int a, int b) { return vm_all_[static_cast<size_t>(a)].mtime > vm_all_[static_cast<size_t>(b)].mtime; });
    vm_sel_ = std::clamp(vm_sel_, 0, std::max(0, static_cast<int>(vm_view_.size()) - 1));
}

// The sound of a video-only file as an mp3 song (ffmpeg, in the background): saved in the regular download folder, connected to the video.
void App::video_manage_make_mp3() {
    if (vm_mp3_fut_.valid()) { vf_msg_ = "an mp3 is being made already"; vf_warn_ = true; return; }
    if (vm_view_.empty()) return;
    const VMEntry& e = vm_all_[static_cast<size_t>(vm_view_[static_cast<size_t>(std::clamp(vm_sel_, 0, static_cast<int>(vm_view_.size()) - 1))])];
    if (e.coupled) { vf_msg_ = "this video is coupled with a song already -- the mp3 is for video only files"; vf_warn_ = true; return; }
    const std::string video = path_utf8(e.path);
    const std::string stem = path_utf8(e.path.stem());   // (already a valid file name)
    const std::string base = stem.empty() ? std::string("video") : stem;
    const std::string title = path_utf8(e.path.stem());
    const fs::path dir = cache_.download_dir();
    vm_mp3_video_ = video;
    vf_msg_ = "making an mp3 from " + e.label + " ...";
    vf_warn_ = false;
    vm_mp3_fut_ = std::async(std::launch::async, [video, dir, base, title]() {
        std::string err;
        const std::string mp3 = video_make_mp3_file(video, dir, base, title, err);
        return std::make_pair(mp3, err);
    });
}

// ---- keys ------------------------------------------------------------------------------------------------------------------------------

void App::video_manage_key(int key) {
    const bool arrow = last_key_was_arrow();
    auto hovered = [&]() -> const VMEntry* {
        if (vm_view_.empty()) return nullptr;
        return &vm_all_[static_cast<size_t>(vm_view_[static_cast<size_t>(std::clamp(vm_sel_, 0, static_cast<int>(vm_view_.size()) - 1))])];
    };
    if (key == 27) {
        if (!vm_confirm_.empty()) { vm_confirm_.clear(); vf_msg_ = "delete cancelled"; vf_warn_ = false; return; }
        if (vf_focus_ == 0 && !vm_query_.empty()) { vm_query_.clear(); edit_caret_ = edit_anchor_ = 0; vm_sel_ = 0; video_manage_refilter(); return; }
        video_fetch_close();
        return;
    }
    if (!arrow && key == '\t') { text_disengage(); vf_focus_ = vf_focus_ == 0 ? 1 : 0; return; }
    if (key == kKeyCtrlF) {   // which videos are listed: all / coupled with a song / video only
        text_disengage();
        vm_confirm_.clear();
        vm_filter_ = (vm_filter_ + 1) % 4;
        video_manage_refilter();
        vm_sel_ = 0;
        vf_msg_ = vm_filter_ == 0 ? "listed: all videos" : vm_filter_ == 1 ? "listed: videos that are coupled with a song" : vm_filter_ == 2 ? "listed: video only (no song uses them)" : "listed: all videos, the newest (added / downloaded last) first";
        vf_warn_ = false;
        return;
    }
    if (vf_focus_ != 0 && !arrow && key_is("HKeySearch", key)) {   // "/": to the filter field
        vf_focus_ = 0;
        edit_focus("vm-search", vm_query_);
        text_engage("vm-search");
        edit_caret_ = edit_anchor_ = vm_query_.size();
        return;
    }
    if (vf_focus_ == 0) {   // --- the filter field ---
        if (key == '\r' || key == '\n' || (arrow && key == 'B')) { text_disengage(); vf_focus_ = 1; return; }
        if (arrow) return;
        edit_focus("vm-search", vm_query_);
        text_engage("vm-search");
        std::string note;
        if (edit_text_key(vm_query_, edit_caret_, edit_anchor_, key, 120, &note)) { vm_sel_ = 0; video_manage_refilter(); }
        if (!note.empty()) { vf_msg_ = note; vf_warn_ = false; }
        return;
    }
    // --- the list ---
    const int n = static_cast<int>(vm_view_.size());
    if (!vm_confirm_.empty()) {   // the deletion waits for its answer
        const std::string p = vm_confirm_;
        vm_confirm_.clear();
        if (!arrow && (key == kKeyDelete || key == '\r' || key == '\n')) {
            std::string msg;
            const bool ok = video_delete_file(p, msg);
            vf_msg_ = msg;
            vf_warn_ = !ok;
            video_manage_rebuild();
            video_fetch_rebuild_list();   // (the songs of the first tab have no video now)
            return;
        }
        vf_msg_ = "delete cancelled";
        vf_warn_ = false;
        if (key == 27) return;
    }
    if (arrow && (key == 'A' || key == 'B')) {
        if (key == 'A' && vm_sel_ == 0 && !last_key_was_ctrl()) { vf_focus_ = 0; return; }
        vm_sel_ = video_fetch_nav(vm_sel_, n, key == 'A');
        return;
    }
    if (key == kKeyHome) { vm_sel_ = 0; return; }
    if (key == kKeyEnd) { vm_sel_ = std::max(0, n - 1); return; }
    if (key == kKeyCtrl3 || (!arrow && key == 'm')) { video_manage_make_mp3(); return; }   // (CTRL+3 is reported only by terminals that send modified keys; m works everywhere)
    if (!arrow && key == 'u') {   // undo the last deletion
        std::string msg;
        const bool ok = video_undelete(msg);
        vf_msg_ = msg;
        vf_warn_ = !ok;
        if (ok) { video_manage_rebuild(); video_fetch_rebuild_list(); }
        return;
    }
    const VMEntry* h = hovered();
    if (!h) return;
    if (key == kKeyDelete) {   // DEL: delete this video file for good (the second DEL / ENTER confirms)
        vm_confirm_ = path_utf8(h->path);
        vf_msg_ = h->coupled ? "DELETE " + h->label + "? Used by " + std::to_string(h->songs.size()) + (h->songs.size() == 1 ? " song" : " songs") + " -- they lose the video. [DEL] confirms"
                             : "DELETE " + h->label + "? No song uses it. [DEL] confirms";
        vf_warn_ = true;
        return;
    }
}

// ---- drawing -----------------------------------------------------------------------------------------------------------------------------

std::vector<std::string> App::build_vm_list_panel(int w, int h) const {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const std::string hdr = header_sgr(settings_);
    const int inner = w - 4;
    std::vector<std::string> out;
    int coupled = 0;
    for (const VMEntry& e : vm_all_) if (e.coupled) ++coupled;
    const char* fname = vm_filter_ == 0 ? "all videos" : vm_filter_ == 1 ? "coupled" : vm_filter_ == 2 ? "video only" : "newest first";
    out.push_back(box_top(std::string("VIDEOS, ") + fname + " (" + std::to_string(vm_view_.size()) + " of " + std::to_string(vm_all_.size()) + "), " + std::to_string(coupled) + " coupled" +
                              (vf_focus_ == 1 ? focus_indicator(settings_, border) : std::string()), w, border));
    const int rows = std::max(1, h - 2);
    const int n = static_cast<int>(vm_view_.size());
    const int sel = std::clamp(vm_sel_, 0, std::max(0, n - 1));
    const int first = std::clamp(sel - rows / 2, 0, std::max(0, n - rows));
    for (int r = 0; r < rows; ++r) {
        const int i = first + r;
        if (i >= n) {
            std::string c = (r == 0 && n == 0) ? truncate_str(vm_all_.empty() ? "no video files in the library" : "no video matches the filter", inner) : std::string();
            out.push_back(bar + " " + pad_right(c, inner) + " " + bar);
            continue;
        }
        const VMEntry& e = vm_all_[static_cast<size_t>(vm_view_[static_cast<size_t>(i)])];
        const std::string text = std::string(e.coupled ? "[c] " : "[ ] ") + apply_font_map(e.label, settings_.font_map);   // [c] coupled with a song   [ ] video only
        const std::string padded = pad_right(truncate_str(text, inner), inner);
        const bool hi = i == sel && vf_focus_ == 1;
        // the coupled ones are highlighted, the cursor row has its own colours
        out.push_back(bar + " " + (hi ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + padded + R : (e.coupled ? hdr + padded + R : padded)) + " " + bar);
    }
    const int remaining = n - (first + rows);
    out.push_back(box_bottom(w, remaining > 0 ? "(" + std::to_string(remaining) + " more)" : std::string(), border_bottom));
    return out;
}

std::vector<std::string> App::build_vm_detail_panel(int w, int h) const {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const std::string hdr = header_sgr(settings_);
    const std::string leg = legend_sgr(settings_);
    const int inner = w - 4;
    auto line = [&](const std::string& content, int used) { return bar + " " + content + std::string(static_cast<size_t>(std::max(0, inner - used)), ' ') + " " + bar; };
    auto text_line = [&](const std::string& t, const std::string& sgr) {
        const std::string c = truncate_str(t, inner);
        return line(sgr + c + R, display_width(c));
    };
    std::vector<std::string> out;
    const VMEntry* e = vm_view_.empty() ? nullptr : &vm_all_[static_cast<size_t>(vm_view_[static_cast<size_t>(std::clamp(vm_sel_, 0, static_cast<int>(vm_view_.size()) - 1))])];
    out.push_back(box_top("VIDEO AND COUPLED SONGS", w, border));
    if (!e) {
        for (int r = 0; r < h - 2; ++r) out.push_back(line("", 0));
        out.push_back(box_bottom(w, "", border_bottom));
        return out;
    }
    out.push_back(text_line("Video:   " + apply_font_map(e->label, settings_.font_map), hdr));
    out.push_back(text_line("Folder:  " + path_utf8(e->path.parent_path()), leg));
    out.push_back(text_line("Size:    " + vm_size_text(e->bytes), leg));
    // the songs that show this video (no reserved rows: as many lines as songs, at most 4; the rest is counted)
    if (e->songs.empty()) {
        out.push_back(text_line("Coupled: video only -- no song uses it", leg));
    } else {
        const int total = static_cast<int>(e->songs.size());
        out.push_back(text_line(std::string("Coupled: ") + std::to_string(total) + (total == 1 ? " song" : " songs"), hdr));
        const int shown = total > 4 ? 3 : total;
        for (int r = 0; r < shown; ++r) out.push_back(text_line("  " + apply_font_map(path_utf8(path_from_utf8(e->songs[static_cast<size_t>(r)]).filename()), settings_.font_map), leg));
        if (total > shown) out.push_back(text_line("  ... and " + std::to_string(total - shown) + " more", leg));
    }
    out.push_back(line("", 0));
    if (!e->coupled) {   // video only files: the mp3 command above the note about the deletion
        const bool making = vm_mp3_fut_.valid();
        out.push_back(text_line(making ? "making the mp3 ..." : "[CTRL+3 / m] makes an mp3 song from this video (download folder)", making ? hdr : leg));
    }
    {
        const bool asking = !vm_confirm_.empty();
        out.push_back(text_line(asking ? "[DEL] again deletes it" : "[DEL] deletes this video file from the disk (asked first)", asking ? std::string("\x1b[31m") : leg));
    }
    // a frame of the video: a real picture (Kitty / Sixel)
    out.push_back(line("", 0));   // an empty row above the preview ...
    const int used = static_cast<int>(out.size());
    const int pic_rows = std::max(2, h - used - 2);   // ... and one below it
    const std::string vpath = path_utf8(e->path);
    const int scr_col = vf_art_col_ + 2, scr_row = 3 + used;
    bool as_picture = false;
    if (gfx_proto_ != GfxProto::None) as_picture = cover_full_picture_img(preview_video_frame(vpath), inner, pic_rows, scr_col, scr_row, false);
    const std::string note = preview_video_frame_loading(vpath) ? "reading a frame ..." : gfx_proto_ == GfxProto::None ? "(no picture in this terminal)" : "(the picture cannot be read)";
    for (int r = 0; r < pic_rows; ++r) {
        if (as_picture) out.push_back(bar + " " + gfx_hole(inner) + " " + bar);
        else out.push_back(r == pic_rows / 2 ? text_line(std::string(static_cast<size_t>(std::max(0, (inner - display_width(note)) / 2)), ' ') + note, leg) : line("", 0));
    }
    out.push_back(line("", 0));
    out.push_back(box_bottom(w, "", border_bottom));
    return out;
}

}  // namespace muisc
