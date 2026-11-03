// app_panels.cpp
// Part of class App (src/app.h): The panels of the main screen: metadata, progress bar, search bar, list and queue.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "hifi_bars.h"
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


// ---------------------------------------------------------------------
// Idle-state cassette picture
// ---------------------------------------------------------------------
// Braille cassette shown in the metadata panel's lyrics/sphere column
// whenever no track is loaded (never started, playback stopped in "stop"
// mode, or the file that was playing got deleted). Byte-for-byte copy of
// tape_ascii.txt: 15 rows of 30 braille cells -- deliberately the same
// geometry as the rotating disk (disk_art.cpp), so it fills that column at
// exactly the panel's size and can be tinted with the same top-to-bottom
// disk gradient. See the !has_track_ branch of build_metadata_panel().
//
// The characters are written as \uXXXX escapes so this file stays pure
// ASCII: no editor, copy or checkout can re-encode them, while /utf-8
// still compiles them into exactly the same UTF-8 bytes.
static constexpr const char* kTapeArt[] = {
    "\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2880\u2864\u28c4\u2800\u2800\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2880\u28c0\u28e4\u28c0\u28c0\u2800\u2800\u2880\u2840\u2814\u2801\u2800\u28b8\u2803\u2800\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2820\u2802\u2809\u2809\u2809\u2811\u28b2\u28fe\u2849\u2800\u2800\u2800\u2800\u2808\u2801\u2800\u2800\u28c0\u2814\u2802\u2801\u2800\u2800\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2847\u2800\u2800\u2800\u2800\u2800\u28b8\u2800\u2809\u28a6\u2800\u2800\u2800\u2800\u2800\u2800\u28b0\u2801\u2800\u2800\u2800\u2880\u28c0\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2810\u28c4\u2800\u2800\u2800\u2800\u2808\u2823\u2804\u280a\u2800\u28c0\u28c0\u28e4\u28e4\u28f4\u28fe\u28f6\u28ff\u28ff\u28ff\u28ff\u28ff\u2847\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2800\u2808\u28f3\u28c4\u28e0\u28e4\u28f4\u28f6\u28f6\u28ff\u28ff\u28bf\u28df\u28ef\u28bf\u28dd\u28af\u2877\u28fb\u28be\u28df\u28ff\u28fb\u28ff\u2800\u2800\u2800\u2800",
    "\u28e4\u28f6\u28f6\u28fe\u28ff\u28bf\u28fb\u289f\u28ef\u28bf\u2875\u28fb\u28ae\u283f\u28ee\u28b7\u28ef\u28fb\u28fd\u286b\u28bf\u289d\u286b\u285b\u28cd\u28bf\u2846\u2800\u2800\u2800",
    "\u28bb\u28fe\u28ff\u28ff\u28ef\u28b7\u28ef\u287b\u28f7\u28eb\u285f\u287d\u28cf\u287b\u2873\u286d\u286a\u28d6\u289c\u28ce\u28b3\u289d\u2854\u286e\u28d5\u283d\u28f7\u2800\u2800\u2800",
    "\u2818\u28ff\u283f\u28de\u289b\u2873\u286d\u28f9\u28a2\u28a7\u2879\u28ea\u28ce\u28de\u283c\u282e\u28be\u287e\u28de\u285b\u281a\u2897\u28dd\u28ae\u286a\u2873\u28fb\u2844\u2800\u2800",
    "\u2800\u28bf\u2857\u2875\u28d9\u288e\u285e\u289c\u2837\u28bb\u28ef\u287b\u28de\u28b7\u28a9\u28a3\u28b9\u287e\u28ef\u2866\u2820\u28ed\u2897\u28b5\u2839\u285c\u287d\u28e7\u2800\u2800",
    "\u2800\u2838\u28df\u28f2\u2809\u28de\u28ba\u28eb\u2840\u28c0\u28bf\u28dd\u28de\u287f\u2874\u2875\u2873\u283b\u280d\u281b\u2899\u2801\u2829\u2800\u2804\u2800\u28b9\u28bf\u2840\u2800",
    "\u2800\u2800\u28ff\u28d5\u289d\u2855\u28c7\u283b\u2832\u281a\u280b\u288a\u2808\u28ec\u28c0\u28e4\u2802\u2880\u2801\u2800\u2804\u2800\u28c2\u28c0\u28ec\u28e4\u28fc\u285d\u2847\u2800",
    "\u2800\u2800\u28b8\u28ef\u280e\u2880\u2800\u28b2\u2866\u2815\u2800\u2804\u2800\u28c3\u28c8\u28ed\u28e4\u28f4\u28f6\u28fe\u28ff\u283f\u283f\u281f\u281b\u280b\u2809\u2809\u2800\u2800",
    "\u2800\u2800\u2800\u28ff\u28f3\u28c0\u28ec\u28e4\u28f4\u28f6\u28fe\u28ff\u283f\u283f\u281f\u281b\u280b\u2809\u2809\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2800\u2839\u283d\u281f\u281b\u280b\u2809\u2809\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800",
};

static constexpr int kTapeArtRows = 15;   // == rows in tape_ascii.txt
static constexpr int kTapeArtWidth = 30;  // == cells per row == disk width

// ---------------------------------------------------------------------
// Panel builders
// ---------------------------------------------------------------------

// ---- oscilloscope colours / parameters (shared by the braille and the image style) ----
namespace {
void osci_temperature_rgb(float t, int& r, int& g, int& b) {
    static const float kStops[5][4] = {{0.00f, 190, 30, 12}, {0.25f, 255, 120, 20}, {0.50f, 255, 224, 150}, {0.75f, 205, 225, 255}, {1.00f, 90, 140, 255}};
    t = std::clamp(t, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        if (t <= kStops[i + 1][0]) {
            const float k = (t - kStops[i][0]) / (kStops[i + 1][0] - kStops[i][0]);
            r = static_cast<int>(kStops[i][1] + (kStops[i + 1][1] - kStops[i][1]) * k);
            g = static_cast<int>(kStops[i][2] + (kStops[i + 1][2] - kStops[i][2]) * k);
            b = static_cast<int>(kStops[i][3] + (kStops[i + 1][3] - kStops[i][3]) * k);
            return;
        }
    }
    r = 90; g = 140; b = 255;
}
} // namespace
// Colour of palette `id` (kOsciPaletteNames) at t = 0..1. 0 and 1 use the VIZ gradient of the COLORS tab.
void osci_palette_rgb(const Settings& s, int id, float t, int& r, int& g, int& b) {
    static const float kStops[5][5][3] = {
        {{20, 205, 120}, {30, 225, 205}, {60, 160, 255}, {150, 95, 255}, {235, 80, 205}},
        {{95, 35, 150}, {170, 45, 140}, {235, 70, 105}, {255, 150, 55}, {255, 232, 150}},
        {{45, 95, 235}, {60, 170, 255}, {120, 230, 255}, {210, 250, 255}, {255, 255, 255}},
        {{0, 255, 220}, {0, 150, 255}, {130, 65, 255}, {255, 45, 205}, {255, 100, 120}},
        {{255, 70, 70}, {255, 175, 45}, {235, 235, 70}, {60, 225, 120}, {85, 160, 255}}};
    t = std::clamp(t, 0.0f, 1.0f);
    if (id <= 1) {
        const std::string a = gradient_ansi(s.visualizer_color, s.visualizer_color_end, t);
        if (std::sscanf(a.c_str(), "\x1b[38;2;%d;%d;%dm", &r, &g, &b) != 3) r = g = b = 220;
        return;
    }
    if (id == 2) { osci_temperature_rgb(t, r, g, b); return; }
    if (id == 8) { r = 70; g = 255; b = 90; return; }   // classic green: the phosphor of an old scope, brightness alone makes the picture
    const int k = std::clamp(id - 3, 0, 4);
    const float f = t * 4.0f;
    const int i = std::min(3, static_cast<int>(f));
    const float u = f - static_cast<float>(i);
    r = static_cast<int>(kStops[k][i][0] + (kStops[k][i + 1][0] - kStops[k][i][0]) * u);
    g = static_cast<int>(kStops[k][i][1] + (kStops[k][i + 1][1] - kStops[k][i][1]) * u);
    b = static_cast<int>(kStops[k][i][2] + (kStops[k][i + 1][2] - kStops[k][i][2]) * u);
}
// The spectrogram's "gradient" colours: the two ends of the VIZ gradient (Settings -> COLORS), like the oscilloscope's.
void spectro_gradient_from(const Settings& st) {
    int r0 = 255, g0 = 255, b0 = 255, r1 = 255, g1 = 255, b1 = 255;
    osci_palette_rgb(st, 0, 0.0f, r0, g0, b0);
    osci_palette_rgb(st, 0, 1.0f, r1, g1, b1);
    auto c8 = [](int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); };
    spectro_set_gradient({c8(r0), c8(g0), c8(b0)}, {c8(r1), c8(g1), c8(b1)});
}
std::string osci_rgb_seq(int r, int g, int b, float k) {
    auto sc = [k](int v) { return std::clamp(static_cast<int>(static_cast<float>(v) * k), 0, 255); };
    return "\x1b[38;2;" + std::to_string(sc(r)) + ";" + std::to_string(sc(g)) + ";" + std::to_string(sc(b)) + "m";
}
std::string osci_dim_ansi(const std::string& ansi, float k) {
    int r, g, b;
    if (std::sscanf(ansi.c_str(), "\x1b[38;2;%d;%d;%dm", &r, &g, &b) != 3) return ansi;   // palette colour: can't scale
    return osci_rgb_seq(r, g, b, k);
}
OscilloscopeVisualizer::Params osci_scope_params(const Settings& s, double dt, int style) {   // style: 0 braille, 1 image, -1 the one in use
    const OsciSet& o = style < 0 ? s.osci() : s.osci_set[style == 1 ? 1 : 0];
    OscilloscopeVisualizer::Params p;
    // the afterglow is set per frame at 30 fps: scale by the real frame time so another frame rate keeps the same look
    p.decay = std::pow(o.decay, static_cast<float>(std::clamp(dt, 0.004, 0.15) * 30.0));
    p.dot_threshold = o.dot_threshold;
    p.tail_brightness = o.tail;
    p.interpolate = o.interp;
    p.z_axis = o.z;
    p.z_depth = o.z_depth;
    p.z_source = o.z_source;
    p.trace = o.trace;
    p.rotate = o.rotate;
    p.mono_phase = o.mono_phase;
    p.glow = o.glow;
    p.mode = s.osci_mode;
    if (o.music) { p.mono_phase = false; p.fixed_gain = true; }   // oscilloscope music mode: the exact figure
    p.crt = o.crt;
    p.low_rate_blank = o.low_rate;
    p.dwell = o.dwell;
    p.rough = o.rough;
    return p;
}

