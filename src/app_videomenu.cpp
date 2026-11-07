// app_videomenu.cpp
// Part of class App (src/app.h): the VIDEO overlay (HKeyVideo, "&" = SHIFT+6 on a German keyboard) and the VIDEO OPTION overlay in which
// a track and a video file are connected by hand (the [ Video Option ] button of the meta editor, or the VIDEO overlay).
//
// VIDEO (Mode::VideoMenu): a small floating panel -- Use Video in Lyrics Field on / off, Include Lyrics, Cover, Disk or Thumbnail, and the doors to
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
#include <unordered_set>

namespace muisc {

// ======================================================================================================================
// VIDEO (the & overlay)
// ======================================================================================================================

void App::video_menu_open() {
    if (audiobook_) { status_line_ = "video: not available in the audio book player"; return; }
    vmenu_sel_ = std::clamp(vmenu_sel_, 0, 19);
    vdel_stage_ = 0;
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
        case 4: video_set_size((settings_.video_size + (dir < 0 ? 3 : 1)) % 4); break;
        case 5:   // Rounded Corners
            settings_.video_round = !settings_.video_round;
            set_config_value("VideoRound", settings_.video_round ? "true" : "false");
            status_line_ = settings_.video_round ? "video rounded corners: on -- the corners of the picture are cut round like the corners of the UI" : "video rounded corners: off";
            video_pic_key_.clear();
            break;
        case 6: video_set_fx(video_fx_step(settings_.video_fx, dir)); break;
        case 7: case 8: case 9: {   // Contrast / Brightness / Colors: 0 .. 100 % in steps of 5, round and round
            int& v = row == 7 ? settings_.video_contrast : row == 8 ? settings_.video_brightness : settings_.video_colors;
            v = (((std::clamp(v, 0, 100) / 5) + (dir < 0 ? 20 : 1)) % 21) * 5;
            set_config_value(row == 7 ? "VideoContrast" : row == 8 ? "VideoBrightness" : "VideoColors", std::to_string(v));
            status_line_ = std::string(row == 7 ? "video contrast: " : row == 8 ? "video brightness: " : "video colors: ") + std::to_string(v) + " %" + (row == 8 ? " (50 = as it is)" : "");
            video_pic_key_.clear();
            break;
        }
        case 10:
            settings_.video_chroma = (std::clamp(settings_.video_chroma, 0, kChromaLevels - 1) + (dir < 0 ? kChromaLevels - 1 : 1)) % kChromaLevels;
            set_config_value("VideoChroma", chroma_name(settings_.video_chroma));
            status_line_ = std::string("video chromatic aberration: ") + chroma_name(settings_.video_chroma);
            video_pic_key_.clear();
            break;
        case 11:
            settings_.video_res = (std::clamp(settings_.video_res, 0, 2) + (dir < 0 ? 2 : 1)) % 3;
            set_config_value("VideoResolution", video_res_name(settings_.video_res));
            status_line_ = "video resolution: " + video_res_label(settings_.video_res, gfx_proto_ == GfxProto::Sixel);
            video_pic_key_.clear();
            force_redraw_ = true;
            break;
        case 12: {
            int at = 0;
            for (int i = 0; i < 6; ++i) if (settings_.video_buffer >= kVideoBufferChoices[i]) at = i;
            at = (at + (dir < 0 ? 5 : 1)) % 6;
            settings_.video_buffer = kVideoBufferChoices[at];
            set_config_value("VideoBuffer", std::to_string(settings_.video_buffer));
            status_line_ = "video buffer: " + std::to_string(settings_.video_buffer) + " MB of decoded frames kept ahead";
            force_redraw_ = true;
            break;
        }
        case 13:   // Show Clap Loading: text note / clap / none
            settings_.video_clap = (std::clamp(settings_.video_clap, 0, 2) + (dir < 0 ? 2 : 1)) % 3;
            set_config_value("VideoClap", settings_.video_clap == 2 ? "none" : settings_.video_clap == 0 ? "text note" : "clap");
            status_line_ = settings_.video_clap == 1 ? "show clap loading: clap -- a small clapperboard shows while a video loads" : settings_.video_clap == 0 ? "show clap loading: text note -- the text \"video: starting ...\" shows while a video loads" : "show clap loading: none -- nothing shows while a video loads";
            video_pic_key_.clear();
            break;
        default: break;
    }
}

void App::video_set_size(int size) {
    settings_.video_size = std::clamp(size, 0, 3);
    set_config_value("VideoSize", settings_.video_size == 1 ? "cropped" : settings_.video_size == 2 ? "full" : settings_.video_size == 3 ? "framed" : "regular");
    video_pic_key_.clear();
    status_line_ = settings_.video_size == 1 ? "video size: cropped -- two columns are cut off each side of the picture"
                 : settings_.video_size == 2 ? "video size: full -- the picture fills the field (cropped to fit), the lyric line is a small box over it"
                 : settings_.video_size == 3 ? "video size: framed -- like full, but one row and two columns smaller on every side (an empty frame)"
                                             : "video size: regular -- the whole picture, the lyric line under it";
    force_redraw_ = true;
}

void App::video_set_fx(int fx) {
    settings_.video_fx = video_fx_canon(std::clamp(fx, 0, kVideoFxCount - 1));
    const std::string n = video_fx_key(settings_.video_fx);
    set_config_value("VideoFx", n);
    video_pic_key_.clear();   // (only the picture is made again: no screen clear, the cover behind the disk stays)
    status_line_ = std::string("video effect: ") + video_fx_name(settings_.video_fx);
}

void App::video_track_fx_step(int dir) {
    const int nx = video_fx_step(video_fx_for(current_path_), dir);
    VideoTrackOpts o = cover_track_video_opts(path_utf8(current_path_));
    o.fx = nx;   // always the effect of this track (the same one as in the Style Option overlay), never the general setting
    set_cover_track_video_opts(path_utf8(current_path_), o);
    video_pic_key_.clear();   // (only the picture is made again: the video keeps playing, the cover behind the disk stays, no screen clear)
    status_line_ = std::string("video effect of this track: ") + video_fx_name(nx);
}

