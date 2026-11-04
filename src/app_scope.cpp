// app_scope.cpp
// Part of class App (src/app.h): The oscilloscope / spectrogram windows and full screens, and their option panels.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "book_art.h"
#include "keyboard_layout.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "console_log.h"
#include "karaoke_font.h"
#include "glyph_raster.h"
#include "chiptune.h"
#include "scope_big.h"
#include "scope_window.h"
#include "wizard.h"
#include "creator.h"
#include "mic_capture.h"
#include "process_util.h"
#include "app_internal.h"
#include "update_check.h"
#include "open_url.h"
#include "app_settings_tables.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <sys/types.h>
#include <sys/stat.h>   // file_added_time(): stat() / _wstat64()
#if defined(_WIN32)
#include "win_compat.h"
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace muisc {


// ---- Scope window (SHIFT+9) -------------------------------------------------------------------------------------
void App::scope_window_toggle() {
    if (scope_window_running()) {
        scope_window_close();
        scope_win_on_ = false;
        status_line_ = "scope window closed";
        return;
    }
    std::string err;
    scope_win_on_ = true;
    scope_window_sync(false);   // the settings are there before the first sample
    if (!scope_window_open(&err)) {
        scope_win_on_ = false;
        status_line_ = err;
        return;
    }
    status_line_ = "scope window opened: F / double-click fullscreen, T always on top, ESC or " + hotkey_text("HKeyScopeWindow", ")") + " closes";
}

void App::scope_window_sync(bool check_alive) {
    if (!scope_win_on_ && check_alive && scope_window_running()) scope_win_on_ = true;   // opened in the radio
    if (!scope_win_on_) return;
    if (check_alive && !scope_window_running()) {   // closed from its side (its X, ESC, q) or failed
        scope_win_on_ = false;
        const std::string e = scope_window_take_error();
        status_line_ = e.empty() ? "scope window closed" : e;
        return;
    }
    // The style set in the oscilloscope overlay (SHIFT+o) decides what the window shows: the image style is a smooth pixel
    // picture (Decay, Glow, Z axis, rotation, colours ...), the braille style is the terminal's dot grid scaled up.
    const bool braille = settings_.osci_style == 0;
    const OsciSet& o = settings_.osci_set[braille ? 0 : 1];
    ScopeWinConfig c;
    c.braille = braille ? 1 : 0;
    c.mode = static_cast<uint8_t>(std::clamp(settings_.osci_mode, 0, 4));
    c.dot_threshold = o.dot_threshold;
    c.tail = o.tail;
    c.trace = o.trace;
    c.decay = o.decay;
    c.glow = o.glow;
    c.z_depth = o.z_depth;
    c.z = o.z ? 1 : 0;
    c.z_source = static_cast<uint8_t>(o.z_source == 1 ? 1 : 0);
    c.rotate = o.rotate ? 1 : 0;
    c.mono_phase = o.mono_phase ? 1 : 0;
    c.interp = o.interp ? 1 : 0;
    c.color_by_x = o.palette == 0 ? 1 : 0;
    c.music = o.music ? 1 : 0;
    c.crt = o.crt ? 1 : 0;
    c.low_rate = o.low_rate ? 1 : 0;
    c.dwell = o.dwell ? 1 : 0;
    c.rough = o.rough ? 1 : 0;
    for (int h = 0; h < 256; ++h) {
        int r = 255, g = 255, b = 255;
        osci_palette_rgb(settings_, o.palette, static_cast<float>(h) / 255.0f, r, g, b);
        c.pal[h][0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
        c.pal[h][1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
        c.pal[h][2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
    }
    std::string title;
    if (has_track_) { const TrackMetadata dm = display_metadata(); title = dm.artist.empty() ? dm.name : dm.artist + " - " + dm.name; }
    std::snprintf(c.title, sizeof c.title, "%s", title.c_str());
    scope_window_set_config(c);
}

// ---- Spectrogram window (SHIFT+8) --------------------------------------------------------------------------------
void App::spectro_window_toggle() {
    if (spectro_window_running()) {
        spectro_window_close();
        spectro_win_on_ = false;
        status_line_ = "spectrogram window closed";
        return;
    }
    std::string err;
    spectro_win_on_ = true;
    spectro_window_sync(false);   // the settings are there before the first sample
    if (!spectro_window_open(&err)) {
        spectro_win_on_ = false;
        status_line_ = err;
        return;
    }
    status_line_ = "spectrogram window opened: F / double-click fullscreen, T always on top, ESC or " + hotkey_text("HKeySpectroWindow", "(") + " closes";
}

void App::spectro_window_sync(bool check_alive) {
    if (!spectro_win_on_ && check_alive && spectro_window_running()) spectro_win_on_ = true;   // opened in the radio
    if (!spectro_win_on_) return;
    if (check_alive && !spectro_window_running()) {   // closed from its side (its X, ESC, q) or failed
        spectro_win_on_ = false;
        const std::string e = spectro_window_take_error();
        status_line_ = e.empty() ? "spectrogram window closed" : e;
        return;
    }
    // the SHIFT+i options and the colours of the scheme in use (the gradient one from the VIZ / OSCI colours)
    SpectroWinConfig c;
    spectro_win_pack(settings_.spectro, c);
    spectro_gradient_from(settings_);
    const auto& pal = spectro_palette_view(settings_.spectro);   // (the waterfall has black for its background / outline, and its own steps in the high resolution)
    for (int h = 0; h < 256; ++h) for (int k = 0; k < 3; ++k) c.pal[h][k] = pal[static_cast<size_t>(h)][static_cast<size_t>(k)];
    std::string title;
    if (has_track_) { const TrackMetadata dm = display_metadata(); title = dm.artist.empty() ? dm.name : dm.artist + " - " + dm.name; }
    std::snprintf(c.title, sizeof c.title, "%s", title.c_str());
    spectro_window_set_config(c);
}

// The switches of the Viz/Lyrics field depend on each other. This brings them in line, so no combination is left
// contradicting itself (called after every edit of such a row with its id, and every frame without one):
//   * an explicit pick wins: choosing a view (Lyrics/Viz, When No Lyrics Show, Lyrics Engine) that is switched off turns its
//     "Use ..." switch on;
//   * a switch that is off takes its dependents with it, instead of being turned on again: "Use Lyrics" off turns the Lyrics
//     Engine off; "Use Oscilloscope / Spectrogram / Sphere" off moves Lyrics/Viz and When No Lyrics Show to the next visual that
//     is on (When No Lyrics Show to "none" when there is none; Lyrics/Viz to the lyrics when they are all that is left).
// Returns a sentence about what was changed ("" = nothing), for the status line.
std::string App::enforce_visual_switches(int changed) {
    static const char* const names[4] = {"sphere", "oscilloscope", "spectrogram", "lyrics"};
    static const char* const use_rows[4] = {"Use Sphere", "Use Oscilloscope", "Use Spectrogram", "Use Lyrics"};
    Settings& s = settings_;
    std::string note;
    auto add = [&](const std::string& t) { note += (note.empty() ? "" : "; ") + t; };
    auto set_use = [&](int v, bool on) { (v == 0 ? s.use_sphere : v == 1 ? s.use_osci : v == 2 ? s.use_spectro : s.use_lyrics) = on; };
    // 1. explicit picks
    if (changed == kSidLyricViz && s.viz_default <= 3 && !s.viz_usable(s.viz_default)) { set_use(s.viz_default, true); add(std::string(use_rows[s.viz_default]) + " turned on"); }
    if (changed == kSidNoLyricsViz && s.nolyrics_viz <= 2 && !s.viz_usable(s.nolyrics_viz)) { set_use(s.nolyrics_viz, true); add(std::string(use_rows[s.nolyrics_viz]) + " turned on"); }
    if (changed == kSidNoVideoViz && s.novideo_viz <= 2 && !s.viz_usable(s.novideo_viz)) { set_use(s.novideo_viz, true); add(std::string(use_rows[s.novideo_viz]) + " turned on"); }
    // 2. what a switch that is off takes with it
    if (!s.use_lyrics && s.element_lyrics) { s.element_lyrics = false; add("the field no longer shows the lyrics"); }
    if (s.viz_default <= 3 && !s.viz_usable(s.viz_default)) {
        const int n = s.viz_step(s.viz_default, 1, false);
        if (n != s.viz_default) { s.viz_default = n; add(std::string("Lyrics/Viz Default -> ") + kVizNames[n]); }
    }
    if (s.nolyrics_viz <= 2 && !s.viz_usable(s.nolyrics_viz)) {
        const int n = s.viz_step(s.nolyrics_viz, 1, true);
        s.nolyrics_viz = n != s.nolyrics_viz ? n : 3;
        add(std::string("When No Lyrics Show -> ") + (s.nolyrics_viz == 3 ? "none" : names[s.nolyrics_viz]));
    }
    if (s.novideo_viz <= 2 && !s.viz_usable(s.novideo_viz)) {
        s.novideo_viz = 4;
        add("When No Video Shows -> lyrics");
    }
    if (s.lyric_viz <= 2 && !s.viz_usable(s.lyric_viz)) {
        const int n = s.viz_step(s.lyric_viz, 1, true);
        if (n != s.lyric_viz) { s.lyric_viz = n; add(std::string("Lyrics/Viz -> ") + names[n]); }
        else if (s.use_lyrics && !s.element_lyrics) { s.element_lyrics = true; add("Lyrics Engine turned on (no visual is left)"); }
    }
    return note;
}

void App::lyrics_view_nudge() {
    if (!has_track_ || !settings_.lyrics_on()) return;
    const std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
    last_lyrics_status_.clear();   // a fresh caption window, not a leftover from before the lyrics were off
    launch_lyrics_fetch(metadata_.name, artist, current_path_);
}

// ---- Spectrogram (SHIFT+i options, SHIFT+u full screen) ----------------------------------------------------------
void App::spectro_menu_adjust(int dir) {
    const int row = std::clamp(spectro_menu_row_, static_cast<int>(kSrView), kSpectroRowCount - 1);
    if (row == kSrDisplay) {   // what the lyrics area shows: sphere -> osci -> spectro
        settings_.lyric_viz = settings_.viz_step(settings_.lyric_viz, dir > 0 ? 1 : -1, true);
        if (settings_.element_lyrics && settings_.lyric_viz == 2) status_line_ = "the spectrogram shows when there are no lyrics (. switches)";
        return;
    }
    spectro_adjust(settings_.spectro, row, dir);
    settings_.spectro_sync();   // another view / style: its own values come in
}

std::vector<std::string> App::build_spectro_menu_panel() const {
    const int W = kSpectroMenuPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    std::vector<std::string> lines;
    lines.push_back(box_top("Spectrogram", W, border));
    for (const int r : spectro_visible_rows(settings_.spectro)) {
        const bool sel = r == spectro_menu_row_;
        std::string val = spectro_row_value(settings_.spectro, r);
        if (r == kSrStyle && settings_.spectro.style == 1 && gfx_proto_ == GfxProto::None) val = "image (no Sixel/Kitty here)";
        lines.push_back(row(std::string(sel ? "> " : "  ") + pad_right(spectro_row_label(r), 22) + pad_left(val, inner - 24), sel));
    }
    lines.push_back(row("", false));
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return bar + " " + border_bottom + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, inner - left - display_width(tx))), ' ') + "\x1b[0m" + " " + bar;
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[UP/DOWN] select  [LEFT/RIGHT] change", "[R] Audacity defaults  [" + hotkey_text("HKeySpectroFull", "U") + "] full screen", "[" + hotkey_text("HKeySpectroMenu", "I") + " / ESC] close"}, leg_in, leg_bot);
    return lines;
}

// The full-screen spectrogram: a frame with the track as its title, the frequency labels on the left (like
// Audacity's ruler), a time ruler on top and the picture (or braille) filling the rest.
std::string App::build_spectro_full(int W, const char* clear_prefix) const {
    spectro_used_ = true;
    spectro_gradient_from(settings_);
    SpectroAnalyzer& a = spectro();
    a.update(settings_.spectro);
    const SpectroSettings& sp = settings_.spectro;
    const int H = std::max(6, term_rows_ - 1);
    const int label_w = 6;
    const int cols = std::max(4, W - 2 - label_w - 1);
    const int rows = std::max(2, H - 3);
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string legend = ansi_for(settings_.legend_color, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const bool image = sp.style == 1 && gfx_proto_ != GfxProto::None;
    int cw = 10, chh = 20;
    if (image) gfx_cell_pixels(settings_.cell_pixels, cw, chh);
    const int unit = image ? std::max(1, chh) : 4;
    const bool wf = sp.view == 1;
    const auto labels = wf ? std::vector<std::pair<int, std::string>>() : spectro_axis(sp, a.rate(), rows * unit, unit);
    std::vector<std::string> lab(static_cast<size_t>(rows));
    for (const auto& l : labels) if (l.first >= 0 && l.first < rows) lab[static_cast<size_t>(l.first)] = l.second;
    std::string title = wf ? "WATERFALL" : "SPECTROGRAM";
    if (has_track_) { const TrackMetadata dm = display_metadata(); title += " - " + (dm.artist.empty() || dm.artist == "-" ? dm.name : dm.artist + " - " + dm.name); }
    std::ostringstream f;
    f << clear_prefix << "\x1b[?25l";
    f << box_top(truncate_str(title, W - 6), W, border) << "\n";
    if (wf) {   // the waterfall has no time axis: the frequencies of the front graph (in Hz, "k" = thousand) take the ruler's row
        std::string ruler(static_cast<size_t>(cols), ' ');
        if (sp.labels)
            for (const auto& l : spectro_axis_x(sp, a.rate(), cols * (image ? cw : 2), image ? cw : 2))
                for (size_t k = 0; k < l.second.size() && static_cast<size_t>(l.first) + k < ruler.size(); ++k) ruler[static_cast<size_t>(l.first) + k] = l.second[k];
        f << bar << legend << std::string(static_cast<size_t>(label_w + 1), ' ') << ruler << R << bar << "\n";
    } else {   // time ruler: seconds of the page (sweep) or seconds before now (scroll)
        std::string ruler(static_cast<size_t>(cols), ' ');
        const int step = sp.span <= 5 ? 1 : sp.span <= 10 ? 2 : sp.span <= 30 ? 5 : 10;
        for (int t = 0; t <= sp.span; t += step) {
            const int x = static_cast<int>(static_cast<long long>(t) * (cols - 1) / std::max(1, sp.span));
            const std::string txt = sp.motion == 1 ? (t == sp.span ? "now" : "-" + std::to_string(sp.span - t) + "s") : std::to_string(t) + "s";
            const int x0 = std::clamp(x - (t == 0 ? 0 : t == sp.span ? static_cast<int>(txt.size()) - 1 : static_cast<int>(txt.size()) / 2), 0, std::max(0, cols - static_cast<int>(txt.size())));
            for (size_t k = 0; k < txt.size() && x0 + static_cast<int>(k) < cols; ++k) ruler[static_cast<size_t>(x0) + k] = txt[k];
        }
        f << bar << legend << std::string(static_cast<size_t>(label_w + 1), ' ') << ruler << R << bar << "\n";
    }
    std::vector<std::string> body;
    if (image) {
        spectro_area_[0] = 2;
        spectro_area_[1] = 1 + label_w + 1;
        spectro_area_[2] = cols;
        spectro_area_[3] = rows;
    } else body = a.braille(sp, cols, rows);
    for (int r = 0; r < rows; ++r) {
        const std::string l = sp.labels ? lab[static_cast<size_t>(r)] : std::string();
        f << bar << legend << pad_left(l, label_w) << R << " ";
        if (image) f << gfx_hole(cols);
        else f << (r < static_cast<int>(body.size()) ? body[static_cast<size_t>(r)] : std::string(static_cast<size_t>(cols), ' '));
        f << bar << "\n";
    }
    f << box_bottom(W, "[" + hotkey_text("HKeySpectroMenu", "I") + "] options  [" + hotkey_text("HKeySpectroFull", "U") + " / ESC] close  " + spectro_row_value(sp, kSrScale) + " " +
                       std::to_string(sp.min_freq) + "-" + std::to_string(std::min(sp.max_freq, a.rate() / 2)) + " Hz", border_bottom) << "\n";
    return clamp_output_rows(f.str(), term_rows_);
}

// ---- Oscilloscope overlay (SHIFT+0) --------------------------------------------------------------------------------
// Keys of the overlay itself. While the grid colour is being typed every key belongs to the prompt.
bool App::scope_big_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (scope_big_color_edit_) {
        if (arrow) return true;
        if (key == 27) { scope_big_color_edit_ = false; status_line_ = ""; }
        else if (key == '\r' || key == '\n') {
            scope_big_color_edit_ = false;
            if (scope_big_color_buf_.empty()) settings_.scope_grid_color.clear();   // empty = the colour of inactive list elements
            else settings_.scope_grid_color = std::to_string(std::clamp(std::atoi(scope_big_color_buf_.c_str()), 0, 255));
            save_settings(settings_);
        } else if ((key == 127 || key == 8) && !scope_big_color_buf_.empty()) scope_big_color_buf_.pop_back();
        else if (key >= '0' && key <= '9' && scope_big_color_buf_.size() < 3) scope_big_color_buf_.push_back(static_cast<char>(key));
        return true;
    }
    if (scope_metrics_open_) {   // the metrics window: UP / DOWN pick a row, LEFT / RIGHT change it, R resets, ESC / SHIFT+p close
        if (arrow) {
            if (key == 'A') scope_metrics_row_ = (scope_metrics_row_ + kScopeMetricRows - 1) % kScopeMetricRows;
            else if (key == 'B') scope_metrics_row_ = (scope_metrics_row_ + 1) % kScopeMetricRows;
            else if (key == 'C' || key == 'D') { scope_metric_step(settings_.scope_metrics, scope_metrics_row_, key == 'C' ? 1 : -1); save_settings(settings_); }
        } else if (key == 27 || key == 'P') scope_metrics_open_ = false;
        else if (key == 'r' || key == 'R') { settings_.scope_metrics = ScopeMetrics(); save_settings(settings_); }
        return true;
    }
    if (arrow) return false;
    if (key == 27) { scope_big_ = false; force_redraw_ = true; return true; }
    if (key == 'P') { scope_metrics_open_ = true; return true; }                                               // SHIFT+p: grid metrics
    if (key == 'A') { settings_.scope_grid = !settings_.scope_grid; save_settings(settings_); return true; }   // SHIFT+a: axes / grid
    if (key == 'V') { settings_.scope_values = !settings_.scope_values; save_settings(settings_); return true; }   // SHIFT+v: axis values
    if (key == 'C') {                                                                                          // SHIFT+c: grid colour
        scope_big_color_edit_ = true;
        scope_big_color_buf_.clear();
        return true;
    }
    return false;
}

std::string App::build_scope_big(int W, const char* clear_prefix) const {
    const int H = std::max(6, term_rows_ - 1);
    const int inner = W - 2;
    const int avail = std::max(3, H - 2);   // between the top and the bottom border
    const bool grid = settings_.scope_grid, values = settings_.scope_values;
    const ScopeBigGeom g = scope_big_geometry(inner, avail, values);
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string legend = ansi_for(settings_.legend_color, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    // both pictures are drawn from the same audio, each in its own phosphor buffer, with the values of the style in use
    // (braille characters, or -- Osci style "image" on a terminal with Kitty graphics / Sixel -- real pixel pictures)
    scope_big_xy_.copy_ring_from(scope_);
    scope_big_yt_.copy_ring_from(scope_);
    const bool image = settings_.osci_style == 1 && gfx_proto_ != GfxProto::None && g.cols > 0 && g.rows > 0;
    OscilloscopeVisualizer::Params px = osci_scope_params(settings_, viz_dt_, image ? 1 : 0);
    px.mode = 0;
    OscilloscopeVisualizer::Params py = px;
    py.mode = 1;
    const OsciSet& os = settings_.osci_set[image ? 1 : 0];
    const int pal = os.palette;
    auto color_of = [&](int tx, int cols, uint8_t hue, float k) {
        if (pal == 0) {
            const float t = cols > 1 ? static_cast<float>(tx) / static_cast<float>(cols - 1) : 0.0f;
            return osci_dim_ansi(gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t), k);
        }
        int r = 255, gg = 255, b = 255;
        osci_palette_rgb(settings_, pal, hue / 255.0f, r, gg, b);
        return osci_rgb_seq(r, gg, b, k);
    };
    // the grid and the axis values take the colour of inactive list elements unless one was typed in
    const std::string grid_ansi = ansi_for(settings_.scope_grid_color.empty() ? settings_.list_color : settings_.scope_grid_color, false);
    const ScopeMetrics& sm = settings_.scope_metrics;
    std::vector<std::string> left, right;
    if (image) {
        // The terminal draws the two pictures itself: the boxes are placeholder cells here (render_frame() hands the pictures to the
        // main loop through gfx_ / gfx2_), the grid is part of each picture.
        gfx_cell_pixels(settings_.cell_pixels, cell_w_, cell_h_);
        const int cw0 = std::max(4, cell_w_), ch0 = std::max(8, cell_h_);
        const int cwq = std::clamp(osci_detail_cap(settings_.image_detail, true) / g.cols, 4, cw0);
        const int scale = std::clamp(settings_.image_scale, 1, 3);   // Osci image resolution: 1/scale of the screen resolution
        const int cws = std::max(2, std::min(cwq, cw0 / scale));
        const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
        int gr = 128, gg2 = 128, gb = 128;
        try_parse_rgb(settings_.scope_grid_color.empty() ? settings_.list_color : settings_.scope_grid_color, gr, gg2, gb);
        for (int k = 0; k < 2; ++k) {
            GfxFrame& F = k == 0 ? gfx_ : gfx2_;
            OscilloscopeVisualizer& sc = k == 0 ? scope_big_yt_ : scope_big_xy_;
            const OscilloscopeVisualizer::Params& pp = k == 0 ? py : px;
            F.id = k; F.scale = scale; F.out_cw = cw0; F.out_ch = ch0; F.crop = 0; F.cut_w = 0;
            F.cols = g.cols; F.rows = g.rows; F.w = g.cols * cws; F.h = g.rows * chs;
            F.row = 1 + g.top + 1;   // below the top border and the picture names
            F.col = 1 + g.margin + g.lab + (k == 0 ? 0 : g.cols + g.gap + g.lab);
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
            if (grid) F.grid = k == 0 ? scope_grid_pixels(F.w, F.h, g.cols, g.rows, 1, sm.yt_time_div, sm.yt_value_div)
                                      : scope_grid_pixels(F.w, F.h, g.cols, g.rows, 0, sm.xy_div, sm.xy_div);
            else F.grid.clear();
            F.grid_rgb = {static_cast<uint8_t>(gr), static_cast<uint8_t>(gg2), static_cast<uint8_t>(gb)};
            F.active = true;
            F.fresh = gfx_due_;
        }
        left.assign(static_cast<size_t>(g.rows), gfx_hole(g.cols));
        right = left;
    } else {
        left = scope_big_pane(scope_big_yt_, py, g.cols, g.rows, grid, 1, grid_ansi, color_of, sm.yt_time_div, sm.yt_value_div);
        right = scope_big_pane(scope_big_xy_, px, g.cols, g.rows, grid, 0, grid_ansi, color_of, sm.xy_div, sm.xy_div);
    }

    // what the axis values and the picture names show: the time span, the scales that 100 % stands for, the measurements
    ScopeBigInfo info;
    info.m = sm;
    info.rotate = os.rotate;
    info.samples = std::clamp(os.trace, 128, 8192);
    info.ms = info.samples * 1000.0 / std::max(8000, player_.sample_rate());
    info.music = px.fixed_gain;
    {
        const OscilloscopeVisualizer::Measure me = scope_big_yt_.measure(px);
        info.rms_db = me.rms_db;
        info.peak_db = me.peak_db;
        info.corr = me.corr;
        info.bal_db = me.bal_db;
    }
    const std::vector<std::string> body = scope_big_compose(g, inner, avail, left, right, legend, grid_ansi, info);
    std::string title = "OSCILLOSCOPE";
    if (has_track_) { const TrackMetadata dm = display_metadata(); title += " - " + (dm.artist.empty() || dm.artist == "-" ? dm.name : dm.artist + " - " + dm.name); }
    std::ostringstream f;
    f << clear_prefix << "\x1b[?25l";
    f << box_top(truncate_str(title, W - 6), W, border) << "\n";
    for (const std::string& line : body) f << bar << line << bar << "\n";
    const std::string foot_head = "[" + hotkey_text("HKeyOscMenu", "O") + "] options  [" + hotkey_text("HKeyScopeBig", "=") + "/ESC] close  ";
    const std::string foot = scope_pick_head({foot_head + "[SHIFT+a] grid  [SHIFT+v] values  [SHIFT+c] color  [SHIFT+p] metrics",
                                              foot_head + "[SHIFT+a/v/c/p] grid / values / color / metrics"}, W - 6);
    f << box_bottom(W, truncate_str(foot, W - 6), border_bottom) << "\n";
    return clamp_output_rows(f.str(), term_rows_);
}

// The window of the grid metrics (SHIFT+p in the oscilloscope overlay): what the axis values are measured in, the divisions of the
// grid and the measurement each picture's name shows -- separately for the YT and the XY picture.
std::vector<std::string> App::build_scope_metrics_panel() const {
    const int W = kScopeMetricsPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string legend = ansi_for(settings_.legend_color, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi, bool head) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : head ? legend + body + R : body) + " " + bar;
    };
    std::vector<std::string> lines;
    lines.push_back(box_top("Grid metrics", W, border));
    for (const auto& r : scope_metric_panel_rows(settings_.scope_metrics, scope_metrics_row_, inner)) {
        const bool head = !r.second && r.first.size() > 1 && r.first[0] != ' ' && r.first[0] != '>';
        lines.push_back(row(r.first, r.second, head));
    }
    lines.push_back(row("Readouts are averaged and update twice a second.", false, false));
    lines.push_back(row("[UP/DOWN] select  [LEFT/RIGHT] change", false, false));
    lines.push_back(box_bottom(W, "[R] reset  [SHIFT+p / ESC] close", border_bottom));
    return lines;
}