// Lays one meta-editor entry (typed or fetched, saved or not) over the block
// the main screen shows for the playing file. `cur` is the file's path BEFORE
// any rename, which is how a name that is only the file name (no title tag)
// is recognised. Empty tag values show as "-" like probe_metadata() does.
void overlay_meta_entry(TrackMetadata& md, const MetaEditEntry& e, const fs::path& cur) {
    auto dash = [](const std::string& v) { return v.empty() ? std::string("-") : v; };
    const int ti = static_cast<int>(MetaField::Title), ar = static_cast<int>(MetaField::Artist);
    const int al = static_cast<int>(MetaField::Album), yr = static_cast<int>(MetaField::Year);
    const int fn = static_cast<int>(MetaField::FileName);
    if (e.edited[ti]) {
        md.name = e.value[ti].empty() ? path_utf8(cur.stem()) : e.value[ti];
    } else if (e.edited[fn] && !e.value[fn].empty() && md.name == path_utf8(cur.stem())) {
        md.name = e.value[fn];   // no title tag: the name IS the file name, and it is being renamed
    }
    if (e.edited[ar]) md.artist = dash(e.value[ar]);
    if (e.edited[al]) md.album = dash(e.value[al]);
    if (e.edited[yr]) md.year = dash(e.value[yr]);
}

// What the main screen shows: the loaded metadata_ with the pending meta-editor
// edits of the playing local file on top, so typing or fetching in the editor
// (SHIFT+n, SHIFT+t, ...) is visible there at once, before CTRL+s writes it.
TrackMetadata App::display_metadata() const {
    TrackMetadata md = metadata_;
    if (!has_track_ || !current_is_local_ || current_path_.empty()) return md;
    if (const MetaEditEntry* e = meta_entry(path_utf8(current_path_))) overlay_meta_entry(md, *e, current_path_);
    return md;
}

// What the meta data panel needs at the least. In a window that is too narrow the disk goes first, and the oscilloscope /
// lyrics field before it; in a window that is too short for it the whole panel goes (show_metadata_panel()).
namespace {
constexpr int kMetaMinW = 20;      // the meta data column: "Title     : " and a few characters of the value
constexpr int kVizMinW = 14;       // the oscilloscope / lyrics field beside it
constexpr int kMinListRows = 4;    // list / queue rows the window must still have for the panel to stay
}

// The panel stays when the window has the rows for it AND the list and queue panes beneath still get kMinListRows of their own
// (the fixed parts: progress bar and search box), and when it is wide enough for the meta data column.
bool App::show_metadata_panel(int total_width) const {
    if (total_width - 4 - 2 < kMetaMinW) return false;
    const int panel_rows = disk_.height() + 2;
    const int chrome = static_cast<int>(build_progress_panel(total_width).size() + build_search_bar(total_width).size());
    return term_rows_ - 1 - 2 - (panel_rows + chrome) >= kMinListRows;
}