void App::video_full_toggle() {
    if (audiobook_) { status_line_ = "video: not available in the audio book player"; return; }
    video_full_ = !video_full_;
    if (video_full_) { spectro_full_ = false; scope_big_ = false; video_pic_key_.clear(); }
    else video_pic_key_.clear();
    status_line_ = video_full_ ? "full screen video (ESC or " + hotkey_text("HKeyVideoFull", "v") + " closes, UP/DOWN change the effect, TAB the lyrics)" : "";
    force_redraw_ = true;
}

// The keys of the full screen video. true: handled. LEFT / RIGHT (seek), SPACE/p (pause), + / - ... stay what they are.
namespace {
int vopts_step(int cur, int dir, int max_value, int step) {   // -1 ("as in Settings") and the values 0 .. max_value
    const int count = max_value / step + 2;
    int idx = cur < 0 ? 0 : cur / step + 1;
    idx = ((idx + dir) % count + count) % count;
    return idx == 0 ? -1 : (idx - 1) * step;
}
}  // namespace

void App::video_opts_change(int row, int dir, bool reset) {
    VideoTrackOpts o = cover_track_video_opts(path_utf8(current_path_));
    if (row == 0) o.fx = reset ? -1 : video_fx_step(o.fx, dir, true);
    else if (row == 1) o.contrast = reset ? -1 : vopts_step(o.contrast, dir, 100, 5);
    else if (row == 2) o.brightness = reset ? -1 : vopts_step(o.brightness, dir, 100, 5);
    else if (row == 3) o.colors = reset ? -1 : vopts_step(o.colors, dir, 100, 5);
    else if (row == 4) o.chroma = reset ? -1 : vopts_step(o.chroma, dir, kChromaLevels - 1, 1);
    else if (row == 5) o.res = reset ? -1 : vopts_step(o.res, dir, 2, 1);
    else if (row == 6) o.round = reset ? -1 : vopts_step(o.round, dir, 1, 1);
    else {   // 7: Use mp4 audio for this song (a song paired with a video); it applies when the song starts the next time
        const int before = o.mp4audio;
        o.mp4audio = reset ? -1 : (o.mp4audio == 1 ? -1 : 1);
        set_cover_track_video_opts(path_utf8(current_path_), o);
        if (o.mp4audio != before) { reload_current_audio(); status_line_ = o.mp4audio == 1 ? "mp4 audio for this song: true -- the audio is loaded again, at the same time" : "mp4 audio for this song: false -- the audio is loaded again, at the same time"; }
    }
    set_cover_track_video_opts(path_utf8(current_path_), o);
    video_pic_key_.clear();
    if (row == 5) { video_for_track_.clear(); ++video_rev_; force_redraw_ = true; }   // (the resolution: the video is read again; the other rows only change the picture)
}

std::vector<std::string> App::build_video_opts_panel() const {
    const int W = kVideoOptsPanelWidth, inner = W - 2;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string bar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    const VideoTrackOpts o = cover_track_video_opts(path_utf8(current_path_));
    auto pct = [&](int own, int global) { return own >= 0 ? std::to_string(own) + " %" : std::string("as in Settings (") + std::to_string(global) + " %)"; };
    struct Row { const char* label; std::string value; };
    std::vector<Row> rows = {
        {"Video Effect", o.fx >= 0 ? std::string(video_fx_name(o.fx)) : std::string("VIDEO SETTINGS (") + video_fx_name(settings_.video_fx) + ")"},
        {"Video Contrast", pct(o.contrast, settings_.video_contrast)},
        {"Video Brightness", pct(o.brightness, settings_.video_brightness)},
        {"Video Colors", pct(o.colors, settings_.video_colors)},
        {"Video Chromatic Aberration", o.chroma >= 0 ? std::string(chroma_name(o.chroma)) : std::string("as in Settings (") + chroma_name(settings_.video_chroma) + ")"},
        {"Video Resolution", o.res >= 0 ? std::string(video_res_name(o.res)) : std::string("as in Settings (") + video_res_name(settings_.video_res) + ")"},
        {"Video Rounded Corners", o.round >= 0 ? std::string(o.round ? "on" : "off") : std::string("as in Settings (") + (settings_.video_round ? "on" : "off") + ")"},
    };
    if (video_opts_pair_) rows.push_back({"Use mp4 audio for this song", o.mp4audio == 1 ? std::string("true") : std::string("false")});
    std::vector<std::string> lines;
    lines.push_back(box_top("VIDEO OPTIONS  " + truncate_str(path_utf8(current_path_.filename()), 36), W, border));
    auto plain = [&](const std::string& t) { return bar + pad_right(truncate_str(t, inner), inner) + bar; };
    lines.push_back(plain(" Only for this track."));
    lines.push_back(plain(""));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        std::string t = " " + pad_right(rows[static_cast<size_t>(i)].label, 27) + " \u25c0 " + rows[static_cast<size_t>(i)].value + " \u25b6";
        t = pad_right(truncate_str(t, inner - 1), inner - 1) + " ";
        lines.push_back(bar + (i == video_opts_sel_ ? "\x1b[7m" + t + "\x1b[0m" : t) + bar);
    }
    lines.push_back(plain(""));
    lines.push_back(box_bottom(W, "[\u2191\u2193] select  [\u2190\u2192] change  [DEL/d] reset  [S / ESC] close", settings_.border_color_bottom.empty() ? border : ansi_for(settings_.border_color_bottom, false)));
    return lines;
}

