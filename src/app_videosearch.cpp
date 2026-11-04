// app_videosearch.cpp
// Part of class App (src/app.h): SEARCH YT (Mode::VideoSearch), reached from the VIDEO overlay ("&"): a search for videos of the playing
// song (the query starts as its file name and can be edited), the results with a preview picture of each, and the download of the chosen
// one into <download folder>/video_down. The downloaded file is named like the song and connected to it at once (video_links.h).
// Search and download run in the background (yt-dlp); the overlay can be closed while a download runs.
#include "app.h"
#include "app_internal.h"
#include "console_log.h"
#include "cover_art.h"
#include "open_url.h"
#include "overlay_common.h"
#include "path_utf8.h"
#include "process_util.h"
#include "string_util.h"
#include "terminal_ui.h"
#include "text_edit.h"
#include "utf8_util.h"
#include "video_links.h"
#include "video_player.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace muisc {

namespace {
// A name for a file made from text: no characters a file system refuses.
std::string safe_file_name(std::string s) {
    for (char& c : s) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 32) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    if (s.size() > 150) s.resize(150);
    return s;
}
// "03 - Song" / "03. Song" -> "Song"
std::string without_track_number(const std::string& stem) {
    size_t i = 0;
    while (i < stem.size() && (std::isdigit(static_cast<unsigned char>(stem[i])) || stem[i] == ' ' || stem[i] == '.' || stem[i] == '-' || stem[i] == '_')) ++i;
    return i < stem.size() ? stem.substr(i) : stem;
}
std::string vs_clock_text(double sec) {
    if (sec < 0) return "";
    const int s = static_cast<int>(sec + 0.5);
    char b[24];
    if (s >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}
}  // namespace

fs::path App::video_down_dir() const { return cache_.download_dir() / "video_down"; }

// The start of the search: the file name of the song (without its track number), or -- vs_by_tags_ -- "artist title" from its tags.
std::string App::video_search_default_query() const {
    if (!has_track_ || current_path_.empty()) return std::string();
    if (vs_by_tags_) {
        const TrackMetadata md = display_metadata();
        const std::string artist = (md.artist == "-") ? std::string() : md.artist;
        const std::string title = (md.name == "-") ? std::string() : md.name;
        if (!title.empty()) return artist.empty() ? title : artist + " " + title;
    }
    return without_track_number(path_utf8(current_path_.stem()));
}

// The yt-dlp command for a download (a function of its own: tested without a network).
std::vector<std::string> video_download_args(const std::string& id, const fs::path& dir, const std::string& base_name, int max_height) {
    const std::string h = std::to_string(std::clamp(max_height, 144, 2160));
    return {"yt-dlp", "-4", "--no-warnings", "--no-playlist", "--newline", "--progress",
            "--encoding", "utf-8", "-f", "bv*[height<=" + h + "]+ba/b[height<=" + h + "]/b", "--merge-output-format", "mp4",
            "--progress-template", "download:PCT %(progress._percent_str)s",
            "--print", "after_move:FILE:%(filepath)s",
            "-o", path_utf8(dir / path_from_utf8(base_name + ".%(ext)s")),
            "https://www.youtube.com/watch?v=" + id};
}

void App::video_search_start() {
    if (vs_busy_) return;
    const std::string q = trim_ws(vs_query_);
    if (q.empty()) { vs_msg_ = "type what to search for"; vs_warn_ = true; return; }
    vs_busy_ = true;
    vs_warn_ = false;
    vs_msg_ = "searching ...";
    vs_results_.clear();
    vs_sel_ = 0;
    vs_future_ = std::async(std::launch::async, [q]() { OnlineSource src; return src.search_videos(q, 10); });
}

void App::video_search_open() {
    if (vs_query_.empty() || vs_query_for_ != path_utf8(current_path_)) {
        vs_query_ = video_search_default_query();
        vs_query_for_ = path_utf8(current_path_);
    }
    edit_owner_.clear();   // (the caret of the text field goes to the end of the text)
    vs_target_ = has_track_ ? current_path_ : fs::path();
    vs_pane_ = 0;
    mode_ = Mode::VideoSearch;
    force_redraw_ = true;
    if (vs_results_.empty() && !vs_busy_ && !vs_query_.empty()) video_search_start();
}

// /sv:query -- the SEARCH YT overlay with that text, searched at once
void App::video_search_open(const std::string& query) {
    vs_query_ = query;
    vs_query_for_ = path_utf8(current_path_);
    edit_owner_.clear();
    vs_target_ = has_track_ ? current_path_ : fs::path();
    vs_pane_ = 0;
    vs_results_.clear();
    vs_busy_ = false;
    mode_ = Mode::VideoSearch;
    force_redraw_ = true;
    if (!vs_query_.empty()) { video_search_start(); vs_pane_ = 1; }
}