std::vector<std::string> App::build_metadata_panel(int total_width) const {
    if (!show_metadata_panel(total_width)) return {};
    const int inner = total_width - 4;
    // The audio book's picture sits in the disk's place; its field is one column wider on the left (the other fields move to the
    // right, the meta data field is a column shorter).
    const int book_extra = 0;   // (the field was a column wider for a while; the picture keeps its place, the field is the disk's width again)
    std::string sep = "  " + settings_.meta_separator + "  ";
    int sep_w = display_width(sep);
    // too narrow for the disk beside a meta data column: the disk goes (then the oscilloscope / lyrics field, if even that does not fit)
    const bool show_disk = settings_.element_disk && inner - disk_.width() - sep_w >= kMetaMinW;
    const int disk_w = show_disk ? disk_.width() : 0;
    const int panel_h = disk_.height();

    const int fixed_extra = show_disk ? sep_w : 2;

    int avail = std::max(10, inner - disk_w - fixed_extra + book_extra);   // (the extra column is taken from the meta data below)
    // The oscilloscope / lyrics field goes by the width the window has WITH the disk in place (not by what is left after the disk went
    // for lack of room), so that narrowing the window only ever takes things away: all three, no field, meta data alone.
    const int setup_avail = settings_.element_disk ? inner - disk_.width() - sep_w : inner - 2;
    const bool show_viz = settings_.element_vizfield && setup_avail >= kMetaMinW + kVizMinW;
    int meta_w = std::min(42, std::max(kMetaMinW, avail - kVizMinW));
    meta_w = std::min(meta_w, avail);
    int lyrics_w = std::max(0, avail - meta_w);
    if (!show_viz) { meta_w = avail; lyrics_w = 0; }   // "Viz/Lyrics Field" off, or no room: the meta data take the field's place
    if (book_extra) meta_w = std::max(10, meta_w - book_extra);

    std::vector<std::string> disk_frame;
    bool cover_pic_img = false;  // the album cover is a pixel image this frame: the oscilloscope picture then goes into the second slot gfx2_
    bool book_pic_img = false;   // the audio book picture is a real pixel image this frame (it uses the picture slot gfx_)
    if (show_disk && audiobook_ && book_picture_gfx(2, disk_w, panel_h)) {
        book_pic_img = true;
        // placeholder cells: the main loop writes them as spaces or (Sixel) skips them so the picture stays
        for (int i = 0; i < panel_h; ++i) disk_frame.push_back(gfx_hole(disk_w));
    } else if (show_disk && audiobook_) {
        // The opening book takes the disk's place: braille dots in the colour wave. It is smaller than the disk, so it sits
        // centred between the left border and the ':' of the separator (a column to the right of the exact middle when the
        // widths do not divide), and in the upper part of the field (centred vertically).
        const std::vector<std::string> art = book_art_lines(0, book_anim_t_);
        const int art_w = BookArt::get(BookArt::Which::Book).cols();
        const int left = std::clamp((disk_w + 4 - art_w + 1) / 2 - 2 + 1 + 1, 0, std::max(0, disk_w - art_w));
        const int top = std::max(0, (panel_h - static_cast<int>(art.size())) / 2 - 2);   // two rows above the middle: the sparkles rise above the book
        for (int i = 0; i < panel_h; ++i) {
            const int ai = i - top;
            if (ai >= 0 && ai < static_cast<int>(art.size()))
                disk_frame.push_back(std::string(static_cast<size_t>(left), ' ') + art[static_cast<size_t>(ai)] +
                                     std::string(static_cast<size_t>(std::max(0, disk_w - left - art_w)), ' '));
            else disk_frame.emplace_back(std::string(static_cast<size_t>(disk_w), ' '));
        }
    } else if (show_disk && !audiobook_ && eff_album_cover() && album_cover_gfx(2, disk_w, panel_h)) {
        // Album Cover: the cover of the playing track is a pixel image in the disk's place (it uses the picture slot gfx_, so the
        // oscilloscope image goes into the second slot gfx2_)
        cover_pic_img = true;
        for (int i = 0; i < panel_h; ++i) disk_frame.push_back(gfx_hole(disk_w));
    } else if (show_disk && !audiobook_ && eff_album_cover() && (gfx_proto_ == GfxProto::None || eff_cover_mode() >= 5) &&
               !(disk_frame = album_cover_lines(disk_w, panel_h)).empty()) {
        // (no Kitty / Sixel: the cover in braille dots)
    } else if (show_disk) {
        disk_frame.clear();
        disk_frame = disk_.frame(angle_);
        while (static_cast<int>(disk_frame.size()) < panel_h) disk_frame.emplace_back(std::string(disk_w, ' '));
        for (size_t row_i = 0; row_i < disk_frame.size(); ++row_i) {
            float t = disk_frame.size() > 1 ? static_cast<float>(row_i) / static_cast<float>(disk_frame.size() - 1) : 0.0f;
            std::string disk_end = settings_.disk_color_end;
            std::string row_ansi = gradient_ansi(settings_.disk_color, disk_end, t);
            disk_frame[row_i] = row_ansi + disk_frame[row_i] + "\x1b[0m";
        }
    }

    double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    fft_.set_fluidity(settings_.visualizer_fluidity);
    fft_.set_degradation_speed(settings_.visualizer_degradation_speed);
    fft_.set_viscosity(settings_.visualizer_viscosity);

    // meta content rows
    std::vector<std::string> meta_rows(panel_h, std::string());
    std::vector<int> bars; // computed once below, reused by the sphere visualizer fallback further down
    if (has_track_) {
        int meta_row_cursor_ = 1; // row 0 stays blank: the metadata block starts below the panel's top edge (the idle message is centred in the column instead)
        std::string k_col = settings_.meta_key_color.empty() ? ansi_for(settings_.list_color) : ansi_for(settings_.meta_key_color);
        std::string v_col = settings_.meta_val_color.empty() ? ansi_for(settings_.list_color) : ansi_for(settings_.meta_val_color);
        // Rows that may be used by the metadata block: everything above the
        // visualizer's fixed two bottom rows (all rows when the visualizer is
        // switched off). Nothing below can ever reach past this line, so the
        // spectrum is never overwritten, shifted or pushed -- whatever the
        // tags look like.
        // (Hi-Fi bars are up to four rows high: as many as the meta data leave, at least two)
        const bool hifi_bars = settings_.element_visualizer && settings_.bars_style == 1;
        const int viz_rows = !settings_.element_visualizer ? 0 : hifi_bars ? std::clamp(panel_h - 11, 2, 4) : 2;
        const int last_meta_row = panel_h - viz_rows - 1;
        // `marquee_slot` >= 0: the value always takes exactly one row and, when
        // it is wider than the column, scrolls through it (Name / Artist /
        // Album). Otherwise it wraps over up to `max_lines` rows, but only as
        // many as are still free (Location).
        auto kv = [&](const std::string& label, const std::string& value, int max_lines = 1, int marquee_slot = -1) {
            if (meta_row_cursor_ > last_meta_row) return;   // no free row left: drop it, never touch the visualizer rows
            std::string mapped_label = apply_font_map(label, settings_.font_map);
            std::string mapped_val = apply_font_map(value, settings_.font_map);
            int avail_v = std::max(0, meta_w - 12);
            std::string l_pad = pad_right(mapped_label, 10);

            std::vector<std::string> value_lines;
            if (marquee_slot >= 0 && marquee_slot < 3) {
                // The scroll restarts whenever the text changes (new track).
                const int key = static_cast<int>(std::hash<std::string>{}(mapped_val) & 0x7fffffffu);
                // the scrolling text ends one column before the next field starts
                const int mw = std::max(1, avail_v - 1);
                value_lines.push_back(marquee_or_truncate(mapped_val, mw, key,
                                                          marquee_meta_idx_[marquee_slot], marquee_meta_since_[marquee_slot]) +
                                      (avail_v > 0 ? " " : ""));
            } else {
                const int room = std::max(1, last_meta_row - meta_row_cursor_ + 1);
                value_lines = wrap_lines(mapped_val, avail_v, std::min(max_lines, room));
                if (value_lines.empty()) value_lines.push_back(std::string());
            }

            for (size_t li = 0; li < value_lines.size(); ++li) {
                if (meta_row_cursor_ > last_meta_row) break;   // never past the visualizer
                const std::string& v_tr = value_lines[li];
                bool first = (li == 0);
                // Continuation lines repeat the label column as blank
                // space (not the colon) so the wrapped text lines up
                // directly under where the value on line one starts.
                std::string label_col = first ? l_pad : std::string(10, ' ');
                std::string sep_txt = first ? ": " : "  ";
                std::string plain = label_col + sep_txt + v_tr;
                std::string ansi = first
                    ? (k_col + label_col + "\x1b[0m" + sep_txt + v_col + v_tr + "\x1b[0m")
                    : (label_col + sep_txt + v_col + v_tr + "\x1b[0m");
                ansi += std::string(std::max(0, meta_w - display_width(plain)), ' ');
                meta_rows[meta_row_cursor_++] = ansi;
            }
        };
        // Name, Artist and Album are the fields most likely to overrun the
        // column (long titles, multi-artist collabs). They stay on one row
        // each and scroll as a marquee when too long, so the block has a
        // fixed height and the visualizer below is never affected. Location
        // (deep folder paths) may still use a second row if one is free.
        const TrackMetadata shown = display_metadata();   // pending meta-editor edits included
        // The first line follows SHIFT+n (HKeyToggleMetaOnly), like the list rows do: "Title" is the title tag (the
        // file name when the file has none), "File" is the file name -- also with a pending FILE rename applied.
        // Streams have no file name and always show "Title".
        const bool show_file = has_track_ && current_is_local_ && !current_path_.empty() && !settings_.meta_only;
        std::string first_value = shown.name;
        if (show_file) {
            first_value = path_utf8(current_path_.stem());
            if (const MetaEditEntry* me = meta_entry(path_utf8(current_path_))) {
                const int fn = static_cast<int>(MetaField::FileName);
                if (me->edited[fn] && !me->value[fn].empty()) first_value = me->value[fn];
            }
        }
        if (audiobook_) {
            // The audio book's block: everything that matters for a book (tags read by book_poll(), "-" while they load).
            const bool tags_ready = book_meta_path_ == path_utf8(current_path_);
            auto tag = [&](int i) { return tags_ready && !book_meta_vals_[static_cast<size_t>(i)].empty() ? book_meta_vals_[static_cast<size_t>(i)] : std::string("-"); };
            std::string book_name = tag(1);
            if (book_name == "-" && !book_dir_.empty()) book_name = path_utf8(path_from_utf8(book_dir_).filename());
            const int cur_ch = book_current_chapter();
            std::string chapter = tag(4);
            if (chapter == "-") chapter = path_utf8(current_path_.stem());
            if (cur_ch >= 0) chapter = std::to_string(cur_ch + 1) + "/" + std::to_string(chapters_.size()) + "  " + chapter;
            double total = 0, done = 0; bool complete = false;
            book_totals(elapsed, total, done, complete);
            auto clock = [](double sec) {
                const long long t = static_cast<long long>(std::max(0.0, sec) + 0.5);
                char b[32];
                if (t >= 3600) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
                else std::snprintf(b, sizeof b, "%02lld:%02lld", t / 60, t % 60);
                return std::string(b);
            };
            kv("Book", book_name, 1, 0);
            kv("Author", tag(2), 1, 1);
            kv("Narrator", tag(3), 1, 2);
            kv("Chapter", chapter);
            kv("Publisher", tag(5));
            kv("Genre", tag(6));
            if (total > 0) {
                kv("Length", clock(total) + (complete ? "" : " +"));
                kv("Heard", clock(done) + "  (" + std::to_string(static_cast<int>(100.0 * done / total + 0.5)) + " %)");
            }
            kv("Sampling", metadata_.sampling);
            kv("Format", metadata_.format);
            kv("File size", metadata_.file_size);
        } else {
        kv(show_file ? "File" : "Title", first_value, 1, 0);
        kv("Artist", shown.artist, 1, 1);
        kv("Album", shown.album, 1, 2);
        kv("Year", shown.year);
        kv("Sampling", metadata_.sampling);
        kv("Type", metadata_.type);
        kv("Format", metadata_.format);
        kv("File size", metadata_.file_size);
        kv("Location", metadata_.location, 2);
        if (!metadata_.extra_label.empty()) kv(metadata_.extra_label, metadata_.extra_value);
        }

        // Real spectrum visualizer (KISS FFT), not a copy of the progress
        // bar's RMS envelope. Two rows: bottom row is the base level
        // (0-4), top row is whatever's left over above that (0-4) so
        // taller peaks build upward — attached to the panel's bottom row
        // per instruction, with the second row directly above it.
        if (hifi_bars) {
            // the Hi-Fi style: a stereo spectrum, a braille dot column per frequency step, at the frame rate of the program
            const int viz_w = std::min(meta_w, show_viz ? 48 : 96);
            bars = fft_.compute_bars(48, viz_dt_);   // (the sphere below uses the classic bars)
            HifiView hv;
            hv.cells = viz_w; hv.rows = viz_rows; hv.resolution = settings_.hifi_resolution; hv.channels = settings_.hifi_channels;
            hv.color = settings_.hifi_color; hv.scale = settings_.hifi_scale; hv.adapt = settings_.hifi_adapt; hv.fscale = settings_.hifi_fscale; hv.gain = settings_.hifi_gain; hv.range = settings_.hifi_range; hv.split = settings_.hifi_split; hv.fluidity = settings_.hifi_fluidity; hv.degradation = settings_.hifi_degradation;
            hv.viscosity = settings_.hifi_viscosity; hv.dt = viz_dt_;
            spectro_gradient_from(settings_);
            const auto& pal = spectro_palette(settings_.spectro.scheme);
            std::vector<std::string> hrows = hifi_rows(hifi(), hv,
                [&](float t) {
                    return !settings_.viz_center_color.empty() ? multi_stop_gradient_ansi(settings_.viz_left_color, settings_.viz_center_color, settings_.viz_right_color, t)
                                                              : gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t);
                },
                [&](float f) {
                    const auto& c = pal[static_cast<size_t>(std::clamp(48 + static_cast<int>(f * 207.0f), 0, 255))];
                    return "\x1b[38;2;" + std::to_string(c[0]) + ";" + std::to_string(c[1]) + ";" + std::to_string(c[2]) + "m";
                });
            for (int r = 0; r < viz_rows; ++r) {
                std::string line = hrows[static_cast<size_t>(r)];
                if (viz_w < meta_w) line += std::string(static_cast<size_t>(meta_w - viz_w), ' ');
                meta_rows[static_cast<size_t>(panel_h - viz_rows + r)] = line;
            }
        } else if (settings_.element_visualizer) {
            int viz_w = std::min(meta_w, show_viz ? 48 : 96);
            bars = fft_.compute_bars(viz_w, viz_dt_);
            std::string viz_top, viz_bottom;
            int nbars = static_cast<int>(bars.size());
            for (int i = 0; i < nbars; ++i) {
                int level = bars[i];
                float t = nbars > 1 ? static_cast<float>(i) / static_cast<float>(nbars - 1) : 0.0f;
                std::string bar_ansi;
                if (!settings_.viz_center_color.empty()) {
                    bar_ansi = multi_stop_gradient_ansi(settings_.viz_left_color, settings_.viz_center_color, settings_.viz_right_color, t);
                } else {
                    bar_ansi = gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t);
                }
                viz_bottom += bar_ansi;
                viz_bottom += fft_glyph(std::min(level, 4));
                viz_top += bar_ansi;
                viz_top += fft_glyph(std::max(0, level - 4));
            }
            viz_top += "\x1b[0m";
            viz_bottom += "\x1b[0m";
            if (viz_w < meta_w) {
                std::string tail(meta_w - viz_w, ' ');
                viz_top += tail;
                viz_bottom += tail;
            }
            meta_rows[panel_h - 2] = viz_top;
            meta_rows[panel_h - 1] = viz_bottom;
        } else {
            bars = fft_.compute_bars(48, viz_dt_);
        }
    } else {
        // Nothing loaded -- never started, playback stopped with no track,
        // or the file that was playing got deleted. The message sits centred
        // (both ways) in the metadata/visualizer column instead of being
        // glued to its first row, where it would hide behind the disk.
        meta_rows[std::max(0, panel_h / 2)] = center_pad(audiobook_ ? "Currently No Narrative Loaded" : "Currently No Track Loaded", meta_w);
    }
    for (int i = 0; i < static_cast<int>(meta_rows.size()); ++i) {
        if (meta_rows[i].empty()) {
            meta_rows[i] = std::string(meta_w, ' ');
        }
        // Every other row (kv data, visualizer, and the centred idle message
        // above -- center_pad() already pads that one to meta_w) was padded
        // by its own builder, and padding it again would miscount its ANSI
        // color escapes as visible columns, truncating it.
    }

    // lyrics window: word-wrapped, center-aligned, word-level highlight on
    // the active line — windowed so the active line's wrapped block is
    // always vertically centered, blank-padded at the edges.
    std::vector<std::string> lyric_rows(panel_h, std::string(lyrics_w, ' '));
    bool lyrics_avail = false;
    std::vector<LyricLine> lines_copy;
    std::string lyrics_status;
    double lyrics_delay = 0.0; // lyrics timing correction (ALT+L), seconds, + = lyrics later
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (audiobook_) {
            // the audio book mode fetches no lyrics: its field shows the notes (or the visualization), never a "fetching" caption
        } else if (settings_.lyrics_on() && lyrics_ready_) {
            lines_copy = lyrics_result_.lines;
            latinize_lyrics(lines_copy);
            lyrics_delay = lyrics_result_.delay;
            lyrics_status = lyrics_result_.message;
            lyrics_avail = !lines_copy.empty();
        } else if (settings_.lyrics_on() && has_track_) {
            lyrics_status = "fetching lyrics ...";
        }
        // else: Lyrics Engine is off -- no fetch ever ran, so there's
        // nothing to report. Leaving lyrics_status empty means the sphere
        // below renders with no caption at all, rather than a stale or
        // misleading status line.
    }

    if (!has_track_ && audiobook_) {
        // No book loaded: the stack of books builds itself up where the visualizations are.
        const std::vector<std::string> st = book_art_lines(1, book_idle_t_);
        const int sw = BookArt::get(BookArt::Which::Stack).cols();
        const int start = std::max(0, (panel_h - static_cast<int>(st.size())) / 2);
        const int left = std::max(0, (lyrics_w - sw) / 2);
        for (int i = 0; i < static_cast<int>(st.size()) && start + i < panel_h && lyrics_w >= sw; ++i)
            lyric_rows[static_cast<size_t>(start + i)] = std::string(static_cast<size_t>(left), ' ') + st[static_cast<size_t>(i)] +
                                                         std::string(static_cast<size_t>(std::max(0, lyrics_w - left - sw)), ' ');
    } else if (!has_track_) {
        // Nothing is playing, so there is no lyric visual and no lyric line to draw
        // and this column would just be empty. Fill it with the braille
        // cassette picture (kTapeArt above -- 15 rows of 30 cells, the disk's
        // own dimensions), centred in the column and tinted with the SAME
        // top-to-bottom disk gradient the rotating disk gets, so the idle
        // panel keeps the colour it has while playing.
        int start = std::max(0, (panel_h - kTapeArtRows) / 2);
        for (int i = 0; i < kTapeArtRows && start + i < panel_h && lyrics_w > 0; ++i) {
            float t = panel_h > 1 ? static_cast<float>(start + i) / (panel_h - 1) : 0.0f;
            int left = (lyrics_w - kTapeArtWidth) / 2;
            std::string body = (left >= 0)
                ? std::string(left, ' ') + kTapeArt[i]
                  + std::string(lyrics_w - left - kTapeArtWidth, ' ')
                : utf8_skip_take(kTapeArt[i], -left, lyrics_w); // column narrower than the art: keep its middle, never wrap braille cells
            lyric_rows[start + i] =
                gradient_ansi(settings_.disk_color, settings_.disk_color_end, t) + body + "\x1b[0m";
        }
    } else if (!lyrics_avail) {
        // A status message ("fetching...", "no lyrics found", etc.) is
        // only shown for the first 1.75s after it appears -- after that
        // the placeholder visual (sphere or osci) gets the whole panel to
        // itself instead of a permanently stuck caption line. Each distinct message content
        // gets its own fresh window (so "fetching..." showing, then
        // later "no lyrics found", each get their moment) rather than
        // one timer for the whole track.
        if (lyrics_status != last_lyrics_status_) {
            last_lyrics_status_ = lyrics_status;
            lyrics_status_shown_at_ = std::chrono::steady_clock::now();
        }
        double status_age = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - lyrics_status_shown_at_).count();
        bool show_caption = status_age < 1.75 && !lyrics_status.empty();

        if (has_track_ && lyrics_w >= 6 && panel_h >= 3) {
            // Fill the panel with the audio-reactive placeholder visual
            // instead of leaving it blank -- which one is picked by
            // Settings -> ANIMA/VIZ -> "Lyrics/Viz" / "When No Lyrics Show":
            //   sphere -> reuses `bars` (already computed for the main
            //             spectrum strip above, same frame) rather than
            //             running a second independent audio analysis.
            //   osci   -> XY scope (L = horizontal, R = vertical) drawn from the
            //             raw waveform ring the audio callback feeds
            //             (OscilloscopeVisualizer).
            // Both are tinted with the VIZ colors (Settings -> Colors ->
            // VIZ): the sphere with the pair's midpoint, the scope with a
            // LEFT -> RIGHT sweep, dimmed per cell by beam brightness.
            // The visual always gets the whole panel; a caption ("fetching lyrics ...") is a small box laid over its
            // bottom row (below), so the picture never changes its size or moves while it shows.
            const int viz_rows_h = panel_h;
            // Lyrics view without lyrics: the "When No Lyrics Show" pick; otherwise the view the "." cycle is on.
            int area_viz = settings_.element_lyrics ? settings_.nolyrics_viz : settings_.lyric_viz;
            if (area_viz < 3 && !settings_.viz_usable(area_viz)) area_viz = 3;   // nothing is switched on: an empty field
            if (area_viz == 1) {
                // XY scope: render() hands back exactly lyrics_w cells per
                // row (empty ones included), each with a braille pattern and
                // a 0..255 beam brightness. EVERY cell is emitted -- blanks
                // as a plain space -- so each row is exactly lyrics_w
                // columns wide; dropping the blanks (as an earlier version
                // did) made rows shorter than the panel, which pushed the
                // right border around and left stale cells on screen.
                // Colour = the VIZ gradient swept left -> right, dimmed by
                // the cell's brightness so the afterglow fades out.
                const OsciSet& os = settings_.osci();
                const int pal = os.palette;
                OscilloscopeVisualizer::Params osci_params = osci_scope_params(settings_, viz_dt_);
                bool as_image = false;
                if (settings_.osci_style == 1 && gfx_proto_ != GfxProto::None && lyrics_w > 0 && !book_pic_img) {
                    // image style: the terminal draws the picture; the cells stay blank here and the loop emits it
                    gfx_cell_pixels(settings_.cell_pixels, cell_w_, cell_h_);
                    const int cw0 = std::max(4, cell_w_), ch0 = std::max(8, cell_h_);
                    const int cwq = std::clamp(osci_detail_cap(settings_.image_detail, false) / lyrics_w, 4, cw0);
                    GfxFrame& g = cover_pic_img ? gfx2_ : gfx_;   // (the album cover owns gfx_)
                    // Image resolution (SHIFT+o): render 1/scale of it; Kitty scales the picture up itself, Sixel repeats
                    // its pixels up to the real cell size (out_cw x out_ch). Less to render, encode and send per frame.
                    g.scale = std::clamp(settings_.image_scale, 1, 3);
                    g.out_cw = cw0; g.out_ch = ch0;
                    // 1/scale of the real cell pixels, but never more than the width cap above (cwq)
                    const int cws = std::max(2, std::min(cwq, cw0 / g.scale));
                    const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
                    g.cols = lyrics_w; g.rows = viz_rows_h; g.w = lyrics_w * cws; g.h = viz_rows_h * chs;
                    g.col = 2 + disk_w + (show_disk ? sep_w : 2) + meta_w;
                    g.row = 1;
                    g.plain = false; g.direct = false; g.opaque = false; g.erase = false;   // (the album cover's picture is a plain one)
                    g.id = cover_pic_img ? 1 : 0; g.grid.clear(); g.cut_w = 0; g.crop = 0;   // no grid on the main screen (the big overlay sets one on the same frame object)
                    scope_.render_image(g.w, g.h, osci_params, g.level, g.hue);
                    if (pal == 0) {
                        for (int y = 0; y < g.h; ++y)
                            for (int x = 0; x < g.w; ++x)
                                g.hue[static_cast<size_t>(y) * g.w + x] = static_cast<uint8_t>(x * 255 / std::max(1, g.w - 1));
                    }
                    for (int h = 0; h < 256; ++h) {
                        int r = 255, gr = 255, b = 255;
                        osci_palette_rgb(settings_, pal, static_cast<float>(h) / 255.0f, r, gr, b);
                        g.pal[static_cast<size_t>(h)] = {static_cast<uint8_t>(r), static_cast<uint8_t>(gr), static_cast<uint8_t>(b)};
                    }
                    g.active = true;
                    g.fresh = gfx_due_;
                    as_image = true;
                    // placeholder cells: the main loop writes them as spaces or (Sixel) skips them so the picture stays
                    for (int i = 0; i < panel_h; ++i) lyric_rows[i] = gfx_hole(lyrics_w);
                }
                if (!as_image) {
                auto osci_cells = scope_.render(lyrics_w, viz_rows_h, osci_params);
                for (int i = 0; i < static_cast<int>(osci_cells.size()) && i < panel_h; ++i) {
                    std::string colored, last_ansi;
                    for (int cell = 0; cell < lyrics_w; ++cell) {
                        const auto& c = osci_cells[i][cell];
                        if (c.braille == 0) { colored += ' '; continue; }
                        const float t = lyrics_w > 1
                            ? static_cast<float>(cell) / static_cast<float>(lyrics_w - 1) : 0.0f;
                        // 8 brightness steps keeps the escape traffic small
                        const float lvl = std::round(c.level / 255.0f * 7.0f) / 7.0f;
                        const float k = 0.30f + 0.70f * lvl;
                        std::string ansi;
                        if (pal == 0) ansi = osci_dim_ansi(gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t), k);
                        else { int r = 255, g = 255, b = 255; osci_palette_rgb(settings_, pal, c.hue / 255.0f, r, g, b); ansi = osci_rgb_seq(r, g, b, k); }
                        if (ansi != last_ansi) { colored += ansi; last_ansi = ansi; }
                        const int cp = 0x2800 + c.braille;
                        colored += static_cast<char>(0xE0 | (cp >> 12));
                        colored += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        colored += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    lyric_rows[i] = colored + "\x1b[0m";
                }
                }
            } else if (area_viz == 2) {
                // Spectrogram (like Audacity's): braille, or a picture the terminal draws (tiles sent by the main loop)
                spectro_used_ = true;
                spectro_gradient_from(settings_);
                spectro().update(settings_.spectro);
                if (settings_.spectro.style == 1 && gfx_proto_ != GfxProto::None) {
                    spectro_area_[0] = 1;
                    spectro_area_[1] = 2 + disk_w + (show_disk ? sep_w : 2) + meta_w;
                    spectro_area_[2] = lyrics_w;
                    spectro_area_[3] = viz_rows_h;
                    for (int i = 0; i < viz_rows_h && i < panel_h; ++i) lyric_rows[i] = gfx_hole(lyrics_w);
                } else {
                    auto rows = spectro().braille(settings_.spectro, lyrics_w, viz_rows_h);
                    for (int i = 0; i < static_cast<int>(rows.size()) && i < panel_h; ++i) lyric_rows[i] = rows[static_cast<size_t>(i)];
                }
            } else if (area_viz == 0) {
                auto sphere_rows = sphere_.render(lyrics_w, viz_rows_h, bars, viz_dt_);
                std::string sphere_ansi = gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, 0.5f);
                for (int i = 0; i < static_cast<int>(sphere_rows.size()) && i < panel_h; ++i) {
                    lyric_rows[i] = sphere_ansi + pad_right(sphere_rows[i], lyrics_w) + "\x1b[0m";
                }
            }
            if (show_caption) {
                const std::string border = ansi_for(settings_.border_color, false);
                std::string text = lyrics_status;
                if (display_width(text) > lyrics_w - 4) text = truncate_str(text, std::max(1, lyrics_w - 4));
                const std::string box = border + "[ \x1b[0m" + text + border + " ]\x1b[0m";
                const int bw = display_width(text) + 4;
                const int x = std::max(0, (lyrics_w - bw) / 2);
                const int row = panel_h - 1;
                lyric_rows[row] = overlay_cells(lyric_rows[row], x, box, bw, lyrics_w);
                const int area_col = 2 + disk_w + (show_disk ? sep_w : 2) + meta_w;
                // where it is on screen (0-based): the picture styles leave these cells free / write it again on top
                caption_rect_[0] = 1 + row; caption_rect_[1] = area_col + x; caption_rect_[2] = bw;
                caption_text_ = box;
            }
        } else if (show_caption) {
            int pad = std::max(0, (lyrics_w - display_width(lyrics_status)) / 2);
            lyric_rows[0] = pad_right(std::string(pad, ' ') + lyrics_status, lyrics_w);
        }
    } else {
        const double lyric_t = elapsed - lyrics_delay; // the time the lyrics are looked up at
        int active = 0;
        for (size_t i = 0; i < lines_copy.size(); ++i) {
            if (lines_copy[i].start_time <= lyric_t) active = static_cast<int>(i);
            else break;
        }

        if (settings_.lyrics_animation == 4) {
            // Only active word: show nothing but whichever single word is
            // currently being sung, centered alone in the panel. Falls
            // back to the whole line if this song has no word-level sync
            // data at all (nothing finer to show).
            const LyricLine& al = lines_copy[active];
            std::string word = al.full_text;
            if (!al.words.empty()) {
                word = al.words.front().second;
                for (const auto& wt : al.words) if (wt.first <= lyric_t) word = wt.second; // last one <= lyric_t
            }
            word = apply_font_map(word, settings_.font_map);
            int pad = std::max(0, (lyrics_w - display_width(word)) / 2);
            std::string line = std::string(pad, ' ') + word;
            std::string colored = ansi_for(settings_.active_word_color) + pad_right(truncate_str(line, lyrics_w), lyrics_w) + "\x1b[0m";
            lyric_rows[panel_h / 2] = colored;
        } else if (settings_.lyrics_animation == 3) {
            // Only active line: same per-word rendering as the default
            // view, just without the scrolling context lines around it.
            auto wrapped = render_lyric_line_wrapped(lines_copy[active], lyric_t, lyrics_w, true, settings_);
            int start_row = std::max(0, (panel_h - static_cast<int>(wrapped.size())) / 2);
            for (size_t i = 0; i < wrapped.size() && start_row + static_cast<int>(i) < panel_h; ++i) {
                lyric_rows[start_row + i] = wrapped[i];
            }
        } else {
            // Full (default) and Word-by-word/Letter-by-letter (which only
            // change render_lyric_line_wrapped's *content*, not this
            // scrolling layout) all share the same multi-line context view.
            //
            // Only wrap lines actually near the visible window — wrapping
            // the whole song every frame would be wasted work.
            int context = 6;
            int lo = std::max(0, active - context);
            int hi = std::min(static_cast<int>(lines_copy.size()) - 1, active + context);

            std::vector<std::string> flat_rows;
            int active_row_start = 0, active_row_count = 1;
            for (int li = lo; li <= hi; ++li) {
                auto wrapped = render_lyric_line_wrapped(lines_copy[li], lyric_t, lyrics_w, li == active, settings_);
                if (li == active) {
                    active_row_start = static_cast<int>(flat_rows.size());
                    active_row_count = static_cast<int>(wrapped.size());
                }
                for (auto& r : wrapped) flat_rows.push_back(std::move(r));
            }

            int active_mid = active_row_start + active_row_count / 2;
            int start = active_mid - panel_h / 2;
            for (int row = 0; row < panel_h; ++row) {
                int idx = start + row;
                if (idx >= 0 && idx < static_cast<int>(flat_rows.size())) lyric_rows[row] = flat_rows[idx];
            }
        }
    }

    // Audio book: the note that is typed or shown takes the field's place (oscilloscope / spectrogram come back when it is put away).
    if (audiobook_ && lyrics_w >= 12 && book_field_has_note()) lyric_rows = book_note_field_rows(lyrics_w, panel_h);

    // --- assemble bordered block ---
    // NOTE: lyric_rows may contain ANSI color codes (word-highlighting),
    // so this assembles rows by direct concatenation of pre-padded pieces
    // rather than routing through box_line()/pad_right() — those count
    // UTF-8 codepoints for width, and ANSI escape bytes would be
    // miscounted as visible columns, throwing off alignment. Every piece
    // here (disk_frame/meta_rows/lyric_rows) is already padded to its own
    // exact width, so the concatenation is guaranteed to equal `inner`.
    std::vector<std::string> out;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    out.push_back(box_top("", total_width, border_ansi));

    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::string sep_ansi = ansi_for(settings_.border_color, false) + sep + "\x1b[0m";
    for (int row = 0; row < panel_h; ++row) {
        std::string content = "";
        if (show_disk) {
            content += disk_frame[row] + sep_ansi;
        } else {
            content += "  ";
        }
        content += meta_rows[row];
        // lyrics_w is now always reserved (see the split above), and
        // lyric_rows is always fully padded to it -- whether that's
        // actual synced lyrics, a status caption, or just the sphere --
        // so this no longer needs to be conditional on element_lyrics.
        content += lyric_rows[row];
        out.push_back(bar + " " + content + " " + bar);
    }

    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_progress_panel(int total_width) const {
    const int button_content_w = 9;
    const int button_total_w = button_content_w + 4; // "│ X │"
    int side_panel_w = settings_.element_dummy_buttons ? (button_total_w * 3) : 38;
    // too narrow for the buttons / the volume bar beside the progress bar: they go, the bar takes the whole width
    const bool has_side = total_width >= 24 + side_panel_w;
    const bool buttons = settings_.element_dummy_buttons && has_side;
    if (!has_side) side_panel_w = 0;
    int main_total_w = has_side ? std::max(24, total_width - side_panel_w) : std::max(8, total_width);
    int wave_w = main_total_w - 4;

    double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    int active_cols = (total_sec_ > 0) ? static_cast<int>((elapsed / static_cast<double>(total_sec_)) * wave_w) : 0;
    active_cols = std::clamp(active_cols, 0, wave_w);

    int reveal_cols = wave_w;
    if (waveform_ready_) {
        double reveal_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - waveform_reveal_start_).count();
        if (reveal_sec < 0.7) {
            reveal_cols = static_cast<int>((reveal_sec / 0.7) * wave_w);
        }
    } else {
        reveal_cols = 0;
    }

    std::string color_played = ansi_for(settings_.progress_played_color);
    std::string color_unplayed = ansi_for(settings_.progress_remaining_color);
    std::string color_reset = "\x1b[0m";

    std::string top_wave, mid_wave, bot_wave;
    std::vector<int> waveform_levels;
    if (!waveform_envelope_.empty()) {
        waveform_levels = WaveformQuantizer::resample_for_ui(waveform_envelope_, wave_w);
    } else {
        waveform_levels.assign(wave_w, 0);
    }

    for (int i = 0; i < wave_w; ++i) {
        bool revealed = i < reveal_cols;
        int level = (revealed && i < static_cast<int>(waveform_levels.size())) ? waveform_levels[i] : 0;
        BrailleColumn col = WaveformQuantizer::get_column(level);
        if (i == 0) {
            std::string c = (i < active_cols) ? color_played : color_unplayed;
            top_wave += c; mid_wave += c; bot_wave += c;
        } else if (i == active_cols) {
            top_wave += color_unplayed; mid_wave += color_unplayed; bot_wave += color_unplayed;
        }
        top_wave += col.top; mid_wave += col.mid; bot_wave += col.bot;
    }
    top_wave += color_reset; mid_wave += color_reset; bot_wave += color_reset;

    // Generic fallback bar for when the waveform element is switched
    // off: a plain "[#####-------]" fill on the middle row, blank above and below it. top_wave and
    // mid_wave are drawn only when the waveform is on, so turning it off clears the whole band, not
    // just the bottom row.
    std::string generic_blank(wave_w, ' ');
    std::string generic_bar;
    if (!settings_.element_waveform) {
        int inner_w = std::max(0, wave_w - 2); // account for the '[' and ']'
        int filled = (total_sec_ > 0) ? static_cast<int>((elapsed / static_cast<double>(total_sec_)) * inner_w) : 0;
        filled = std::clamp(filled, 0, inner_w);
        generic_bar = "[" + color_played + std::string(filled, '#') + color_reset
                     + color_unplayed + std::string(inner_w - filled, '-') + color_reset + "]";
    }

    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    // Buttons (<<< PLAY >>>) and the volume bar now match the border
    // color rather than the separate (and, for these two elements,
    // effectively unused/inert) button_color field.
    std::string button_ansi = border_ansi;

    auto button_mid = [&](const std::string& text, const std::string& text_ansi = std::string()) {
        std::string centered = (text_ansi.empty() ? button_ansi : text_ansi) + center_pad(text, button_content_w) + "\x1b[0m";
        std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
        return bar + " " + centered + " " + bar;
    };
    std::string play_label = (has_track_ && player_.is_paused()) ? "PLAY" : (has_track_ ? "PAUSE" : "PLAY");

    // While seeking (the arrow keys) the << or >> label takes the BORDER_COLOR BOTTOM colour for a moment.
    std::string seek_back_text = border_ansi, seek_fwd_text = border_ansi;
    if (std::chrono::steady_clock::now() < seek_flash_until_) {
        if (seek_flash_dir_ < 0) seek_back_text = border_ansi_bottom;
        else seek_fwd_text = border_ansi_bottom;
    }

    std::vector<std::string> out;
    if (buttons) {
        out.push_back(box_top("PROGRESS BAR", main_total_w, border_ansi)
                      + box_top("", button_total_w, border_ansi) + box_top("", button_total_w, border_ansi)
                      + box_top("", button_total_w, border_ansi));
    } else {
        out.push_back(box_top("PROGRESS BAR", main_total_w, border_ansi));
    }
    
    std::string row1_content = settings_.element_waveform ? top_wave : generic_blank;
    std::string row2_content = settings_.element_waveform ? mid_wave : generic_bar;
    std::string row3_content = settings_.element_waveform ? bot_wave : generic_blank;

    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    if (buttons) {
        out.push_back(bar + " " + row1_content + " " + bar
                      + button_mid("<<<", seek_back_text) + button_mid(play_label) + button_mid(">>>", seek_fwd_text));
        out.push_back(bar + " " + row2_content + " " + bar
                      + box_bottom(button_total_w, "", border_ansi_bottom) + box_bottom(button_total_w, "", border_ansi_bottom)
                      + box_bottom(button_total_w, "", border_ansi_bottom));
    } else {
        out.push_back(bar + " " + row1_content + " " + bar);
        out.push_back(bar + " " + row2_content + " " + bar);
    }

    int vol = player_.volume();
    int vol_hashes = (vol * 20) / 100;
    // "#" (filled) matches the waveform's played color, "-" (empty)
    // matches its unplayed/remaining color -- color_played/color_unplayed
    // are already computed above from progress_played_color/
    // progress_remaining_color for the waveform itself, reused here so
    // the volume bar visually reads as the same kind of fill. The
    // "VOLUME BAR:[...]" label and brackets use the border color, same
    // as the buttons above.
    // The filled part is drawn with the Volume Indicator of the GENERAL tab ("#", "⠶", "⣿" or the "⡾⣽" row); every
    // position is one column wide whatever the look, so the width math below is the same for all of them.
    // angle_: the disk's angle, so "disk" follows it
    std::string vol_fill_colored;
    if (settings_.volume_gradient) {   // "gradient": every position takes its colour from the VIZ gradient, left to right, as the oscilloscope does
        const auto cells = volume_bar_cells(settings_.volume_indicator, vol_hashes, angle_);
        for (size_t i = 0; i < cells.size(); ++i)
            vol_fill_colored += gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, static_cast<float>(i) / (kVolBarCells - 1)) + cells[i];
    } else {
        vol_fill_colored = color_played + volume_bar_fill(settings_.volume_indicator, vol_hashes, angle_);
    }
    std::string vol_bar_colored = vol_fill_colored + "\x1b[0m"
                                 + color_unplayed + std::string(20 - vol_hashes, '-') + "\x1b[0m";
    std::string vol_bar_text = std::string(vol_hashes, '#') + std::string(20 - vol_hashes, '-'); // plain, for width math only
    std::string vol_tail = border_ansi + "VOLUME BAR:[" + "\x1b[0m" + vol_bar_colored
                          + border_ansi + "]" + "\x1b[0m" + " " + std::to_string(vol) + "%";
    if (!has_side) vol_tail.clear();
    int side_w = total_width - main_total_w;
    // pad_left counts raw bytes, so it can't be used once vol_tail carries
    // ANSI bytes -- pad by the *visible* width instead (the volume-bar
    // color fix below is what introduced the mismatch).
    int visible_w = display_width("VOLUME BAR:[" + vol_bar_text + "] " + std::to_string(vol) + "%");
    int gap = std::max(0, side_w - visible_w);

    if (!settings_.autosave_enabled) {
        // AutoSave fully off: no indicator, and the blank gap moves to
        // the *right* of the text instead of sitting between the
        // border and it -- "│VOLUME BAR" touching the border directly,
        // same total line width either way so nothing else has to
        // change to stay aligned.
        if (gap > 0) vol_tail = vol_tail + std::string(gap, ' ');
    } else {
        // AutoSave on: keep the existing gap (│  VOLUME BAR), but let
        // the indicator glyph occupy the first character of it when
        // enabled -- │• VOLUME BAR with room to spare, or │•VOLUME BAR
        // once the gap is down to exactly one column.
        std::string pad(gap, ' ');
        if (gap >= 1) {
            std::string glyph = autosave_indicator_glyph();
            if (!glyph.empty()) pad = glyph + pad.substr(1);
        }
        vol_tail = pad + vol_tail;
    }

    std::string time_plain = "[ " + fmt_mmss(elapsed) + " ]" + settings_.box_horizontal + "[ " + fmt_mmss(static_cast<double>(total_sec_)) + " ]";
    // Manual box-bottom construction (rather than the shared box_bottom()
    // helper) so the timestamp text can carry its own color: box_bottom()
    // measures/pads the footer as plain text, and embedding ANSI bytes
    // into that path would get miscounted as visible columns.
    std::string ts_ansi = ansi_for(settings_.progress_timestamp_color.empty() ? settings_.border_color : settings_.progress_timestamp_color, false);
    std::string time_colored = ts_ansi + time_plain + "\x1b[0m";
    std::string prefix_plain = settings_.box_lower_left + settings_.box_horizontal + " " + time_plain + " ";
    int used = display_width(prefix_plain);
    int dashes_n = std::max(0, main_total_w - used - 1);
    std::string bottom_line = border_ansi_bottom + settings_.box_lower_left + settings_.box_horizontal + " "
                             + time_colored + " "
                             + border_ansi_bottom; // re-apply the border color -- time_colored's own reset above would otherwise leave the rest of this line uncolored.
                                                   // Straight in front of the dashes: the smooth gradient only recolours a border escape that a border glyph follows
    for (int i = 0; i < dashes_n; ++i) bottom_line += settings_.box_horizontal;
    bottom_line += settings_.box_lower_right;
    bottom_line += "\x1b[0m";
    // This row is only main_total_w wide, but the row above it runs the
    // full width (volume bar occupies the right-hand side). Frames are
    // drawn without a screen clear (cursor-home only), so any cell this
    // row does not write keeps whatever was there before -- e.g. the edge
    // of a floating panel (Retry Lyrics / Bulk Add / Clear Queue) that was
    // stamped over it, which then stayed on screen under the volume bar
    // after the panel closed. Pad to the full width so it is overwritten.
    if (side_w > 0) bottom_line += std::string(side_w, ' ');
    out.push_back(bar + " " + row3_content + " " + bar + vol_tail);
    out.push_back(bottom_line);
    return out;
}
std::vector<std::string> App::build_search_bar(int total_width) const {
    std::string label = (list_source_ == ListSource::Online) ? "SEARCH ONLINE"
                       : (list_source_ == ListSource::Playlist) ? "SEARCH PLAYLISTS"
                       : (list_source_ == ListSource::Folder) ? (audiobook_ ? "SEARCH BOOKS" : "SEARCH FOLDERS")
                       : "SEARCH LOCAL";
    if (list_source_ == ListSource::Local) {   // "/r:" and "/a:" narrow the search to artists / albums
        std::string rest;
        const int scope = local_query_scope(mode_ == Mode::Search ? search_buffer_ : last_local_query_, rest);
        if (scope == 1) label = "SEARCH ARTISTS";
        else if (scope == 2) label = "SEARCH ALBUMS";
    }

    {
        const std::string sl = sleep_timer_label();
        if (!sl.empty()) label += "  [" + sl + "]";
    }
    // The online resolve/download step (YouTube, SoundCloud, Bandcamp) shows its live note here, in the search box's
    // top border, instead of on an extra row under the list: that row made the frame one line taller than the
    // terminal for a moment, which scrolled the whole UI.
    if (load_in_progress_.load() && load_stage_.load() == 1) {
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - load_started_at_).count();
        const std::string t = std::to_string(static_cast<int>(secs)) + "s)";
        const int room = total_width - 5 - 5;   // search box width minus its corners and label padding
        for (const std::string& note : {"  resolving/downloading... (" + t, "  downloading... (" + t, "  (" + t}) {
            if (display_width(label + note) <= room) { label += note; break; }
        }
    }

    std::string content;
    if (mode_ == Mode::Search) {
        content.clear(); // caret/selection box built lower down
    } else if (list_source_ == ListSource::Online) {
        content = std::string(last_online_source_ == 1 ? "/sc:" : last_online_source_ == 2 ? "/b:" : "/s:") + last_online_query_;
    } else if (list_source_ == ListSource::Playlist) {
        content = "/p:" + last_playlist_query_;
    } else if (list_source_ == ListSource::Folder) {
        content = (audiobook_ ? "/" : "/f:") + last_folder_query_;
    } else {
        std::string rest;
        const int scope = local_query_scope(last_local_query_, rest);
        content = scope ? "/" + std::string(1, scope == 1 ? 'r' : 'a') + ":" + rest : "/l:" + last_local_query_;
    }

    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::vector<std::string> out;
    int search_w = total_width - 5;
    // Audio book player: a small box with the playback speed sits left of the play-mode box ("1.0x", "1.25x" ...); it is as wide
    // as its text, and the search box gives up those columns.
    const std::string speed_txt = audiobook_ ? speed_label(settings_.book_speed) : std::string();
    const int speed_w = speed_txt.empty() ? 0 : static_cast<int>(speed_txt.size()) + 4;   // "│ 1.25x │"
    if (speed_w > 0 && search_w - speed_w >= 20) search_w -= speed_w;
    const bool speed_box = speed_w > 0 && search_w + speed_w + 5 <= total_width;
    std::string dash;
    for (int i = 0; i < speed_w - 2; ++i) dash += "─";
    if (mode_ == Mode::Search) label += focus_indicator(settings_, border_ansi);   // the search box has the keys
    out.push_back(box_top(label, search_w, border_ansi) + (speed_box ? border_ansi + "╭" + dash + "╮" : std::string()) + border_ansi + "╭───╮\x1b[0m");
    // Play-mode indicator: L=list, R=repeat, S=shuffle, Q=queue then stop,
    // O=stop -- one letter for whichever of the five settings_.play_mode
    // states is active, cycled with a single "m" press
    // (HKeyCyclePlayMode) rather than a separate toggle per mode.
    std::string mode_letter(1, play_mode_letter());
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::string line;
    if (mode_ == Mode::Search) {
        // Caret + selection + block cursor. This one row cannot go through
        // box_line(): that pads with display_width(), which counts the
        // reverse-video escapes of a marked range as columns, so every
        // highlighted character would over-pad the line -- pad against
        // paint_edit_field column count instead (it returns exactly what
        // those escapes are worth).
        const int inner = std::max(1, search_w - 4); // box_line inner width
        const std::string idle = search_idle_sgr(settings_);
        const bool typed = !search_buffer_.empty();
        EditPaint p = paint_edit_field(search_buffer_, edit_caret_, edit_anchor_, inner - 1, typed ? search_sgr(settings_) : idle, true);
        std::string body = "/" + search_run(settings_, p.s, typed);
        const int fill = std::max(0, inner - (1 + p.cols));
        std::string pad(static_cast<size_t>(fill), ' ');
        line = bar + " " + idle + body + pad + "\x1b[0m " + bar;
    } else {
        const int inner = std::max(0, search_w - 4);
        // "/l:" etc. stay idle; only the query behind it is highlighted.
        const std::string shown = truncate_str(content, inner);
        const size_t colon = shown.find(':');
        const std::string pre = colon == std::string::npos ? shown : shown.substr(0, colon + 1);
        const std::string qry = shown.substr(pre.size());
        const int fill = std::max(0, inner - display_width(shown));
        line = bar + " " + search_idle_sgr(settings_) + pre + search_run(settings_, qry, !qry.empty())
             + std::string(static_cast<size_t>(fill), ' ') + "\x1b[0m " + bar;
    }
    const std::string speed_cell = speed_box ? border_ansi + settings_.box_vertical + "\x1b[0m " + speed_txt + " " + border_ansi + settings_.box_vertical + "\x1b[0m" : std::string();
    out.push_back(line + speed_cell + border_ansi + settings_.box_vertical
                  + " " + mode_letter + " " + settings_.box_vertical + "\x1b[0m");
    out.push_back(box_bottom(search_w, "", border_ansi_bottom) + (speed_box ? border_ansi_bottom + "╰" + dash + "╯" : std::string()) + border_ansi_bottom + "╰───╯\x1b[0m");
    return out;
}