bool App::video_full_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (video_opts_open_) {   // the options panel over the picture
        if (!arrow && (key == 27 || key == 's' || key == 'S')) { video_opts_open_ = false; video_pic_key_.clear(); force_redraw_ = true; return true; }   // (clearing the key sends the whole picture again: a paused video has no new frame, and the old pixels still have the panel's hole in them)
        if (arrow && key == 'A') { video_opts_sel_ = (video_opts_sel_ + (video_opts_pair_ ? 7 : 6)) % (video_opts_pair_ ? 8 : 7); return true; }
        if (arrow && key == 'B') { video_opts_sel_ = (video_opts_sel_ + 1) % (video_opts_pair_ ? 8 : 7); return true; }
        if (arrow && (key == 'C' || key == 'D')) { video_opts_change(video_opts_sel_, key == 'C' ? 1 : -1, false); return true; }
        if (!arrow && (key == '\r' || key == '\n' || key == ' ')) { video_opts_change(video_opts_sel_, 1, false); return true; }
        if (!arrow && (key == 'd' || key == 'r' || key == kKeyDelete || key == 127)) { video_opts_change(video_opts_sel_, 0, true); return true; }
        return true;   // (nothing else works under the panel)
    }
    if (!arrow && (key == 's' || key == 'S')) { video_opts_open_ = true; video_opts_sel_ = 0; video_opts_pair_ = song_paired_with_video(current_path_); force_redraw_ = true; return true; }
    if (!arrow && (key == 27 || key_is("HKeyVideoFull", key))) { video_full_ = false; video_opts_open_ = false; video_pic_key_.clear(); force_redraw_ = true; return true; }
    if (arrow && (key == 'A' || key == 'B') && last_key_was_shifted()) {   // SHIFT+UP / DOWN: the layout
        settings_.video_layout = (std::clamp(settings_.video_layout, 0, 3) + (key == 'B' ? 1 : 3)) % 4;
        set_config_value("VideoLayout", std::to_string(settings_.video_layout));
        video_pic_key_.clear();
        static const char* kN[4] = {"video", "video + oscilloscope", "video + spectrogram", "video + oscilloscope + spectrogram"};
        status_line_ = std::string("video layout: ") + kN[settings_.video_layout];
        force_redraw_ = true;
        return true;
    }
    if (!arrow && vl_has_osci(settings_.video_layout) && key_is("HKeyOscMenu", key)) { osci_menu_row_ = 0; mode_ = Mode::OsciMenu; force_redraw_ = true; return true; }
    if (!arrow && vl_has_spectro(settings_.video_layout) && key_is("HKeySpectroMenu", key)) { spectro_menu_row_ = kSrView; mode_ = Mode::SpectroMenu; force_redraw_ = true; return true; }
    if (!arrow && key == 'G') {   // grid and scales of the oscilloscope / spectrogram
        settings_.vl_scales = !settings_.vl_scales;
        set_config_value("VideoLayoutScales", settings_.vl_scales ? "true" : "false");
        video_pic_key_.clear();
        status_line_ = settings_.vl_scales ? "video layout: grid and scales on" : "video layout: grid and scales off";
        force_redraw_ = true;
        return true;
    }
    if (arrow && (key == 'A' || key == 'B')) {
        video_track_fx_step(key == 'B' ? 1 : -1);
        return true;
    }
    if (!arrow && key == 9) {   // TAB: the lyric line off -> on -> on with its translation
        bool& on = settings_.video_full_lyrics;
        bool& tr = settings_.video_full_translation;
        if (!on) { on = true; tr = false; status_line_ = "full screen video: the lyric line is shown"; }
        else if (!tr) {
            tr = true;
            vtr_key_.clear();   // (look again: a translation may have been saved since)
            const bool has_tr = [&] { std::lock_guard<std::mutex> lk(lyrics_mutex_); return !video_translation_lines().empty(); }();
            status_line_ = has_tr ? "full screen video: lyrics with their translation"
                                  : "full screen video: lyrics with translation - none saved for these lyrics yet (READ LYRICS, then t, translates)";
        } else { on = false; tr = false; status_line_ = "full screen video: no lyrics"; }
        set_config_value("VideoFullLyrics", on ? "true" : "false");
        set_config_value("VideoFullTranslation", tr ? "true" : "false");
        video_pic_key_.clear();
        force_redraw_ = true;
        return true;
    }
    return false;
}