void App::video_search_close() {
    text_disengage();
    mode_ = Mode::Browse;
    force_redraw_ = true;
}

void App::video_download_start(const OnlineResult& r) {
    if (vd_.active) { vs_msg_ = "a download is running already"; vs_warn_ = true; return; }
    const fs::path dir = video_down_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { vs_msg_ = "cannot create " + path_utf8(dir) + ": " + ec.message(); vs_warn_ = true; return; }
    std::string base = vs_target_.empty() ? safe_file_name(r.title) : safe_file_name(path_utf8(vs_target_.stem()));
    if (base.empty()) base = r.video_id;
    {
        std::lock_guard<std::mutex> lk(vd_.m);
        vd_.progress = "starting ...";
        vd_.file.clear();
        vd_.error.clear();
        vd_.done = false;
    }
    vd_.active = true;
    vd_.dir = dir;
    vd_.base = base;
    vd_.title = r.title;
    vd_.track = vs_target_;
    const std::vector<std::string> args = video_download_args(r.video_id, dir, base, settings_.video_down_height);
    ConsoleLog::instance().log_basic("video download: " + command_text(args));
    vd_.thread = std::thread([this, args]() {
        std::unique_ptr<ChildProcess> child = spawn_capture(args, true);
        if (!child) {
            std::lock_guard<std::mutex> lk(vd_.m);
            vd_.error = "yt-dlp could not be started (is it installed and on the PATH?)";
            vd_.done = true;
            return;
        }
        {
            std::lock_guard<std::mutex> lk(vd_.child_m);
            vd_.child = child.get();
        }
        std::string buf, last_error;
        char chunk[4096];
        for (;;) {
            const std::ptrdiff_t n = child->read(chunk, sizeof chunk);
            if (n <= 0) break;
            buf.append(chunk, static_cast<size_t>(n));
            size_t nl;
            while ((nl = buf.find_first_of("\r\n")) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.empty()) continue;
                if (line.rfind("download:PCT", 0) != 0 && line.rfind("PCT ", 0) != 0) ConsoleLog::instance().log_basic("video download: " + line);
                std::lock_guard<std::mutex> lk(vd_.m);
                if (line.rfind("download:PCT ", 0) == 0 || line.rfind("PCT ", 0) == 0) {
                    const size_t sp = line.find("PCT ");
                    vd_.progress = "downloading " + trim_ws(line.substr(sp + 4));
                } else if (line.rfind("FILE:", 0) == 0) {
                    vd_.file = trim_ws(line.substr(5));
                } else if (line.rfind("ERROR", 0) == 0) {
                    last_error = line;
                } else if (line.find("[Merger]") != std::string::npos || line.find("[VideoConvertor]") != std::string::npos) {
                    vd_.progress = "joining picture and sound ...";
                }
            }
        }
        const int code = child->wait();
        {
            std::lock_guard<std::mutex> lk(vd_.child_m);
            vd_.child = nullptr;
        }
        std::lock_guard<std::mutex> lk(vd_.m);
        if (code != 0 || vd_.file.empty()) vd_.error = !last_error.empty() ? last_error.substr(0, 200) : "yt-dlp ended with exit code " + std::to_string(code);
        vd_.done = true;
    });
}