// "LOCAL AUDIO FILES (sort: ..., folder: <name> [c] clear)". A long folder name would run past the pane's top border
// and break the frame, so the text inside the () is shortened with "..." until the whole title fits.
std::string App::local_pane_label(int total_width) const {
    const std::string head = "LOCAL AUDIO FILES (sort: " + std::string(sort_mode_name(local_sort_mode_, settings_.meta_only));
    if (folder_filter_.empty()) return head + ")";
    const int room = total_width - 5;   // the title sits between "╭─ " and " ──╮"
    const std::string name = path_utf8(path_from_utf8(folder_filter_).filename());
    const std::string pre = ", folder: ", post = " [c] clear)";
    int avail = room - display_width(head) - display_width(pre) - display_width(post);
    if (avail >= 4) return head + pre + truncate_str(name, avail) + post;
    // very narrow pane: drop the "[c] clear" hint first, then the folder name itself
    const std::string post2 = ")";
    avail = room - display_width(head) - display_width(pre) - display_width(post2);
    if (avail >= 4) return head + pre + truncate_str(name, avail) + post2;
    std::string tiny = head + ")";
    return display_width(tiny) <= room ? tiny : truncate_str(tiny, std::max(1, room));
}

std::vector<std::string> App::build_list_panel(int total_width, int height) const {
    bool online = (list_source_ == ListSource::Online);
    bool playlists_mode = (list_source_ == ListSource::Playlist);
    bool folders_mode = (list_source_ == ListSource::Folder);
    std::string label = online ? "ONLINE RESULTS"
                       : playlists_mode ? "SAVED PLAYLISTS (Enter: queue all)"
                       : folders_mode ? (audiobook_ ? "AUDIO BOOKS (Enter: open the book)" : "LOCAL AUDIO FOLDERS (Enter: open folder)")
                       : local_pane_label(total_width - 2);   // 2 columns stay free for the focus indicator
    size_t total = online ? online_view_.size() : playlists_mode ? playlist_view_.size()
                 : folders_mode ? folder_view_.size() : local_view_.size();
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);

    std::vector<std::string> out;
    // The list has the keys unless the search box is being typed into or the queue was focused (TAB).
    if (!queue_focus_ && mode_ != Mode::Search) label += focus_indicator(settings_, border_ansi);
    out.push_back(box_top(label, total_width, border_ansi));

    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll_ + row;
        std::string content;
        if (idx < static_cast<int>(total)) {
            if (online) {
                const auto& r = online_view_[idx];
                const int uploader_w = 18;
                int title_w = std::max(5, inner - idx_w - 2 - 2 - uploader_w);
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(r.title, settings_.font_map);
                std::string t_uploader = apply_font_map(r.uploader, settings_.font_map);
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_title, title_w), title_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_uploader, uploader_w), uploader_w);
            } else if (playlists_mode) {
                const auto& p = playlist_view_[idx];
                const int count_w = 10;
                int name_w = std::max(5, inner - idx_w - 2 - 2 - count_w);
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_name = apply_font_map(p.name, settings_.font_map);
                std::string t_count = std::to_string(p.track_count) + (p.track_count == 1 ? " track" : " tracks");
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_name, name_w), name_w) + settings_.list_separator + " "
                        + pad_right(t_count, count_w);
            } else if (folders_mode) {
                const auto& f = folder_view_[idx];
                const int parent_w = 16;
                const int count_w = 10;
                int name_w = std::max(5, inner - idx_w - 2 - 2 - parent_w - 2 - count_w);
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_name = apply_font_map(f.name, settings_.font_map);
                std::string t_parent = apply_font_map(f.parent, settings_.font_map);
                std::string t_count = std::to_string(f.track_count) + (audiobook_ ? (f.track_count == 1 ? " chapter" : " chapters") : (f.track_count == 1 ? " track" : " tracks"));
                if (audiobook_ && f.up) { t_name = "\u2190 .."; t_count.clear(); }
                else if (audiobook_ && f.container) { t_name = "\u25b8 " + t_name; t_count = std::to_string(f.sub_count) + (f.sub_count == 1 ? " book" : " books"); }   // a folder you walk into
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + (idx == selected_ ? marquee_or_truncate(t_name, name_w, idx + 1000000, marquee_row_idx_, marquee_since_)   // (the hovered folder / book scrolls when too long)
                                            : pad_right(truncate_str(t_name, name_w), name_w)) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_parent, parent_w), parent_w) + settings_.list_separator + " "
                        + pad_right(t_count, count_w);
            } else {
                const auto& t = local_view_[idx];
                const int artist_w = 16;
                const int dur_w = 5;
                int title_w = std::max(5, inner - idx_w - 2 - 2 - artist_w - 2 - dur_w);
                double dur = -1;
                // The artist comes from the file's own tag (probed lazily for visible rows, the same tag the
                // metadata panel shows). The parent-folder name is used only until the tag has been read.
                std::string artist = t.folder_artist;
                {
                    std::lock_guard<std::mutex> lk(row_meta_mutex_);
                    auto it = row_meta_cache_.find(path_utf8(t.path));
                    if (it != row_meta_cache_.end()) {
                        dur = it->second.duration_sec;
                        if (!it->second.artist.empty()) {
                            artist = it->second.artist;
                        } else if (it->second.tags_resolved) {
                            // A real probe already ran and confirmed this file
                            // has no artist tag -- same distinction the
                            // metadata panel now makes (see start_local_track).
                            // Without this, an untagged file kept showing the
                            // folder-name guess forever instead of "-", which
                            // just relocated the original bug into this list.
                            artist = "-";
                        }
                    }
                }
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(list_row_title(t.path, t.title), settings_.font_map);
                std::string t_artist = apply_font_map(artist, settings_.font_map);
                std::string t_dur = apply_font_map(fmt_mmss(dur), settings_.font_map);

                // Only the hovered row animates, and only when its title
                // is actually too long to fit -- every other row still
                // gets the same static truncate_str() as before, so
                // nothing about the rest of the list changes.
                std::string title_shown = (idx == selected_)
                    ? marquee_or_truncate(t_title, title_w, idx, marquee_row_idx_, marquee_since_)
                    : pad_right(truncate_str(t_title, title_w), title_w);
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(title_shown, title_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_artist, artist_w), artist_w) + settings_.list_separator + " "
                        + t_dur;
            }
        }
        bool sel = (idx == selected_) && idx < static_cast<int>(total);
        bool is_playing_row = has_track_ && list_source_ == ListSource::Local && idx < static_cast<int>(total)
                               && local_view_[idx].path == current_path_;
        std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else if (is_playing_row) {
            std::string playing_ansi = ansi_for(settings_.list_playing_color) + bg_ansi_for(settings_.list_playing_bg_color);
            out.push_back(bar + " " + playing_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }

    std::string footer;
    int remaining = static_cast<int>(total) - (scroll_ + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_queue_panel(int total_width, int height, bool wide_artist) const {
    if (audiobook_) return build_chapter_panel(total_width, height);   // (the audio book player has chapters instead of a queue)
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;
    // "locked" sits next to "focused" (and shows without focus too, because
    // the lock changes what playback does whether or not the pane has focus).
    // The focus indicator replaces the old "(focused)" note; "locked" shows with or without focus (the lock changes
    // what playback does whether or not the pane has the keys).
    std::string title = queue_locked_ ? "QUEUE (locked)" : "QUEUE";
    if (queue_focus_) title += focus_indicator(settings_, border_ansi);
    out.push_back(box_top(title, total_width, border_ansi));

    if (queue_.empty()) {
        int mid_row = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid_row) {
                std::string text = apply_font_map("ADD TRACKS TO QUEUE", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string queue_ansi = ansi_for(settings_.queue_color, false);
            out.push_back(bar + " " + queue_ansi + padded + "\x1b[0m " + bar);
        }
    } else {
        for (int row = 0; row < height; ++row) {
            int idx = queue_scroll_ + row;
            std::string content;
            bool is_row_playing = false;
            bool is_row_hovering = false;
            if (idx < static_cast<int>(queue_.size())) {
                const auto& q = queue_[idx];
                // The same information as the LOCAL AUDIO pane (title / name and artist), without the list number.
                std::string artist = q.artist;
                if (q.is_local) {   // the real artist tag once the list has probed the file, like the LOCAL AUDIO pane
                    std::lock_guard<std::mutex> lk(row_meta_mutex_);
                    auto it = row_meta_cache_.find(path_utf8(q.local_path));
                    if (it != row_meta_cache_.end()) {
                        if (!it->second.artist.empty()) artist = it->second.artist;
                        else if (it->second.tags_resolved) artist = "-";
                    }
                }
                if (artist.empty()) artist = "-";
                std::string t_title = apply_font_map(q.is_local ? list_row_title(q.local_path, q.title) : q.title, settings_.font_map);   // "Show meta data only" counts here too
                const std::string t_artist = apply_font_map(artist, settings_.font_map);
                const int sep_w = display_width(settings_.list_separator) + 1;
                const int artist_w = inner >= 34 ? (wide_artist && inner >= 80 ? 32 : 16) : inner >= 24 ? 10 : 0;   // (the big overlay: twice the room for the artist)
                if (artist_w == 0) content = t_title;
                else {
                    const int title_w = std::max(5, inner - artist_w - sep_w);
                    content = pad_right(truncate_str(t_title, title_w), title_w) + settings_.list_separator + " " + truncate_str(t_artist, artist_w);
                }
                is_row_playing = has_track_ && q.is_local && q.local_path == current_path_;
                is_row_hovering = queue_focus_ && (idx == queue_selected_);
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string color_ansi;
            const bool is_hit = wide_artist && !queue_find_.empty() && idx < static_cast<int>(queue_.size()) && queue_find_match(queue_find_, static_cast<size_t>(idx));
            if (is_row_hovering) color_ansi = cursor_sgr(settings_.queue_cursor_color, settings_.queue_cursor_bg_color);
            else if (is_hit) color_ansi = header_sgr(settings_);   // (a match of the search bar)
            else if (is_row_playing) color_ansi = ansi_for(settings_.queue_playing_color, true) + bg_ansi_for(settings_.queue_playing_bg_color);
            else color_ansi = ansi_for(settings_.queue_color, false) + bg_ansi_for(settings_.queue_inactive_bg_color);
            out.push_back(bar + " " + color_ansi + padded + "\x1b[0m " + bar);
        }
    }

    // Like the LOCAL AUDIO pane: how many titles are below the visible rows.
    std::string footer;
    const int remaining = static_cast<int>(queue_.size()) - (queue_scroll_ + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

} // namespace muisc