void App::video_menu_key(int key) {
    const bool arrow = last_key_was_arrow();
    constexpr int kRows = 20;
    if (vdel_stage_ > 0) {   // a question is open over the overlay
        const bool tab = !arrow && key == '\t';
        if ((arrow && (key == 'C' || key == 'D')) || tab) { vdel_sel_ = 1 - vdel_sel_; return; }
        if (!arrow && (key == 'y' || key == 'Y')) { vdel_sel_ = 1; return; }
        if (!arrow && (key == 'n' || key == 'N')) { vdel_sel_ = 0; return; }
        if (!arrow && key == 27) { vdel_stage_ = 0; vdel_video_.clear(); status_line_ = "nothing deleted"; return; }
        if (!arrow && (key == '\r' || key == '\n')) {
            if (vdel_sel_ == 0) { status_line_ = vdel_stage_ == 3 ? "refetch cancelled" : "nothing deleted"; vdel_stage_ = 0; vdel_video_.clear(); return; }
            if (vdel_stage_ == 1) { vdel_stage_ = 2; vdel_sel_ = 0; return; }   // the second question
            if (vdel_stage_ == 2) {
                std::string msg;
                video_delete_file(vdel_video_, msg);
                status_line_ = msg;
                vdel_stage_ = 0;
                vdel_video_.clear();
                video_menu_close();
                return;
            }
            vdel_stage_ = 0;   // 3: refetch
            video_refetch_start();
            return;
        }
        return;
    }
    if (arrow && key == 'A') { vmenu_sel_ = (vmenu_sel_ + kRows - 1) % kRows; return; }
    if (arrow && key == 'B') { vmenu_sel_ = (vmenu_sel_ + 1) % kRows; return; }
    if (arrow && (key == 'C' || key == 'D')) { if (vmenu_sel_ <= 13) video_menu_change(vmenu_sel_, key == 'C' ? 1 : -1); return; }
    if (!arrow && (key == '\r' || key == '\n' || key == ' ')) {
        if (vmenu_sel_ <= 13) { video_menu_change(vmenu_sel_, 1); return; }
        if (vmenu_sel_ == 14) { video_menu_close(); video_full_toggle(); return; }
        if (vmenu_sel_ == 15) { video_menu_close(); video_link_open(has_track_ ? current_path_ : fs::path(), Mode::Browse); return; }
        if (vmenu_sel_ == 16) { video_menu_close(); video_search_open(); return; }
        if (vmenu_sel_ == 17) { video_menu_close(); video_fetch_open(); return; }
        if (vmenu_sel_ == 18 || vmenu_sel_ == 19) {   // Delete Current Video / Refetch this Video: a question first
            if (!has_track_ || current_path_.empty() || !current_is_local_) { status_line_ = "video: play a track from your library first"; return; }
            if (LocalSource::is_video_file(current_path_)) { status_line_ = "this track is a video file itself -- delete it in FETCH VIDEOS -> VIDEO FILES"; return; }
            const std::string v = video_source_for(current_path_);
            if (v.empty()) { status_line_ = "this track has no video"; return; }
            vdel_video_ = v;
            vdel_stage_ = vmenu_sel_ == 18 ? 1 : 3;
            vdel_sel_ = 0;
            return;
        }
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
    // true / false switches on the right in one column; the cover choice is a selector; the three overlays are buttons
    auto toggle = [&](bool on) { return std::string(on ? "[x] true " : "[ ] false"); };
    auto section = [&](const std::string& title) {
        const std::string head = "\u2500\u2500 " + title + " ";
        std::string rule = head;
        for (int w = display_width(head); w < inner; ++w) rule += settings_.box_horizontal;
        return bar + " " + border + rule + R + " " + bar;
    };
    auto button = [&](int idx, const std::string& label) {
        const std::string l = std::string(vmenu_sel_ == idx ? "> " : "  ") + "[ " + label + " ]";
        return row(l, vmenu_sel_ == idx);
    };
    std::vector<std::string> lines;
    lines.push_back(box_top("VIDEO SETTINGS", W, border));
    lines.push_back(section("Switches"));
    lines.push_back(row(entry(vmenu_sel_ == 0, "Use Video in Lyrics Field", toggle(settings_.video_mode)), vmenu_sel_ == 0));
    lines.push_back(row(entry(vmenu_sel_ == 1, "Include Lyrics", toggle(settings_.video_lyrics)), vmenu_sel_ == 1));
    lines.push_back(row(entry(vmenu_sel_ == 2, "Auto Video (else Lyrics/Viz Default)", toggle(settings_.video_auto)), vmenu_sel_ == 2));
    lines.push_back(row(entry(vmenu_sel_ == 3, "Cover, Disk or Thumbnail", std::string("< ") + (settings_.video_cover == 1 ? "thumbnail" : settings_.video_cover == 2 ? "disk" : "cover") + " >"), vmenu_sel_ == 3));
    lines.push_back(section("Picture"));
    lines.push_back(row(entry(vmenu_sel_ == 4, "Picture Size", std::string("< ") + (settings_.video_size == 1 ? "cropped" : settings_.video_size == 2 ? "full" : settings_.video_size == 3 ? "framed" : "regular") + " >"), vmenu_sel_ == 4));
    lines.push_back(row(entry(vmenu_sel_ == 5, "Rounded Corners", toggle(settings_.video_round)), vmenu_sel_ == 5));
    lines.push_back(row(entry(vmenu_sel_ == 6, "Effect", std::string("< ") + video_fx_name(settings_.video_fx) + " >"), vmenu_sel_ == 6));
    {
        auto pct = [&](int v) { return std::string("< ") + std::to_string(std::clamp(v, 0, 100)) + " % >"; };
        lines.push_back(row(entry(vmenu_sel_ == 7, "Contrast", pct(settings_.video_contrast)), vmenu_sel_ == 7));
        lines.push_back(row(entry(vmenu_sel_ == 8, "Brightness", pct(settings_.video_brightness)), vmenu_sel_ == 8));
        lines.push_back(row(entry(vmenu_sel_ == 9, "Colors", pct(settings_.video_colors)), vmenu_sel_ == 9));
        lines.push_back(row(entry(vmenu_sel_ == 10, "Chromatic Aberration", std::string("< ") + chroma_name(settings_.video_chroma) + " >"), vmenu_sel_ == 10));
        lines.push_back(row(entry(vmenu_sel_ == 11, "Resolution", std::string("< ") + video_res_label(settings_.video_res, gfx_proto_ == GfxProto::Sixel) + " >"), vmenu_sel_ == 11));
        lines.push_back(row(entry(vmenu_sel_ == 12, "Buffer", "< " + std::to_string(settings_.video_buffer) + " MB >"), vmenu_sel_ == 12));
    }
    lines.push_back(row(entry(vmenu_sel_ == 13, "Show Clap Loading", std::string("< ") + (settings_.video_clap == 2 ? "none" : settings_.video_clap == 0 ? "text note" : "clap") + " >"), vmenu_sel_ == 13));
    lines.push_back(button(14, "Full Screen Video"));
    lines.push_back(section("Videos for songs"));
    lines.push_back(button(15, "Manually Couple Local Files"));
    lines.push_back(button(16, "Quick Search/Down. YT"));
    lines.push_back(button(17, "General Fetch Videos Menu"));
    lines.push_back(section("This track's video"));
    lines.push_back(button(18, "Delete Current Video"));
    lines.push_back(button(19, "Refetch this Video"));
    lines.push_back(centred_bottom(settings_, W, "[UP/DOWN] [ENTER] [←→] change  [ESC] close", border_bottom));
    return lines;
}

// ======================================================================================================================
// VIDEO OPTION (connect a song and a video file)
// ======================================================================================================================

// Rows of the two lists and of the picture box: the panel (15 fixed rows + list + picture) takes what the terminal has.
static void vl_split_rows(int term_rows, int& list_rows, int& pic_rows) {
    const int avail = std::max(7, term_rows - 4 - 16);
    list_rows = std::clamp(avail * 3 / 5, 3, 16);
    pic_rows = std::clamp(avail - list_rows, 3, 14);
}
int App::video_link_list_rows() const { int l, p; vl_split_rows(term_rows_, l, p); return l; }

// Which songs have a video now (a connection by hand, a download, or a file of the same name beside them).
void App::video_link_refresh_flags() {
    // One pass instead of video_source_for() per song (that lists the song's folder on disk and walks the whole library each time: N x N
    // on a big library, which made the dialog slow to open). The library's videos are indexed by folder + name and by name once.
    auto low = [](const fs::path& p) { std::string x = path_utf8(p.stem()); for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return x; };
    std::unordered_set<std::string> by_folder, by_name;
    for (const VLEntry& v : vl_videos_) {
        const std::string n = low(v.path);
        by_name.insert(n);
        by_folder.insert(path_utf8(v.path.parent_path()) + "\x01" + n);
    }
    for (VLEntry& e : vl_tracks_) {
        const std::string tp = path_utf8(e.path);
        if (VideoLinks::get().is_none(tp)) { e.has_video = false; continue; }   // removed on purpose: no pairing by name either
        const std::string linked = VideoLinks::get().video_for(tp);
        if (!linked.empty()) {
            std::error_code lec;
            if (fs::exists(path_from_utf8(linked), lec)) { e.has_video = true; continue; }
        }
        const std::string n = low(e.path);
        e.has_video = by_folder.count(path_utf8(e.path.parent_path()) + "\x01" + n) > 0 || by_name.count(n) > 0;
    }
}

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
        if (vl_filter_ == 0 && e.has_video) continue;   // "no video yet"
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
        e.hay = vl_hay(with_latin_spelling(e.label) + " " + path_utf8(t.path.filename()), folder);
        if (e.video) vl_videos_.push_back(e);
        else vl_tracks_.push_back(e);   // (the song list holds audio files only: videos are on the right)
    }
    auto by_label = [](const VLEntry& a, const VLEntry& b) { return ascii_lower_str(a.label) < ascii_lower_str(b.label); };
    std::sort(vl_tracks_.begin(), vl_tracks_.end(), by_label);
    std::sort(vl_videos_.begin(), vl_videos_.end(), by_label);
    vl_tq_.clear(); vl_vq_.clear();
    vl_filter_ = 1;               // 0: audio without a video yet, 1: all audio files
    vdel_stage_ = 0;
    vl_tsel_ = vl_vsel_ = 0;
    vl_pane_ = 1;                 // the track is locked in already (the playing / hovered one): the video is what is missing
    vl_lock_track_ = target;
    vl_lock_video_.clear();
    vl_confirm_ = false;
    vl_warn_ = false;
    vl_msg_.clear();
    if (target.empty()) { vl_pane_ = 0; vl_msg_ = "pick the song first (ENTER locks it in)"; }
    video_link_refresh_flags();
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
    video_link_refresh_flags();
    video_link_rebuild();
    vl_msg_ = "connected: " + video_link_label(vl_lock_track_) + "  <->  " + path_utf8(vl_lock_video_.filename());
    log_event(vl_msg_);
    status_line_ = vl_msg_;
}