// Once per frame: a finished search fills the list, a finished download is connected to its song.
void App::video_search_poll() {
    if (vs_busy_ && vs_future_.valid() && vs_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        vs_results_ = vs_future_.get();
        vs_busy_ = false;
        vs_sel_ = 0;
        vs_warn_ = vs_results_.empty();
        vs_msg_ = vs_results_.empty() ? "no videos found (is yt-dlp installed, and the computer online?)" : std::to_string(vs_results_.size()) + " videos found -- [ENTER] downloads the highlighted one";
        force_redraw_ = true;
    }
    if (vd_.active) {
        bool done;
        {
            std::lock_guard<std::mutex> lk(vd_.m);
            done = vd_.done;
        }
        if (!done) { if (mode_ == Mode::VideoSearch) { std::lock_guard<std::mutex> lk(vd_.m); vs_msg_ = vd_.title + ": " + vd_.progress; vs_warn_ = false; } return; }
        if (vd_.thread.joinable()) vd_.thread.join();
        vd_.active = false;
        std::string file, err;
        {
            std::lock_guard<std::mutex> lk(vd_.m);
            file = vd_.file; err = vd_.error;
        }
        std::string msg;
        bool warn = false;
        std::error_code ec;
        if (!fs::exists(path_from_utf8(file), ec)) {   // the printed path can be garbled by the console's code page: look for the file by the name it was given
            file.clear();
            std::filesystem::file_time_type newest{};
            for (fs::directory_iterator it(vd_.dir, ec), end; !ec && it != end; it.increment(ec)) {
                const fs::path p = it->path();
                const std::string ext = path_utf8(p.extension());
                if (path_utf8(p.stem()) != vd_.base || ext == ".part" || ext == ".ytdl" || ext == ".temp") continue;
                std::error_code e2;
                const auto tm = fs::last_write_time(p, e2);
                if (file.empty() || tm > newest) { file = path_utf8(p); newest = tm; }
            }
            if (!file.empty()) err.clear();
        }
        if (!err.empty() || file.empty() || !fs::exists(path_from_utf8(file), ec)) {
            msg = "download failed: " + (err.empty() ? std::string("no file was written (see the console log, lines \"video download:\")") : err);
            warn = true;
        } else {
            if (!vd_.track.empty()) {
                VideoLinks::get().set(path_utf8(vd_.track), file);
                ++video_rev_;
                video_for_track_.clear();
            }
            if (settings_.video_mode) rescan_now();   // the new file is listed
            msg = "downloaded to " + file + (vd_.track.empty() ? "" : " -- connected to the song");
        }
        if (mode_ == Mode::VideoSearch) { vs_msg_ = msg; vs_warn_ = warn; }
        status_line_ = msg;
        log_event(msg);
        force_redraw_ = true;
    }
}

void App::video_search_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (key == 27) { video_search_close(); return; }
    if (!arrow && key == '\t') { vs_pane_ = vs_pane_ == 0 ? 1 : 0; if (vs_pane_ == 1) text_disengage(); return; }
    if (key == kKeyCtrlC && vd_.active && vs_pane_ == 1) {   // (in the search field CTRL+c copies, as in every text field)
        std::lock_guard<std::mutex> lk(vd_.child_m);
        if (vd_.child) vd_.child->kill();
        vs_msg_ = "download cancelled";
        vs_warn_ = true;
        return;
    }
    if (vs_pane_ == 0) {
        if (key == '\r' || key == '\n') { text_disengage(); video_search_start(); if (!vs_results_.empty() || vs_busy_) vs_pane_ = 1; return; }
        if (arrow && key == 'B') { if (!vs_results_.empty()) { text_disengage(); vs_pane_ = 1; } return; }
        if (key == kKeyCtrlT) {   // the query from the file name <-> from the tags (artist + title), in the search field
            vs_by_tags_ = !vs_by_tags_;
            vs_query_ = video_search_default_query();
            edit_owner_.clear();
            edit_focus("vs-search", vs_query_);
            vs_msg_ = vs_by_tags_ ? "search text: artist + title from the tags" : "search text: the file name";
            vs_warn_ = false;
            return;
        }
        // a regular text field: caret, selection, copy / cut / paste, undo (CTRL+z / CTRL+y), HOME / END, word jumps
        edit_focus("vs-search", vs_query_);
        text_engage("vs-search");
        std::string note;
        edit_text_key(vs_query_, edit_caret_, edit_anchor_, key, 300, &note);
        if (!note.empty()) { vs_msg_ = note; vs_warn_ = false; }
        return;
    }
    const int n = static_cast<int>(vs_results_.size());
    if (arrow && key == 'A') { if (vs_sel_ == 0) vs_pane_ = 0; else --vs_sel_; return; }
    if (arrow && key == 'B') { vs_sel_ = std::min(std::max(0, n - 1), vs_sel_ + 1); return; }
    if (key == '\r' || key == '\n') {
        if (n == 0) return;
        video_download_start(vs_results_[static_cast<size_t>(vs_sel_)]);
        return;
    }
    if (!arrow && (key == 'o' || key == 'O') && n > 0) {   // the page in the browser
        open_url("https://www.youtube.com/watch?v=" + vs_results_[static_cast<size_t>(vs_sel_)].video_id);
        return;
    }
}