// The small window for typing the grid colour (SHIFT+c in the oscilloscope overlay): the digits typed so far, a swatch of the colour.
std::vector<std::string> App::build_scope_color_panel() const {
    const int W = kScopeColorPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain) { return bar + " " + pad_right(truncate_str(plain, inner), inner) + " " + bar; };
    const std::string shown = scope_big_color_buf_.empty() ? (settings_.scope_grid_color.empty() ? settings_.list_color : settings_.scope_grid_color)
                                                           : std::to_string(std::clamp(std::atoi(scope_big_color_buf_.c_str()), 0, 255));
    std::vector<std::string> lines;
    lines.push_back(box_top("Grid color", W, border));
    lines.push_back(row("Color (0-255): " + scope_big_color_buf_ + "█"));
    const std::string label = "Preview: ";
    const int blocks = 8;
    lines.push_back(bar + " " + label + ansi_for(shown, false) + [&] { std::string b; for (int i = 0; i < blocks; ++i) b += "█"; return b; }() + R +
                    std::string(static_cast<size_t>(std::max(0, inner - static_cast<int>(label.size()) - blocks)), ' ') + " " + bar);
    lines.push_back(row("Empty = color of inactive list elements"));
    lines.push_back(box_bottom(W, "[ENTER] ok  [ESC] cancel", border_bottom));
    return lines;
}

} // namespace muisc