// [ Delete COMPLETE VIDEO FILE connected to track ]: two questions in their own panels (the second one is the last), both start on No.
void App::video_link_ask_delete(const std::string& video) {
    vdel_video_ = video;
    vdel_stage_ = 1;
    vdel_sel_ = 0;
}

void App::video_link_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (vdel_stage_ > 0) {   // a question is open over the overlay
        const bool tab = !arrow && key == '\t';
        if ((arrow && (key == 'C' || key == 'D')) || tab) { vdel_sel_ = 1 - vdel_sel_; return; }
        if (!arrow && (key == 'y' || key == 'Y')) { vdel_sel_ = 1; return; }
        if (!arrow && (key == 'n' || key == 'N')) { vdel_sel_ = 0; return; }
        if (!arrow && key == 27) { vdel_stage_ = 0; vdel_video_.clear(); vl_msg_ = "nothing deleted"; vl_warn_ = false; return; }
        if (!arrow && (key == '\r' || key == '\n')) {
            if (vdel_sel_ == 0) { vl_msg_ = "nothing deleted"; vl_warn_ = false; vdel_stage_ = 0; vdel_video_.clear(); return; }
            if (vdel_stage_ == 1) { vdel_stage_ = 2; vdel_sel_ = 0; return; }   // the second question
            std::string msg;
            const bool ok = video_delete_file(vdel_video_, msg);
            vl_msg_ = msg;
            vl_warn_ = !ok;
            if (ok) {
                const fs::path gone = path_from_utf8(vdel_video_);
                vl_videos_.erase(std::remove_if(vl_videos_.begin(), vl_videos_.end(), [&](const VLEntry& e) { return e.path == gone; }), vl_videos_.end());
                if (vl_lock_video_ == gone) vl_lock_video_.clear();
                video_link_refresh_flags();
                video_link_rebuild();
            }
            vdel_stage_ = 0;
            vdel_video_.clear();
            return;
        }
        return;
    }
    if (key == kKeyDelete || (!arrow && (key == '\r' || key == '\n') && vl_pane_ == 5)) {   // DEL / [ Delete Connection (Not the Video itself) ]: the connection of the locked-in song goes, the video file stays (the second press confirms)
        vl_warn_ = true;
        const std::string lt = vl_lock_track_.empty() ? std::string() : path_utf8(vl_lock_track_);
        if (lt.empty()) { vl_confirm_ = false; vl_msg_ = "no song locked in -- highlight one and press [ENTER]"; return; }
        const bool was_none = VideoLinks::get().is_none(lt);
        const bool has_now = !LocalSource::is_video_file(vl_lock_track_) && !video_source_for(vl_lock_track_).empty();
        if (!was_none && !has_now) { vl_confirm_ = false; vl_msg_ = "the locked-in song has no video -- nothing to remove"; return; }
        if (!vl_confirm_) {
            vl_confirm_ = true;
            vl_msg_ = was_none ? "this song is set to have no video. Press again to give the automatic pairing by file name back"
                               : "remove the connection of this song (the video file stays)? Press again to confirm";
            return;
        }
        if (was_none) VideoLinks::get().remove(lt);          // the mark goes: the automatic pairing is back
        else VideoLinks::get().mark_none(lt);                // no video, also not by file name
        ++video_rev_;
        video_for_track_.clear();
        vl_confirm_ = false;
        vl_warn_ = false;
        video_link_refresh_flags();
        video_link_rebuild();
        if (was_none) {   // the automatic pairing is back: the song stays locked in with the video it has now
            video_link_show_current(vl_lock_track_);
            vl_msg_ = "the automatic pairing by file name is back";
        } else {          // the song is disconnected: it leaves the locked-in boxes (and the video that was its)
            const std::string name = video_link_label(vl_lock_track_);
            vl_lock_track_.clear();
            vl_lock_video_.clear();
            vl_pane_ = 0;
            vl_msg_ = "connection removed: " + name + " has no video now (the video file stays)";
        }
        return;
    }
    vl_confirm_ = false;
    vl_del_confirm_ = false;
    if (key == 27) { video_link_close(); return; }
    if (!arrow && key == '\t') { vl_pane_ = (vl_pane_ + 1) % 6; vl_msg_.clear(); return; }
    if ((key == '\r' || key == '\n') && vl_pane_ == 4) {   // [ Delete COMPLETE VIDEO FILE connected to track ]: the video file of the locked-in song goes from the disk (two questions first)
        vl_warn_ = true;
        if (vl_lock_track_.empty()) { vl_msg_ = "no song locked in -- highlight one and press [ENTER]"; return; }
        if (LocalSource::is_video_file(vl_lock_track_)) { vl_msg_ = "the locked-in file is a video itself -- delete it in FETCH VIDEOS -> VIDEO FILES"; return; }
        const std::string video = video_source_for(vl_lock_track_);
        if (video.empty()) { vl_msg_ = "the locked-in song has no video -- nothing to delete"; return; }
        vl_warn_ = false;
        vl_msg_.clear();
        video_link_ask_delete(video);
        return;
    }
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
    if (key_is("HKeyVideoFetch", key) && !(key >= 32 && key < 127)) {   // ALT+v (the key of the FETCH VIDEOS overlay itself; CTRL+v pastes into the search fields): FETCH VIDEOS with the cursor on this song (the locked-in one, else the one under the cursor)
        fs::path song = vl_lock_track_;
        if (song.empty() && !vl_tview_.empty()) song = vl_tracks_[static_cast<size_t>(vl_tview_[static_cast<size_t>(std::clamp(vl_tsel_, 0, static_cast<int>(vl_tview_.size()) - 1))])].path;
        if (song.empty() || LocalSource::is_video_file(song)) { vl_msg_ = "choose a song (audio file) first -- [ALT+v] fetches videos for it"; vl_warn_ = true; return; }
        video_link_close();
        video_fetch_open(song);
        return;
    }
    if (key == kKeyCtrlF) {   // the song list: audio files without a video yet / all audio files
        vl_filter_ = (vl_filter_ + 1) % 2;
        vl_tsel_ = 0;
        static const char* const names[2] = {"audio files that have no video yet", "all audio files (those with a video are highlighted)"};
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
    const int PW = std::clamp(W - 2, 70, 150);
    int list_rows_n = 3, pic_rows = 3;
    vl_split_rows(term_rows_, list_rows_n, pic_rows);
    vl_pic_ = 0;
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
    both(box_top(title(0, "SEARCH LOCAL SONGS"), pw_l, border), box_top(title(1, "SEARCH LOCAL VIDEOS"), pw_r, border));
    both(search_line(0, vl_tq_, pw_l), search_line(1, vl_vq_, pw_r));
    both(box_bottom(pw_l, "", border), box_bottom(pw_r, "", border));
    const int list_rows = list_rows_n;
    const int nt = static_cast<int>(vl_tview_.size());
    const int nv = static_cast<int>(vl_vview_.size());
    auto cell = [&](int which, int w, int r, int sel, int n, const std::function<std::string(int)>& text, const std::function<bool(int)>& locked, const std::string& empty_text, const std::function<bool(int)>& marked = nullptr) {
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
            else if (marked && marked(i)) painted = leg + line + R;   // (a song with a connected video)
            else painted = line;
            return pline(painted, cw, w);
        }
        if (r == 0 && n == 0) return pline(pad_right(truncate_str(empty_text, cw), cw), cw, w);
        return pline(std::string(), 0, w);
    };
    static const char* const fnames[2] = {"no video yet", "all audio files"};
    both(box_top(title(0, std::string("SONGS (") + fnames[vl_filter_] + ")"), pw_l, border), box_top(title(1, "VIDEO FILES"), pw_r, border));
    for (int r = 0; r < list_rows; ++r) {
        const std::string l = cell(0, pw_l, r, vl_tsel_, nt, [&](int i) {
            const VLEntry& e = vl_tracks_[static_cast<size_t>(vl_tview_[static_cast<size_t>(i)])];
            return std::string(e.has_video ? "[+] " : "") + apply_font_map(e.label, settings_.font_map);
        }, [&](int i) { return vl_tracks_[static_cast<size_t>(vl_tview_[static_cast<size_t>(i)])].path == vl_lock_track_; }, vl_tracks_.empty() ? "no audio files found in the library folders" : vl_filter_ == 0 ? "every song has a video already (or nothing matches)" : "no match",
        [&](int i) { return vl_tracks_[static_cast<size_t>(vl_tview_[static_cast<size_t>(i)])].has_video; });
        const std::string rr = cell(1, pw_r, r, vl_vsel_, nv, [&](int i) {
            const VLEntry& e = vl_videos_[static_cast<size_t>(vl_vview_[static_cast<size_t>(i)])];
            return apply_font_map(e.label, settings_.font_map) + (e.folder.empty() ? std::string() : "  (" + apply_font_map(e.folder, settings_.font_map) + ")");
        }, [&](int i) { return vl_videos_[static_cast<size_t>(vl_vview_[static_cast<size_t>(i)])].path == vl_lock_video_; },
        vl_videos_.empty() ? "no video files found in the library folders" : "no match");
        both(l, rr);
    }
    {
        const auto l = cut(std::to_string(nt) + " of " + std::to_string(vl_tracks_.size()) + " audio files   [+] highlighted: has a video", pw_l, leg);
        const auto r = cut(std::to_string(nv) + " of " + std::to_string(vl_videos_.size()) + " video files", pw_r, leg);
        both(pline(l.first, l.second, pw_l), pline(r.first, r.second, pw_r));
    }
    both(box_bottom(pw_l, "", border), box_bottom(pw_r, "", border));
    // the video that is shown on the right: the locked-in one, else the one the locked-in song has now
    std::string prev_video;
    bool prev_is_locked = false;
    if (!vl_lock_video_.empty()) { prev_video = path_utf8(vl_lock_video_); prev_is_locked = true; }
    else if (!vl_lock_track_.empty() && !LocalSource::is_video_file(vl_lock_track_)) prev_video = video_source_for(vl_lock_track_);
    both(box_top("LOCKED IN: SONG", pw_l, border), box_top(prev_is_locked || prev_video.empty() ? "LOCKED IN: VIDEO" : "CONNECTED VIDEO", pw_r, border));
    {
        const bool has_t = !vl_lock_track_.empty(), has_v = !vl_lock_video_.empty();
        const auto l1 = mq(2, has_t ? apply_font_map(video_link_label(vl_lock_track_), settings_.font_map) : "(none yet -- highlight a song, press [ENTER])", pw_l, has_t ? hdr : std::string());
        const auto r1 = mq(3, has_v ? apply_font_map(path_utf8(vl_lock_video_.filename()), settings_.font_map) : "(none yet -- highlight a video, press [ENTER])", pw_r, has_v ? hdr : std::string());
        both(pline(l1.first, l1.second, pw_l), pline(r1.first, r1.second, pw_r));
        std::string now;   // what the locked-in song has now
        if (has_t) {
            const std::string linked = VideoLinks::get().video_for(path_utf8(vl_lock_track_));
            std::error_code ec;
            if (!linked.empty()) {
                const std::string dl = path_utf8(video_down_dir());
                const bool from_dl = !dl.empty() && linked.compare(0, dl.size(), dl) == 0;
                now = std::string(from_dl ? "paired (download): " : "paired (by hand): ") + apply_font_map(path_utf8(path_from_utf8(linked).filename()), settings_.font_map) + (fs::exists(path_from_utf8(linked), ec) ? "" : " (file is gone)");
            }
            else {
                const std::string auto_v = video_source_for(vl_lock_track_);
                now = auto_v.empty() ? "no video yet" : "paired (same name): " + apply_font_map(path_utf8(path_from_utf8(auto_v).filename()), settings_.font_map);
            }
        }
        const auto l2 = mq(4, now, pw_l, leg);
        const auto r2 = mq(5, has_v ? apply_font_map(path_utf8(vl_lock_video_.parent_path()), settings_.font_map) : std::string(), pw_r, leg);
        both(pline(l2.first, l2.second, pw_l), pline(r2.first, r2.second, pw_r));
        // the preview of the video: a real picture (Kitty / Sixel) in the right box, a note when there is none
        const int panel_h = 16 + list_rows + pic_rows;
        const int start_col = 1 + std::max(0, (W - PW) / 2);
        const int start_row = std::clamp(1 + std::max(0, (term_rows_ - panel_h) / 2 - kFloatingPanelUpShift), 1, std::max(1, term_rows_ - panel_h));
        const int pic_w = pw_r - 4;
        const int pic_line = static_cast<int>(out.size());   // (0-based line of the panel where the picture starts)
        bool as_picture = false;
        if (!prev_video.empty() && gfx_proto_ != GfxProto::None && pic_w >= 8) {
            const bool second = gfx_.active;   // the main screen's cover stays (the panel covers part of it): the preview takes the second slot
            if (cover_full_picture_img(preview_video_frame(prev_video), pic_w, pic_rows, (start_col - 1) + 1 + pw_l + 1 + 2, (start_row - 1) + pic_line, second)) {
                vl_pic_ = second ? 2 : 1;
                as_picture = true;
            }
        }
        const std::string note = prev_video.empty() ? (vl_lock_track_.empty() ? "(lock in a song to see its video)" : "(this song has no video)")
                               : preview_video_frame_loading(prev_video) ? "reading a frame ..." : gfx_proto_ == GfxProto::None ? "(no picture in this terminal)" : "(the picture cannot be read)";
        for (int r = 0; r < pic_rows; ++r) {
            std::string lt;
            int lu = 0;
            if (r == 0 && !vl_lock_track_.empty() && !LocalSource::is_video_file(vl_lock_track_)) {
                const std::string t = truncate_str(prev_video.empty() ? "no video at the moment" : "has a video: " + apply_font_map(path_utf8(path_from_utf8(prev_video).filename()), settings_.font_map), pw_l - 4);
                lt = leg + t + R; lu = display_width(t);
            }
            std::string rt;
            int ru = 0;
            if (as_picture) { rt = gfx_hole(pic_w); ru = pic_w; }
            else if (r == pic_rows / 2) {
                const std::string t = truncate_str(note, pic_w);
                const int lead = std::max(0, (pic_w - display_width(t)) / 2);
                rt = std::string(static_cast<size_t>(lead), ' ') + leg + t + R; ru = lead + display_width(t);
            }
            both(pline(lt, lu, pw_l), pline(rt, ru, pw_r));
        }
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
    {   // the second button row: the video file of the locked-in song can be deleted
        const std::string b3 = "[ Delete COMPLETE VIDEO FILE connected to track ]";
        const int inner = PW - 4, w3 = display_width(b3), lead = std::max(0, (inner - w3) / 2);
        const std::string p3 = vl_pane_ == 4 ? "\x1b[7m" + b3 + R : hdr + b3 + R;
        out.push_back(bar + " " + std::string(static_cast<size_t>(lead), ' ') + p3 + std::string(static_cast<size_t>(std::max(0, inner - lead - w3)), ' ') + " " + bar);
    }
    {   // the third button row: only the connection of the locked-in song goes, the video file stays
        const std::string b4 = "[ Delete Connection (Not the Video itself) ]";
        const int inner = PW - 4, w4 = display_width(b4), lead = std::max(0, (inner - w4) / 2);
        const std::string p4 = vl_pane_ == 5 ? "\x1b[7m" + b4 + R : hdr + b4 + R;
        out.push_back(bar + " " + std::string(static_cast<size_t>(lead), ' ') + p4 + std::string(static_cast<size_t>(std::max(0, inner - lead - w4)), ' ') + " " + bar);
    }
    out.push_back(bar + " " + (vl_warn_ && !vl_msg_.empty() ? std::string("\x1b[31m") : leg) + pad_right(truncate_str(vl_msg_, PW - 4), PW - 4) + R + " " + bar);
    out.push_back(centred_bottom(settings_, PW, "[TAB] pane  [ENTER] lock in  [CTRL+f] filter  [ALT+v] fetch videos  [DEL] remove connection  [ESC] close", border_bottom));
    return out;
}