// The preview of a result: its thumbnail as braille dots (`cols` x `rows` cells), read in the background.
std::vector<std::string> App::video_search_preview(const OnlineResult& r, int cols, int rows) const {
    std::vector<std::string> out(static_cast<size_t>(rows), std::string(static_cast<size_t>(cols), ' '));
    const std::string url = "https://i.ytimg.com/vi/" + r.video_id + "/mqdefault.jpg";
    const CoverImage* img = preview_cover(url, false);
    if (!img) {
        const std::string t = preview_cover_loading(url, false) ? "loading preview ..." : "(no preview)";
        out[static_cast<size_t>(rows / 2)] = std::string(static_cast<size_t>(std::max(0, (cols - display_width(t)) / 2)), ' ') + t;
        out[static_cast<size_t>(rows / 2)] = pad_right(out[static_cast<size_t>(rows / 2)], cols);
        return out;
    }
    const int bw = cols * 2, bh = rows * 4;
    const CoverRgb& sc = cover_scaled_rgb(*img, bw, bh, 0);
    VideoFrame f;
    f.w = bw; f.h = bh;
    f.rgb.assign(static_cast<size_t>(bw) * bh * 3, 0);
    const int ox = std::max(0, (bw - sc.w) / 2), oy = std::max(0, (bh - sc.h) / 2);
    for (int y = 0; y < sc.h && y + oy < bh; ++y)
        for (int x = 0; x < sc.w && x + ox < bw; ++x)
            for (int k = 0; k < 3; ++k) f.rgb[(static_cast<size_t>(y + oy) * bw + x + ox) * 3 + k] = sc.rgb[(static_cast<size_t>(y) * sc.w + x) * 3 + k];
    const auto cells = VideoPlayer::braille(f, cols, rows);
    for (int rr = 0; rr < rows; ++rr) {
        std::string line;
        for (int c = 0; c < cols; ++c) {
            const auto& cell = cells[static_cast<size_t>(rr)][static_cast<size_t>(c)];
            if (cell.mask == 0) { line += ' '; continue; }
            const float lvl = std::round(cell.level / 255.0f * 7.0f) / 7.0f;
            const float tt = cols > 1 ? static_cast<float>(c) / static_cast<float>(cols - 1) : 0.0f;
            line += osci_dim_ansi(gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, tt), 0.30f + 0.70f * lvl);
            const int cp = 0x2800 + cell.mask;
            line += static_cast<char>(0xE0 | (cp >> 12));
            line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            line += static_cast<char>(0x80 | (cp & 0x3F));
        }
        out[static_cast<size_t>(rr)] = line + "\x1b[0m";
    }
    return out;
}

std::vector<std::string> App::build_video_search_panel(int W) const {
    const int PW = std::clamp(W - 2, 70, 112);
    const int inner = PW - 4;
    const int prev_w = std::min(36, inner / 3);
    const int list_w = inner - prev_w - 1;
    const int prev_rows = 9;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const std::string hdr = header_sgr(settings_);
    const std::string leg = legend_sgr(settings_);
    auto line = [&](const std::string& content, int used) { return bar + " " + content + std::string(static_cast<size_t>(std::max(0, inner - used)), ' ') + " " + bar; };
    std::vector<std::string> out;
    out.push_back(box_top(std::string("SEARCH YT") + (vs_pane_ == 0 ? focus_indicator(settings_, border) : std::string()), PW, border));
    {   // the query field: a regular text field (caret, selection, copy / paste, undo)
        const std::string label = "Search: ";
        const int room = std::max(1, inner - display_width(label) - 1);
        if (vs_pane_ == 0) {
            const EditPaint p = paint_edit_field(vs_query_, edit_owner_ == "vs-search" ? edit_caret_ : vs_query_.size(),
                                                 edit_owner_ == "vs-search" ? edit_anchor_ : vs_query_.size(), room,
                                                 vs_query_.empty() ? search_idle_sgr(settings_) : search_sgr(settings_), true);
            out.push_back(line(search_idle_sgr(settings_) + label + search_run(settings_, p.s, !vs_query_.empty()) + R, display_width(label) + p.cols));
        } else {
            const std::string shown = truncate_str(vs_query_, room);
            out.push_back(line(search_idle_sgr(settings_) + label + search_run(settings_, shown, !vs_query_.empty()) + R, display_width(label) + display_width(shown)));
        }
    }
    out.push_back(box_bottom(PW, "", border));
    const int n = static_cast<int>(vs_results_.size());
    const int sel = std::clamp(vs_sel_, 0, std::max(0, n - 1));
    const OnlineResult* cur = n > 0 ? &vs_results_[static_cast<size_t>(sel)] : nullptr;
    // The thumbnail is a real picture (Kitty / Sixel) over the preview cells; braille dots only when the terminal has neither.
    vs_pic_ = 0;
    std::vector<std::string> prev;
    if (cur && gfx_proto_ != GfxProto::None) {
        const int panel_h = 19;   // (constant: the search box 3, the RESULTS pane 11, the SELECTED box 5)
        const int start_col = 1 + std::max(0, (W - PW) / 2);
        const int start_row = std::clamp(1 + std::max(0, (term_rows_ - panel_h) / 2 - kFloatingPanelUpShift), 1, std::max(1, term_rows_ - panel_h));
        const bool second = gfx_.active;   // the main screen's cover stays (the panel covers part of it): the thumbnail takes the second slot
        const std::string url = "https://i.ytimg.com/vi/" + cur->video_id + "/mqdefault.jpg";
        if (cover_full_picture(url, false, prev_w, prev_rows, (start_col - 1) + list_w + 3, (start_row - 1) + 4, second)) {
            vs_pic_ = second ? 2 : 1;
            prev.assign(static_cast<size_t>(prev_rows), gfx_hole(prev_w));
        }
    }
    if (vs_pic_ == 0 && cur) prev = video_search_preview(*cur, prev_w, prev_rows);
    out.push_back(box_top(std::string("RESULTS") + (vs_pane_ == 1 ? focus_indicator(settings_, border) : std::string()), PW, border));
    for (int r = 0; r < prev_rows; ++r) {
        int first = 0;
        if (sel >= prev_rows) first = sel - prev_rows + 1;
        const int i = first + r;
        std::string l;
        int used = 0;
        if (i < n) {
            const OnlineResult& it = vs_results_[static_cast<size_t>(i)];
            const std::string dur = vs_clock_text(it.duration_sec);
            const std::string right = (dur.empty() ? "" : " " + dur);
            const std::string text = truncate_str((i == sel && vs_pane_ == 1 ? "> " : "  ") + it.title + "  - " + it.uploader, std::max(4, list_w - display_width(right)));
            std::string padded = pad_right(text, list_w - display_width(right)) + right;
            used = list_w;
            l = (i == sel && vs_pane_ == 1) ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + padded + R : (i == sel ? hdr + padded + R : padded);
        } else if (r == 0 && n == 0) {
            const std::string t = vs_busy_ ? "searching ..." : "no results yet -- press [ENTER] in the search field";
            l = pad_right(truncate_str(t, list_w), list_w);
            used = list_w;
        } else {
            l = std::string(static_cast<size_t>(list_w), ' ');
            used = list_w;
        }
        const std::string pv = (cur && static_cast<size_t>(r) < prev.size()) ? prev[static_cast<size_t>(r)] : std::string(static_cast<size_t>(prev_w), ' ');
        out.push_back(bar + " " + l + " " + pv + R + std::string(static_cast<size_t>(std::max(0, inner - list_w - 1 - prev_w)), ' ') + " " + bar);
        (void)used;
    }
    out.push_back(box_bottom(PW, "", border));
    out.push_back(box_top("SELECTED", PW, border));
    {
        const std::string info = cur ? truncate_str(cur->title + "  (" + cur->uploader + (cur->duration_sec >= 0 ? ", " + vs_clock_text(cur->duration_sec) : std::string()) + ")", inner) : std::string();
        out.push_back(line(hdr + info + R, display_width(info)));
        const fs::path dir = video_down_dir();
        const std::string head = "Downloads go to ";
        const std::string tail = vs_target_.empty() ? "" : "  as  " + safe_file_name(path_utf8(vs_target_.stem())) + ".mp4";
        std::string where = path_utf8(dir);
        // a path too long for the panel loses its beginning, not its end (the folder's name is what matters)
        const int room = std::max(8, inner - display_width(head) - display_width(tail));
        if (display_width(where) > room) {
            std::string cutp = where;
            while (display_width(cutp) > room - 3 && !cutp.empty()) {
                size_t i = 1;
                while (i < cutp.size() && (static_cast<unsigned char>(cutp[i]) & 0xC0) == 0x80) ++i;
                cutp.erase(0, i);
            }
            where = "..." + cutp;
        }
        const std::string d = truncate_str(head + where + tail, inner);
        out.push_back(line(leg + d + R, display_width(d)));
    }
    {
        const std::string m = truncate_str(vs_msg_, inner);
        out.push_back(line((vs_warn_ ? std::string("\x1b[31m") : leg) + m + R, display_width(m)));
    }
    out.push_back(centred_bottom(settings_, PW, "[ENTER] search / download  [TAB] field/list  [o] open page  [CTRL+t] file name / artist+title  [CTRL+c] copy / cancel download  [ESC] close", border_bottom));
    return out;
}

} // namespace muisc