// The questions over the VIDEO overlay: "Delete Current Video" asks twice (stage 1, then 2), "Refetch this Video" once (stage 3).
std::vector<std::string> App::build_video_delete_panel() const {
    const int W = 64, inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m", RED = "\x1b[31m";
    const std::string bar = border + settings_.box_vertical + R;
    auto line = [&](const std::string& content, int used) { return bar + " " + content + std::string(static_cast<size_t>(std::max(0, inner - used)), ' ') + " " + bar; };
    const std::string name = path_utf8(path_from_utf8(vdel_video_).filename());
    const size_t users = VideoLinks::get().tracks_for(vdel_video_).size();
    std::vector<std::string> text;
    std::string title, yes, no;
    if (vdel_stage_ == 1) {
        title = "DELETE THE VIDEO?";
        text = {name, "", "This deletes the video file from your disk. Until Mousiki closes it can be brought back with [u] in FETCH VIDEOS -> VIDEO FILES."};
        if (users > 1) text.push_back("It is used by " + std::to_string(users) + " songs: all of them lose their video.");
        yes = "Yes, go on"; no = "No, keep it";
    } else if (vdel_stage_ == 2) {
        title = "ARE YOU REALLY SURE?";
        text = {name, "", "Last question: the file leaves your library. Mousiki keeps it aside until you close the program: [u] in FETCH VIDEOS -> VIDEO FILES brings it back."};
        yes = "Delete for good"; no = "No, keep it";
    } else {
        title = "REFETCH THIS VIDEO?";
        const bool dl = video_is_download(vdel_video_);
        text = {name, "", "Mousiki searches the video again, takes the closest hit and downloads it for this song."};
        text.push_back(dl ? "The old file was downloaded by Mousiki (yt-dlp): it is deleted once the new one has arrived."
                          : "The old file is from your own folders: it is NOT deleted, only replaced for this song.");
        yes = "Yes, refetch"; no = "No, cancel";
    }
    std::vector<std::string> out;
    out.push_back(box_top(title, W, border));
    out.push_back(line("", 0));
    for (const std::string& t : text)
        for (const std::string& w : wrap_words(t, inner)) {
            const std::string c = truncate_str(w, inner);
            out.push_back(line((vdel_stage_ == 2 && &t == &text.front() ? RED : std::string()) + c + R, display_width(c)));
        }
    out.push_back(line("", 0));
    {
        const std::string b1 = "[ " + no + " ]", b2 = "[ " + yes + " ]";
        const int w1 = display_width(b1), w2 = display_width(b2), gap = 4;
        const int lead = std::max(0, (inner - w1 - gap - w2) / 2);
        out.push_back(line(std::string(static_cast<size_t>(lead), ' ') + (vdel_sel_ == 0 ? HI + b1 + R : b1) + std::string(static_cast<size_t>(gap), ' ') + (vdel_sel_ == 1 ? HI + b2 + R : b2), lead + w1 + gap + w2));
    }
    out.push_back(centred_bottom(settings_, W, "[←→] choose  [ENTER] confirm  [ESC] cancel", border_bottom));
    return out;
}

// [ Refetch this Video ]: FETCH VIDEOS opens on this song and searches again by itself; the closest hit is chosen and downloaded without
// further questions. The old video stays connected until the new one has arrived (the new connection replaces it); a failed download leaves
// everything as it was. A downloaded old file (video_down folder / the download list) is deleted afterwards, a file from the user's own
// folders never.
void App::video_refetch_start() {
    const fs::path song = current_path_;
    const std::string p = path_utf8(song);
    const std::string old = video_source_for(song);
    video_menu_close();
    video_fetch_open(song);
    if (!old.empty() && video_is_download(old)) vf_replace_[p] = old;
    else vf_replace_.erase(p);
    for (VFEntry& e : vf_all_) {
        if (path_utf8(e.path) != p) continue;
        e.refetch = true;
        e.searched = false; e.searching = false; e.queued = false; e.cands.clear(); e.choice = -1;
    }
    vf_auto_ = p;
    video_fetch_enqueue(p, true);
    vf_msg_ = "refetch: searching the video again, the closest hit is downloaded by itself ...";
    vf_warn_ = false;
    force_redraw_ = true;
}

bool App::video_is_download(const std::string& file) const { return cache_.is_video_download_file(path_from_utf8(file)); }

} // namespace muisc
