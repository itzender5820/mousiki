// The radio's screen: draws the station list, the player and the overlays, and handles the radio's
// keys. It is the largest file of the radio mode.
#include "radio_ui.h"
#include "overlay_common.h"
#include "about_text.h"
#include "border_gradient.h"
#include "scope_big.h"
#include "scope_window.h"
#include "keyboard_layout.h"
#include "mode_switch.h"
#include "help_search.h"
#include "text_edit.h"
#include "translit.h"   // shown_text(): Latin letters on / off (the same switch as in the music player)
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <map>
#include <unordered_set>
#include "history.h"
#include "settings.h"
#include "radio_sign.h"
#include "sphere_visualizer.h"
#include "hifi_bars.h"
#include "bars_rows.h"
#include "terminal_ui.h"
#include "disk_art.h"      // the help wizard's welcome page
#include "wizard.h"        // its text, shared with the music player

namespace muisc::radio {

namespace {

constexpr const char* kReset = "\x1b[0m";

struct Seg { std::string ansi; std::string text; };

// A one-line key legend as segments: the keys in [square brackets] in `key` (the LEGEND colour, KEY-CMD), everything else in `cmd`
// (the COMMAND colour).
std::vector<Seg> legend_segs(const std::string& key, const std::string& cmd, const std::string& text) {
    std::vector<Seg> out;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '[') {
            const size_t e = text.find(']', i);
            const size_t stop = e == std::string::npos ? text.size() : e + 1;
            out.push_back({key, text.substr(i, stop - i)});
            i = stop;
        } else {
            size_t e = text.find('[', i);
            if (e == std::string::npos) e = text.size();
            out.push_back({cmd, text.substr(i, e - i)});
            i = e;
        }
    }
    return out;
}

// Colours + pads/truncates to exactly `width` visible columns. display_width()
// is only ever called on plain text; the ANSI lives in Seg::ansi.
std::string seg_line(const std::vector<Seg>& segs, int width) {
    std::string out;
    int used = 0;
    for (const auto& s : segs) {
        if (used >= width) break;
        std::string t = s.text;
        int w = display_width(t);
        if (used + w > width) { t = utf8_take(t, width - used); w = display_width(t); }
        if (!s.ansi.empty() && !t.empty()) out += s.ansi + t + kReset; else out += t;
        used += w;
    }
    if (used < width) out += std::string(static_cast<size_t>(width - used), ' ');
    return out;
}

std::string spaces(int n) { return std::string(static_cast<size_t>(std::max(0, n)), ' '); }

std::string repeat(const std::string& s, int n) {
    std::string o;
    for (int i = 0; i < n; ++i) o += s;
    return o;
}

// Look of the screen, resolved once per frame from the radio's own RadioSettings.
// Role names say what they colour:  header = captions / empty-pane messages / the sign while off air,
// legend = key command hints, error lines and cheatsheet titles (see radio_settings.h).
struct Style {
    const RadioSettings& c;
    std::string border, border_bottom, legend, legend_cmd, header, focus, key, val, list, list_playing, list_cursor,
                preset_inactive, preset_key, vol_current, vol_possible, freq_line, freq_mhz, search, search_idle;
    explicit Style(const RadioSettings& cfg) : c(cfg) {
        border = ansi_fg(c.border_top);
        border_bottom = ansi_fg(c.border_bottom);
        legend = ansi_fg(c.legend);
        legend_cmd = ansi_fg(c.legend_cmd);
        header = ansi_fg(c.header);
        focus = ansi_fg(c.focus);
        key = c.meta_key.empty() ? ansi_fg(c.list_fg) : ansi_fg(c.meta_key);
        val = c.meta_val.empty() ? ansi_fg(c.list_fg) : ansi_fg(c.meta_val);
        list = ansi_fg(c.list_fg) + ansi_bg(c.list_bg);
        // Search bar input: its own TEXT / BG cells, an empty one follows LIST INACTIVE.
        search = ansi_fg(c.search_fg.empty() ? c.list_fg : c.search_fg) + ansi_bg(c.search_bg.empty() ? c.list_bg : c.search_bg);
        search_idle = ansi_fg(c.search_inactive_fg.empty() ? c.list_fg : c.search_inactive_fg)
                    + ansi_bg(c.search_inactive_bg);   // no BG inherited: only the typed query is highlighted
        list_playing = ansi_fg(c.list_playing_fg) + ansi_bg(c.list_playing_bg);
        list_cursor = ansi_fg(c.list_cursor_fg) + ansi_bg(c.list_cursor_bg);
        if (list_cursor.empty()) list_cursor = "\x1b[7m"; // never let the cursor row be invisible
        preset_inactive = ansi_fg(c.preset_inactive_fg) + ansi_bg(c.preset_inactive_bg);
        preset_key = ansi_fg(c.preset_key_fg);
        vol_current = ansi_fg(c.volume_current);
        vol_possible = ansi_fg(c.volume_possible);
        freq_line = ansi_fg(c.freq_line);
        freq_mhz = ansi_fg(c.freq_mhz);
    }
    // ON AIR sign: upper left -> bottom right (t = 0 .. 1).
    std::string onair_at(float t) const { return gradient_fg(c.on_air_upper_left, c.on_air_bottom_right, t); }
    // FFT spectrum under the station info (3-stop if a centre colour is set).
    std::string viz_at(float t) const { return gradient3_fg(c.viz_left, c.viz_center, c.viz_right, t); }
    // Oscilloscope.
    std::string osci_at(float t) const { return gradient_fg(c.osci_left, c.osci_right, t); }
};

// The filled part of the VOLUME BAR as segments: one segment in the VOLUME CURRENT colour, or (Volume indicator -> gradient) one
// per position in the colours of the oscilloscope gradient from left to right.
std::vector<Seg> volume_fill_segs(const Style& s, int hashes) {
    const double angle = muisc::volume_clock_angle();
    if (!s.c.volume_gradient) return {{s.vol_current, muisc::volume_bar_fill(s.c.volume_indicator, hashes, angle)}};
    std::vector<Seg> out;
    const auto cells = muisc::volume_bar_cells(s.c.volume_indicator, hashes, angle);
    for (size_t i = 0; i < cells.size(); ++i)
        out.push_back({s.osci_at(static_cast<float>(i) / (muisc::kVolBarCells - 1)), cells[i]});
    return out;
}


// --- boxes (same construction as App::box_top / box_bottom / box_line) -------------------------
std::string box_edge(const Style& s, bool top, int w, const std::vector<Seg>& label, const std::string& ansi) {
    const std::string& l = top ? s.c.box_upper_left : s.c.box_lower_left;
    const std::string& r = top ? s.c.box_upper_right : s.c.box_lower_right;
    int label_w = 0;
    // A label may carry colour escapes (pane_title()'s header-coloured focus
    // triangle); display_width() would count their parameter text as columns
    // and shorten the border, so measure with the CSI sequences removed.
    for (const auto& g : label) {
        std::string plain;
        for (size_t k = 0; k < g.text.size(); ++k) {
            if (g.text[k] == '\x1b' && k + 1 < g.text.size() && g.text[k + 1] == '[') {
                k += 2;
                while (k < g.text.size() && !(g.text[k] >= 0x40 && g.text[k] <= 0x7e)) ++k;
                continue;
            }
            plain += g.text[k];
        }
        label_w += display_width(plain);
    }
    std::string o = ansi + l + s.c.box_horizontal;
    int used = display_width(l) + display_width(s.c.box_horizontal);
    if (!label.empty()) {
        o += " ";
        for (const auto& g : label) {
            if (g.ansi.empty()) {
                // A separator like " ─ " inside the label: the border colour goes in front of its dash (see below), or the dash
                // keeps the plain top/bottom colour when the smooth border gradient is on.
                size_t sp = 0;
                while (sp < g.text.size() && g.text[sp] == ' ') ++sp;
                if (!s.c.box_horizontal.empty() && g.text.compare(sp, s.c.box_horizontal.size(), s.c.box_horizontal) == 0)
                    o += g.text.substr(0, sp) + ansi + g.text.substr(sp);
                else
                    o += g.text;
            } else o += kReset + g.ansi + g.text + kReset + ansi;
        }
        // The border colour goes straight in front of the dashes: the smooth border gradient only recolours a border escape
        // that is followed by a border glyph, so an escape followed by this blank stayed the TOP colour and the right part
        // of the line (behind the title / focus triangle) no longer matched the left part.
        o += " " + ansi;
        used += label_w + 2;
    }
    const int dashes = std::max(0, w - used - display_width(r));
    o += repeat(s.c.box_horizontal, dashes) + r + kReset;
    return o;
}
std::string box_top(const Style& s, int w, const std::string& label, const std::string& ansi) {
    std::vector<Seg> lab;
    if (!label.empty()) lab.push_back({"", label});
    return box_edge(s, true, w, lab, ansi);
}
std::string box_bottom(const Style& s, int w, const std::string& label, const std::string& ansi) {
    std::vector<Seg> lab;
    if (!label.empty()) lab.push_back({"", label});
    return box_edge(s, false, w, lab, ansi);
}
// "│ content │" -- `content` is already exactly w-4 visible columns.
std::string box_line(const Style& s, const std::string& content, const std::string& ansi) {
    const std::string bar = ansi + s.c.box_vertical + kReset;
    return bar + " " + content + " " + bar;
}

// One text-field row of exactly `inner` visible columns: `prefix` + the field + an optional right-aligned
// `tail` (hint / result counter). While `focused` the field is painted by paint_edit_field() (caret as a
// block, marked range in reverse video, window scrolled to the caret); otherwise it is plain text. A
// `required` tail always gets its room (the field shrinks); an optional one only shows if it fits.
// `base` is the SGR the field is drawn in -- re-emitted after every reverse-video run.
std::string field_row(const std::string& base, const std::string& prefix, const std::string& text, const EditState& e,
                      bool focused, int inner, const std::string& tail, const std::string& tail_ansi, bool required,
                      const std::string& query_ansi = "") {
    const int pw = display_width(prefix);
    const int tw = display_width(tail);
    const int room = std::max(1, inner - pw - (required && tw > 0 ? tw + 1 : 0));
    std::string body;
    int cols;
    if (focused) {
        const EditPaint p = paint_edit_field(text, e, room, (text.empty() || query_ansi.empty()) ? base : query_ansi, true);
        body = p.s; cols = p.cols;
    } else {
        body = utf8_take(text, room);
        cols = display_width(body);
    }
    // Only a query that was actually typed is highlighted (behind its own length), the rest stays `base`.
    if (!query_ansi.empty() && !text.empty()) body = query_ansi + body + kReset + base;   // reset: `base` may carry no BG
    const int used = pw + cols;
    // The filler stays inside `base`, so a background colour fills the whole bar.
    std::string out = base + prefix + body;
    if (tw > 0 && (required || used + tw + 2 <= inner))
        out += spaces(inner - used - tw) + kReset + tail_ansi + tail + kReset;
    else
        out += spaces(inner - used) + kReset;
    return out;
}

// --- small glyph helpers ----------------------------------------------------------------------
const char* fft_glyph(int level) {
    switch (std::clamp(level, 0, 4)) {
        case 0: return " ";
        case 1: return "\u28C0";
        case 2: return "\u28E4";
        case 3: return "\u28F6";
        default: return "\u28FF";
    }
}

std::string dim_ansi(const std::string& ansi, float k) {
    int r, g, b;
    if (std::sscanf(ansi.c_str(), "\x1b[38;2;%d;%d;%dm", &r, &g, &b) != 3) return ansi; // palette colour: cannot scale
    auto sc = [k](int v) { return std::clamp(static_cast<int>(v * k), 0, 255); };
    return "\x1b[38;2;" + std::to_string(sc(r)) + ";" + std::to_string(sc(g)) + ";" + std::to_string(sc(b)) + "m";
}

std::string fmt_hms(double sec) {
    if (sec < 0) sec = 0;
    const int t = static_cast<int>(sec);
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    return b;
}

std::string fmt_mhz(double f) {
    char b[16];
    std::snprintf(b, sizeof b, "%.1f", f);
    return b;
}

// Cell canvas for the tuner band: per-cell text + colour, flattened into runs.
struct Canvas {
    std::vector<std::string> ch;
    std::vector<std::string> col;
    explicit Canvas(int w) : ch(static_cast<size_t>(w), " "), col(static_cast<size_t>(w)) {}
    int width() const { return static_cast<int>(ch.size()); }
    void put(int x, const std::string& g, const std::string& ansi) {
        if (x < 0 || x >= width()) return;
        ch[static_cast<size_t>(x)] = g; col[static_cast<size_t>(x)] = ansi;
    }
    void text(int x, const std::string& t, const std::string& ansi) {
        // ASCII only (labels, numbers, station names go through utf8_take first)
        for (size_t i = 0; i < t.size(); ++i) put(x + static_cast<int>(i), std::string(1, t[i]), ansi);
    }
    std::string flatten() const {
        std::string o, cur;
        for (size_t i = 0; i < ch.size(); ++i) {
            if (col[i] != cur) { if (!cur.empty()) o += kReset; cur = col[i]; o += cur; }
            o += ch[i];
        }
        if (!cur.empty()) o += kReset;
        return o;
    }
};

// ===========================================================================================
// INFO PANEL (rows: 15 = border + 13 + border). Columns inside the frame:
//    40 sign | 5 separator | 40 station info | 33 XY scope   = 118
// ===========================================================================================
constexpr int kPanelH = 14;
constexpr int kLeftW = kSignW + 1;  // 1 column margin + the sign
constexpr int kSepW = 5;
constexpr int kMetaW = 42;
// (the oscilloscope takes whatever width is left; see render_radio_frame)

std::vector<std::string> utf8_chars(const std::string& str) {
    std::vector<std::string> out;
    for (size_t i = 0; i < str.size();) {
        const unsigned char c = static_cast<unsigned char>(str[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        out.push_back(str.substr(i, n));
        i += n;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Circle-burst animation behind the ON AIR sign (from "Circle bust animation.json", 29 frames).
// Four rings centred on the panel: one hairline ring shrinks (frames 0-17), two hairline rings and
// one thick ring grow (frames 5-20, 14-29, 12-27). Drawn with braille dots in the cells the sign
// leaves empty, in the sign's own gradient colours but very dim, so the sign stays the star.
// ---------------------------------------------------------------------------------------------
constexpr float kCircleDim = 0.17f;        // brightness of the rings relative to the gradient (faint)
constexpr float kCircleFrames = 29.0f;     // length of the source animation

// AE keyframe easing used by all four rings: cubic-bezier(o = 0.167,0.167  i = 0.667,1) -> value 0..1
float bezier_ease(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    const float x1 = 0.167f, y1 = 0.167f, x2 = 0.667f, y2 = 1.0f;
    float lo = 0.0f, hi = 1.0f, t = x;
    for (int i = 0; i < 24; ++i) {
        t = 0.5f * (lo + hi);
        const float u = 1.0f - t;
        const float bx = 3.0f * u * u * t * x1 + 3.0f * u * t * t * x2 + t * t * t;
        if (bx < x) lo = t; else hi = t;
    }
    const float u = 1.0f - t;
    return 3.0f * u * u * t * y1 + 3.0f * u * t * t * y2 + t * t * t;
}

struct CircleRing {
    float t0, t1;        // keyframe frames
    float s0, s1;        // layer scale in percent at t0 / t1
    float visible_from;  // layer in-point
    float stroke_px;     // stroke width in comp pixels at 100 % scale
    bool  linear;        // Background4 uses a linear ramp, the others the eased one
};

// Comp 3840x2160, ring path diameter 217.094 * 7.8 = 1693.3 px (radius 846.7 px) at 100 %.
constexpr CircleRing kCircleRings[4] = {
    { 0.0f, 17.0f, 270.0f,   0.0f,  0.0f,  7.8f, true  },   // Background4: shrinks
    { 5.0f, 20.0f,   0.0f, 270.0f,  5.0f,  7.8f, false },   // Background3: grows
    {14.0f, 29.0f,   0.0f, 270.0f, 14.0f,  7.8f, false },   // Background6: grows
    {12.0f, 27.0f,   0.0f, 310.0f, 12.0f, 218.4f, false },  // Background5: grows, thick stroke
};

// Braille dots (2 x 4 per cell, square dots) lit by the rings at animation frame `f`.
// Returns one 0..255 braille mask per cell of a (cols x rows) area.
std::vector<unsigned char> circle_burst_mask(int cols, int rows, float f) {
    std::vector<unsigned char> mask(static_cast<size_t>(cols * rows), 0);
    const int dw = cols * 2, dh = rows * 4;
    const float k = static_cast<float>(dh) / 2160.0f;     // comp pixel -> dot (comp height fills the area)
    const float cx = dw * 0.5f, cy = dh * 0.5f;
    struct Live { float r, half; };
    Live live[4]; int n = 0;
    for (const CircleRing& c : kCircleRings) {
        if (f < c.visible_from) continue;
        const float e = c.linear ? std::clamp((f - c.t0) / (c.t1 - c.t0), 0.0f, 1.0f)
                                 : bezier_ease((f - c.t0) / (c.t1 - c.t0));
        const float sc = (c.s0 + (c.s1 - c.s0) * e) / 100.0f;
        if (sc <= 0.0005f) continue;
        live[n].r = 846.67f * sc * k;
        live[n].half = std::max(0.5f * c.stroke_px * sc * k, 0.62f); // never thinner than one dot
        ++n;
    }
    if (n == 0) return mask;
    static const unsigned char bit[2][4] = {{0x01, 0x02, 0x04, 0x40}, {0x08, 0x10, 0x20, 0x80}};
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) - cx;
            const float dy = (static_cast<float>(y) + 0.5f) - cy;
            const float dist = std::sqrt(dx * dx + dy * dy);
            for (int i = 0; i < n; ++i) {
                if (std::fabs(dist - live[i].r) <= live[i].half) {
                    mask[static_cast<size_t>((y / 4) * cols + (x / 2))] |= bit[x & 1][y & 3];
                    break;
                }
            }
        }
    }
    return mask;
}

void append_braille(std::string& out, unsigned char bits) {
    const int cp = 0x2800 + bits;
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
}

std::vector<std::string> build_left(const Style& s, const UiModel& m, const RadioStatus& st) {
    std::vector<std::string> rows(kPanelH, spaces(kLeftW));
    const bool live = st.state == StreamState::Live;
    const SignWave& w = m.wave;
    // Terminal cells are about twice as tall as wide, so x + 2y is a true 45-degree diagonal.
    const float denom = static_cast<float>(kSignW - 1) + 2.0f * static_cast<float>(kSignRows - 1);
    const float phase = static_cast<float>(m.t_sec * s.c.gradient_wave_speed);
    const float two_pi = 6.2831853f;
    constexpr int kSignTop = 1;   // one empty row above the sign
    // Faint circle burst behind the sign, only while the sign is lit. One loop = 1 / (wave speed * 1.25) s.
    std::vector<unsigned char> burst;
    if (live && s.c.pulse_wave) {
        const float loops = static_cast<float>(m.t_sec * s.c.pulse_wave_speed);
        const float frame = (loops - std::floor(loops)) * kCircleFrames;
        burst = circle_burst_mask(kLeftW, kPanelH, frame);
    }
    auto sign_colour = [&](int x, int y) {   // x, y in sign cells (may lie outside the sign: the gradient is clamped)
        const float d = (static_cast<float>(x) + 2.0f * static_cast<float>(y)) / denom;
        const float ripple = 0.5f + 0.5f * std::sin(two_pi * (d * w.waves - phase));
        float t = (1.0f - w.amount) * d + w.amount * ripple;
        t = std::round(std::clamp(t, 0.0f, 1.0f) * 64.0f) / 64.0f; // quantised: fewer colour runs
        return s.onair_at(t);
    };
    auto burst_at = [&](int col, int row) -> unsigned char {   // col/row inside the left panel
        if (burst.empty() || col < 0 || col >= kLeftW || row < 0 || row >= kPanelH) return 0;
        return burst[static_cast<size_t>(row * kLeftW + col)];
    };
    // Appends the burst glyph for panel cell (col,row), or a blank. (sx,sy) = the same cell in sign coordinates.
    auto put_burst = [&](std::string& line, std::string& last, int col, int row, int sx, int sy) {
        const unsigned char b = burst_at(col, row);
        if (b == 0) { line += ' '; return; }
        const std::string a = dim_ansi(sign_colour(sx, sy), kCircleDim);
        if (a != last) { line += a; last = a; }
        append_braille(line, b);
    };
    // Rows without any sign (top row, rows under the sign): burst only.
    if (!burst.empty()) {
        for (int row = 0; row < kPanelH; ++row) {
            const int sy = row - kSignTop;
            if (sy >= 0 && sy < kSignRows) continue;   // sign rows are built below
            std::string line, last;
            for (int col = 0; col < kLeftW; ++col) put_burst(line, last, col, row, col - 1, sy);
            if (!last.empty()) line += kReset;
            rows[static_cast<size_t>(row)] = line;
        }
    }
    for (int y = 0; y < kSignRows && y + kSignTop < kPanelH; ++y) {
        const auto cells = utf8_chars(live ? kOnAirSign[y] : kOffAirSign[y]);
        std::string line;
        std::string last;
        put_burst(line, last, 0, y + kSignTop, -1, y);   // margin column in front of the sign
        for (int x = 0; x < kSignW; ++x) {
            const std::string g = (s.c.on_air_ascii && x < static_cast<int>(cells.size())) ? cells[static_cast<size_t>(x)] : " ";
            if (g == "\u2800" || g == " ") {
                put_burst(line, last, x + 1, y + kSignTop, x, y);
                continue;
            }
            const std::string ansi = sign_colour(x, y);   // the OFF AIR sign wears the same colour wave (only the pulse wave stays off)
            if (ansi != last) { line += ansi; last = ansi; }
            line += g;
        }
        if (!last.empty()) line += kReset;
        rows[static_cast<size_t>(y + kSignTop)] = line;
    }

    // caption under the sign
    std::string cap;
    if (live) cap.clear();   // nothing under the sign while it is lit
    else switch (st.state) {
        case StreamState::Connecting:   cap = "tuning ..."; break;
        case StreamState::Buffering: {
            const int pct = std::clamp(static_cast<int>(st.buffer_sec / 1.5 * 100.0), 0, 99);
            cap = "buffering " + std::to_string(pct) + "%"; break;
        }
        case StreamState::Reconnecting: cap = "reconnecting (" + std::to_string(st.reconnects) + ")"; break;
        case StreamState::Failed:       cap = "no signal"; break;
        default:                        cap.clear(); break;   // idle: the OFF AIR sign says it
    }
    if (!live && s.c.on_air_ascii && !cap.empty()) {   // while live (or idle) the caption is empty and the bottom row keeps the burst
        const int pad = std::max(0, (kLeftW - display_width(cap)) / 2);
        rows[kPanelH - 1] = seg_line({{"", spaces(pad)}, {s.header, cap}}, kLeftW);
    }
    return rows;
}

std::vector<std::string> build_meta(const Style& s, const RadioStatus& st, const std::vector<int>* viz_bars,
                                    const std::vector<std::string>* hifi_lines = nullptr) {
    const int bar_rows = hifi_lines ? static_cast<int>(hifi_lines->size()) : 2;   // text rows the spectrum takes at the bottom
    std::vector<std::string> rows(kPanelH, spaces(kMetaW));
    int cursor = 1;
    auto kv = [&](const std::string& label, const std::string& value, int max_lines = 1) {
        const int room = std::max(1, (kPanelH - bar_rows) - cursor);
        auto lines = wrap_lines(value.empty() ? "-" : value, kMetaW - 12, std::min(max_lines, room));
        if (lines.empty()) lines.push_back("");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (cursor >= kPanelH - bar_rows) break; // never write over the spectrum rows
            const bool first = i == 0;
            rows[static_cast<size_t>(cursor++)] = seg_line({{first ? s.key : "", pad_right(first ? label : "", 10)},
                                                            {"", first ? ": " : "  "}, {s.val, lines[i]}}, kMetaW);
        }
    };
    const StreamInfo& in = st.info;
    const bool have = st.state != StreamState::Idle;
    std::string bitrate;
    if (in.bitrate_kbps > 0) bitrate = std::to_string(in.bitrate_kbps) + " kbps" + (in.codec.empty() ? "" : " " + in.codec);
    else bitrate = in.codec;
    std::string rate;
    if (in.sample_rate > 0) {
        char b[24];
        std::snprintf(b, sizeof b, in.sample_rate % 1000 == 0 ? "%.0f kHz" : "%.1f kHz", in.sample_rate / 1000.0);
        rate = b;
    }
    const std::string country = st.info.country;

    if (have) {
        kv("Station", shown_text(in.station));
        kv("Title", shown_text(in.title), 2);
        kv("Artist", in.artist);
        kv("Genre", in.genre);
        kv("Bitrate", bitrate);
        kv("Sampling", rate);
        kv("Type", in.channels == 1 ? "live stream (mono)" : "live stream");
        kv("Country", country);
        kv("Stream", in.host);
    } else {
        rows[kPanelH / 2] = seg_line({{s.list, spaces((kMetaW - 25) / 2) + "Currently No Station Tuned"}}, kMetaW);
    }

    // FFT spectrum on the bottom two rows -- same bars/glyphs/gradient as the music player.
    if (s.c.element_visualizer && hifi_lines) {
        for (int r = 0; r < bar_rows; ++r) {
            std::string line = (*hifi_lines)[static_cast<size_t>(r)] + kReset;
            rows[static_cast<size_t>(kPanelH - bar_rows + r)] = line;
        }
    } else if (s.c.element_visualizer && viz_bars) {
        const std::vector<int>& bars = *viz_bars;
        std::string top, bottom;
        const int n = static_cast<int>(bars.size());
        for (int i = 0; i < n; ++i) {
            const float t = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
            const std::string ansi = s.viz_at(t);
            bottom += ansi + fft_glyph(std::min(bars[static_cast<size_t>(i)], 4));
            top += ansi + fft_glyph(std::max(0, bars[static_cast<size_t>(i)] - 4));
        }
        top += kReset; bottom += kReset;
        const std::string tail = spaces(kMetaW - n);
        rows[kPanelH - 2] = top + tail;
        rows[kPanelH - 1] = bottom + tail;
    }
    return rows;
}

// Scroll window of a list, like the music player's: the window only moves when the cursor leaves it (the cursor
// walks inside the window up and down; it does not stay glued to the bottom row). One state per list (`key`).
int follow_scroll(const void* key, int cursor, int rows, int total) {
    static std::map<const void*, int> state;
    int& first = state[key];
    if (cursor < first) first = cursor;
    if (cursor >= first + rows) first = cursor - rows + 1;
    first = std::clamp(first, 0, std::max(0, total - rows));
    return first;
}
inline const void* scroll_id(int n) { return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(n)); }


bool parse_rgb(const std::string& ansi, int& r, int& g, int& b) {
    return std::sscanf(ansi.c_str(), "\x1b[38;2;%d;%d;%dm", &r, &g, &b) == 3;
}

// Temperature / frequency colour: slow beam (low pitch) = deep red, through orange and yellow-white, to a cold blue for fast (high pitch).
void temperature_rgb(float t, int& r, int& g, int& b) {
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

// Colour of palette `id` (see kOsciPaletteNames) at t = 0..1. 0 and 1 use the OSCI gradient of the COLORS tab (0 spreads it over the
// picture from left to right, 1 over the beam speed), the others are fixed designs over the beam speed.
void palette_rgb(const Style& s, int id, float t, int& r, int& g, int& b) {
    static const float kStops[5][5][3] = {
        {{20, 205, 120}, {30, 225, 205}, {60, 160, 255}, {150, 95, 255}, {235, 80, 205}},      // aurora
        {{95, 35, 150}, {170, 45, 140}, {235, 70, 105}, {255, 150, 55}, {255, 232, 150}},      // magma
        {{45, 95, 235}, {60, 170, 255}, {120, 230, 255}, {210, 250, 255}, {255, 255, 255}},    // ice
        {{0, 255, 220}, {0, 150, 255}, {130, 65, 255}, {255, 45, 205}, {255, 100, 120}},       // neon
        {{255, 70, 70}, {255, 175, 45}, {235, 235, 70}, {60, 225, 120}, {85, 160, 255}}};      // spectrum (the last stop is cut off below)
    t = std::clamp(t, 0.0f, 1.0f);
    if (id <= 1) {
        if (!parse_rgb(s.osci_at(t), r, g, b)) r = g = b = 220;
        return;
    }
    if (id == 2) { temperature_rgb(t, r, g, b); return; }
    if (id == 8) { r = 70; g = 255; b = 90; return; }   // classic green: the phosphor of an old scope, brightness alone makes the picture
    const int k = std::clamp(id - 3, 0, 4);
    const float f = t * 4.0f;
    const int i = std::min(3, static_cast<int>(f));
    const float u = f - static_cast<float>(i);
    r = static_cast<int>(kStops[k][i][0] + (kStops[k][i + 1][0] - kStops[k][i][0]) * u);
    g = static_cast<int>(kStops[k][i][1] + (kStops[k][i + 1][1] - kStops[k][i][1]) * u);
    b = static_cast<int>(kStops[k][i][2] + (kStops[k][i + 1][2] - kStops[k][i][2]) * u);
}

std::string rgb_seq(int r, int g, int b, float k) {
    auto sc = [k](int v) { return std::clamp(static_cast<int>(static_cast<float>(v) * k), 0, 255); };
    return "\x1b[38;2;" + std::to_string(sc(r)) + ";" + std::to_string(sc(g)) + ";" + std::to_string(sc(b)) + "m";
}

std::string utf8_cp(int cp) {
    std::string o;
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    return o;
}

RadioScope::Params scope_params(const RadioSettings& c, double dt, int style = -1) {   // style: 0 braille, 1 image, -1 the one in use
    const OsciSet& o = style < 0 ? c.osci() : c.osci_set[style == 1 ? 1 : 0];
    RadioScope::Params p;
    // the afterglow is set per frame at 30 fps: scale it by the real frame time so a higher frame rate keeps the same look
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
    p.mode = c.osci_mode;
    if (o.music) { p.mono_phase = false; p.fixed_gain = true; }   // oscilloscope music mode: the exact figure
    p.crt = o.crt;
    p.low_rate_blank = o.low_rate;
    p.dwell = o.dwell;
    p.rough = o.rough;
    return p;
}

// The scope as a terminal image (ANIMA/VIZ -> Osci style: image): a real pixel picture the terminal draws itself. Here the
// pixels are made and left in m.gfx; the cells stay blank and render_radio_frame() adds the position. Returns false when the
// style is braille or the terminal has no graphics protocol.
bool build_scope_image(const Style& s, const UiModel& m, RadioEngine& engine, int scope_w) {
    if (s.c.osci_style != 1 || m.gfx_proto == GfxProto::None || scope_w <= 0) return false;
    GfxFrame& g = m.gfx;
    // pixels per cell, capped so a big terminal does not mean a huge picture (the terminal scales it up)
    const int cw0 = std::max(4, m.cell_w), ch0 = std::max(8, m.cell_h);
    const int cwq = std::clamp(osci_detail_cap(s.c.image_detail, false) / scope_w, 4, cw0);
    // Image resolution (SHIFT+o): render 1/scale of it; Kitty scales the picture up itself, Sixel repeats its pixels up to
    // the real cell size (out_cw x out_ch). Less to render, encode and send per frame.
    g.scale = std::clamp(s.c.image_scale, 1, 3);
    g.out_cw = cw0; g.out_ch = ch0;
    // 1/scale of the real cell pixels, but never more than the width cap above (cwq)
    const int cws = std::max(2, std::min(cwq, cw0 / g.scale));
    const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
    g.cols = scope_w; g.rows = kPanelH; g.w = scope_w * cws; g.h = kPanelH * chs;
    g.id = 0; g.grid.clear(); g.cut_w = 0;   // no grid on the main screen (the big overlay sets one on the same frame object)
    engine.scope().render_image(g.w, g.h, scope_params(s.c, m.dt), g.level, g.hue);
    const int pal = s.c.osci().palette;
    if (pal == 0) {   // OSCI gradient over the picture: the colour follows the horizontal position, like the braille scope
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x)
                g.hue[static_cast<size_t>(y) * g.w + x] = static_cast<uint8_t>(x * 255 / std::max(1, g.w - 1));
    }
    for (int h = 0; h < 256; ++h) {
        int r = 255, gr = 255, b = 255;
        palette_rgb(s, pal, static_cast<float>(h) / 255.0f, r, gr, b);
        g.pal[static_cast<size_t>(h)] = {static_cast<uint8_t>(r), static_cast<uint8_t>(gr), static_cast<uint8_t>(b)};
    }
    g.active = true;
    g.fresh = m.gfx_due;
    return true;
}

std::vector<std::string> build_scope(const Style& s, const UiModel& m, RadioEngine& engine, bool idle, int scope_w) {
    std::vector<std::string> rows(kPanelH, spaces(scope_w));
    if (idle) return rows; // nothing playing: no phosphor dot hovering in the corner
    if (build_scope_image(s, m, engine, scope_w)) {   // placeholder cells: the main loop writes them as spaces or skips them
        for (auto& r : rows) r = gfx_hole(scope_w);
        return rows;
    }
    auto cells = engine.scope().render(scope_w, kPanelH, scope_params(s.c, m.dt));
    const int pal = s.c.osci().palette;
    for (int i = 0; i < kPanelH && i < static_cast<int>(cells.size()); ++i) {
        std::string line, last;
        for (int x = 0; x < scope_w; ++x) {
            const auto& c = cells[static_cast<size_t>(i)][static_cast<size_t>(x)];
            if (c.braille == 0) { line += ' '; continue; }
            const float t = scope_w > 1 ? static_cast<float>(x) / static_cast<float>(scope_w - 1) : 0.0f;
            const float lvl = std::round(c.level / 255.0f * 7.0f) / 7.0f;
            int r = 255, g = 255, b = 255;
            std::string a;
            if (pal == 0) a = dim_ansi(s.osci_at(t), 0.30f + 0.70f * lvl);
            else { palette_rgb(s, pal, c.hue / 255.0f, r, g, b); a = rgb_seq(r, g, b, 0.30f + 0.70f * lvl); }
            if (a != last) { line += a; last = a; }
            line += utf8_cp(0x2800 + c.braille);
        }
        rows[static_cast<size_t>(i)] = line + kReset;
    }
    return rows;
}

// The sphere (ANIMA/VIZ -> Osci/sphere): the player's audio-reactive braille sphere, in the scope's place.
// The satellite shown in the free pane on the right while nothing is live: braille art rotated about its centre, swinging
// +-25 degrees. The angle follows a sine, so it slows down towards both ends like a pendulum. Rotation is done on the dot
// grid (a braille cell is 2 x 4 dots, and since a terminal cell is about twice as tall as wide the dots are square, so
// the rotation is not distorted): every output dot samples the source bitmap bilinearly at the back-rotated position.
std::vector<std::string> build_satellite(const Style& s, double t_sec, int w) {
    std::vector<std::string> rows(kPanelH, spaces(w));
    static std::vector<unsigned char> src;       // kSatRows*4 x kSatW*2 dots, 1 = set
    static int sw = 0, sh = 0;
    if (src.empty()) {
        sw = kSatW * 2; sh = kSatRows * 4;
        src.assign(static_cast<size_t>(sw * sh), 0);
        for (int cy = 0; cy < kSatRows; ++cy) {
            const auto cells = utf8_chars(kSatellite[cy]);
            for (int cx = 0; cx < kSatW && cx < static_cast<int>(cells.size()); ++cx) {
                const std::string& g = cells[static_cast<size_t>(cx)];
                if (g.size() != 3) continue;
                const int cp = ((static_cast<unsigned char>(g[0]) & 0x0F) << 12) | ((static_cast<unsigned char>(g[1]) & 0x3F) << 6) | (static_cast<unsigned char>(g[2]) & 0x3F);
                const int bits = cp - 0x2800;
                if (bits <= 0 || bits > 0xFF) continue;
                // braille dot bits: 1,2,3 = left column rows 0-2; 4,5,6 = right column rows 0-2; 7 = left row 3; 8 = right row 3
                static const int dx[8] = {0, 0, 0, 1, 1, 1, 0, 1}, dy[8] = {0, 1, 2, 0, 1, 2, 3, 3};
                for (int b = 0; b < 8; ++b)
                    if (bits & (1 << b)) src[static_cast<size_t>((cy * 4 + dy[b]) * sw + cx * 2 + dx[b])] = 1;
            }
        }
    }
    constexpr double kPi = 3.14159265358979;
    const double period = 9.0;                                          // seconds for a full swing there and back
    const double ang = 25.0 * std::sin(2.0 * kPi * t_sec / period) * kPi / 180.0;
    const double ca = std::cos(ang), sa = std::sin(ang);
    const int ow = w * 2, oh = kPanelH * 4;                              // output dot grid
    const double scx = sw / 2.0, scy = sh / 2.0, ocx = ow / 2.0, ocy = oh / 2.0;
    auto sample = [&](double x, double y) -> double {                   // bilinear, outside = 0
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const double fx = x - x0, fy = y - y0;
        auto at = [&](int xx, int yy) { return (xx < 0 || yy < 0 || xx >= sw || yy >= sh) ? 0.0 : static_cast<double>(src[static_cast<size_t>(yy * sw + xx)]); };
        return at(x0, y0) * (1 - fx) * (1 - fy) + at(x0 + 1, y0) * fx * (1 - fy) + at(x0, y0 + 1) * (1 - fx) * fy + at(x0 + 1, y0 + 1) * fx * fy;
    };
    static const int bit_of[4][2] = {{0, 3}, {1, 4}, {2, 5}, {6, 7}};   // [dot row][dot col] -> braille bit
    for (int cy = 0; cy < kPanelH; ++cy) {
        std::string line, last;
        for (int cx = 0; cx < w; ++cx) {
            int bits = 0;
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const double px = cx * 2 + dx + 0.5 - ocx, py = cy * 4 + dy + 0.5 - ocy;
                    // back-rotate (the picture is rotated by +ang, so the source position is rotated by -ang)
                    const double qx = ca * px + sa * py + scx - 0.5, qy = -sa * px + ca * py + scy - 0.5;
                    if (sample(qx, qy) >= 0.5) bits |= 1 << bit_of[dy][dx];
                }
            if (bits == 0) { line += ' '; continue; }
            const float t = w > 1 ? static_cast<float>(cx) / static_cast<float>(w - 1) : 0.0f;
            const std::string a = s.osci_at(std::round(t * 32.0f) / 32.0f);
            if (a != last) { line += a; last = a; }
            append_braille(line, static_cast<unsigned char>(bits));
        }
        rows[static_cast<size_t>(cy)] = line + kReset;
    }
    return rows;
}

std::vector<std::string> build_sphere(const Style& s, const std::vector<int>& bars, double dt, int w) {
    static SphereVisualizer sphere;   // keeps its rotation between frames
    std::vector<std::string> rows(kPanelH, spaces(w));
    const auto lines = sphere.render(w, kPanelH, bars, dt);
    const std::string ansi = s.osci_at(0.5f);
    for (int i = 0; i < kPanelH && i < static_cast<int>(lines.size()); ++i)
        rows[static_cast<size_t>(i)] = ansi + pad_right(lines[static_cast<size_t>(i)], w) + kReset;
    return rows;
}

// ===========================================================================================
// TUNER BAND (5 rows, 84 wide) + buttons / volume (gap 1 + 35)
// ===========================================================================================
constexpr int kSideW = 33;   // right of the band: buttons 3 x 11 = 33, volume line 33
constexpr int kBandGap = 1;  // blank column between the band's right border and the buttons / volume bar
constexpr int kBtnW = 11;                            // three buttons = 33 columns
constexpr int kVolBar = 15;                          // volume bar cells
constexpr double kBandLo = 87.5, kBandHi = 108.0;

int dial_x(double mhz, int w) {
    return static_cast<int>(std::lround((mhz - kBandLo) / (kBandHi - kBandLo) * (w - 1)));
}

std::vector<std::string> build_tuner(const Style& s, const UiModel& m, const RadioStatus& st, int tuner_w) {
    const int inner = tuner_w - 4;
    // The label row is one cell wider than the rest: "108" is centred on its tick and its last digit
    // sits in the padding column in front of the right border.
    const int span = inner - 1;   // the ticks span one cell less than the box: a closing "\u2500" follows the last one
    Canvas labels(inner + 1), scale(inner), marks(inner);
    for (int f = 88; f <= 108; ++f) {
        const int x = dial_x(f, span);
        const bool major = (f % 4 == 0);
        scale.put(x, major ? "\u2566" : "\u252C", s.freq_line);
        if (major) {
            const std::string t = std::to_string(f);
            labels.text(std::clamp(x - static_cast<int>(t.size()) / 2, 0, inner + 1 - static_cast<int>(t.size())), t, s.freq_mhz);
        }
    }
    for (int x = 0; x < inner; ++x)
        if (scale.ch[static_cast<size_t>(x)] == " ") scale.put(x, "\u2500", s.freq_line);
    scale.put(0, " ", "");   // the line starts one cell later: the leftmost "\u2500" is gone (88 and all ticks stay put)

    if (m.stations) {
        for (size_t i = 0; i < m.stations->size(); ++i) {
            const Station& stn = (*m.stations)[i];
            const bool preset = preset_slot_of(m.presets, static_cast<int>(i)) >= 0;
            marks.put(dial_x(stn.dial_mhz, span), preset ? "\u2022" : "\u00B7", preset ? s.border : s.freq_mhz);
        }
        if (st.state != StreamState::Idle && st.dial_mhz > 0.0) {   // also for a Radio Browser station that is not in the list
            const int x = dial_x(st.dial_mhz, span);
            const std::string needle = s.onair_at(0.0f);
            marks.put(x, "\u25B2", needle);
            const std::string label = fmt_mhz(st.dial_mhz) + "  " + utf8_take(shown_text(st.tuned_name), 28);
            const int lw = display_width(label);
            const int lx = (x + 2 + lw <= inner) ? x + 2 : x - 1 - lw;
            // label text: names can be non-ASCII, so write it as one run via per-cell put of display columns
            std::string rest = label;
            int cx = lx;
            while (!rest.empty()) {
                const unsigned char c0 = static_cast<unsigned char>(rest[0]);
                const size_t n = c0 < 0x80 ? 1 : (c0 >> 5) == 6 ? 2 : (c0 >> 4) == 14 ? 3 : 4;
                marks.put(cx, rest.substr(0, std::min(n, rest.size())), s.val);
                cx += 1; rest.erase(0, std::min(n, rest.size()));
            }
        }
    }

    // footer:  ▂▃▅▇ signal ─ STEREO ─ ● LIVE 00:14:32
    int level = 0;
    if (st.state == StreamState::Live || st.state == StreamState::Buffering)
        level = st.buffer_sec < 0.5 ? 1 : st.buffer_sec < 1.5 ? 2 : st.buffer_sec < 3.0 ? 3 : 4;
    const char* bars[4] = {"\u2582", "\u2583", "\u2585", "\u2587"};
    const bool blink = static_cast<int>(m.t_sec) % 2 == 0;
    std::string state_txt, lamp;
    const std::string lampc = s.onair_at(0.0f);
    std::string state_col = s.border;
    switch (st.state) {
        case StreamState::Live:
        case StreamState::Buffering:
            if (st.timeshift_paused) { lamp = "\u2016"; state_txt = "PAUSED -" + fmt_hms(st.behind_sec); state_col = lampc; break; }
            if (st.timeshift_shifted && st.behind_sec >= 2.5) { lamp = "\u25D1"; state_txt = "TIMESHIFT -" + fmt_hms(st.behind_sec); state_col = lampc; break; }
            if (st.state == StreamState::Buffering) { lamp = "\u25D0"; state_txt = "BUFFERING"; break; }
            lamp = blink ? "\u25CF" : "\u25CB"; state_txt = "LIVE " + fmt_hms(st.listening_sec); state_col = lampc; break;
        case StreamState::Connecting:   lamp = "\u25CC"; state_txt = "CONNECTING"; break;
        case StreamState::Reconnecting: lamp = "\u21BB"; state_txt = "RECONNECTING"; break;
        case StreamState::Failed:       lamp = "\u2716"; state_txt = "OFFLINE"; break;
        default:                        lamp = "\u25CB"; state_txt = "IDLE"; break;
    }
    const std::string ts = s.border;
    std::vector<Seg> foot;
    for (int i = 0; i < 4; ++i) foot.push_back({i < level ? ts : s.header, bars[i]});
    foot.push_back({ts, " signal "});
    foot.push_back({"", s.c.box_horizontal + " "});
    foot.push_back({ts, st.info.channels == 1 || !s.c.stereo ? "MONO" : "STEREO"});
    foot.push_back({"", " " + s.c.box_horizontal + " "});
    if (st.recording) {   // key y
        foot.push_back({"\x1b[1;31m", std::string(blink ? "\u25CF" : "\u25CB") + " REC " + fmt_hms(st.recording_sec)});
        foot.push_back({"", " " + s.c.box_horizontal + " "});
    }
    foot.push_back({state_col, lamp + " " + state_txt});

    std::vector<std::string> out;
    out.push_back(box_top(s, tuner_w, "FREQUENCY BAND (MHz)", s.border));
    out.push_back(s.border + s.c.box_vertical + kReset + " " + labels.flatten() + s.border + s.c.box_vertical + kReset);
    out.push_back(box_line(s, scale.flatten(), s.border));
    out.push_back(box_line(s, marks.flatten(), s.border));
    out.push_back(box_edge(s, false, tuner_w, foot, s.border_bottom));
    return out;
}

std::vector<std::string> build_side(const Style& s, const RadioStatus& st) {
    const int bw = kBtnW;
    auto center = [](const std::string& t, int w) {
        const int pad = std::max(0, w - display_width(t));
        return spaces(pad / 2) + t + spaces(pad - pad / 2);
    };
    auto mid = [&](const std::string& t) {
        return box_line(s, s.border + center(t, bw - 4) + kReset, s.border);
    };
    const std::string tail = spaces(kSideW - 3 * bw);
    std::vector<std::string> out(5);
    if (s.c.dummy_buttons) {
        out[0] = box_top(s, bw, "", s.border) + box_top(s, bw, "", s.border) + box_top(s, bw, "", s.border) + tail;
        out[1] = mid("<<<") + mid(st.muted ? "TUNE" : "MUTE") + mid(">>>") + tail;
        out[2] = box_bottom(s, bw, "", s.border_bottom) + box_bottom(s, bw, "", s.border_bottom) + box_bottom(s, bw, "", s.border_bottom) + tail;
    } else {
        out[0] = out[1] = out[2] = spaces(kSideW);
    }
    const int hashes = (st.volume * kVolBar) / 100;
    std::vector<Seg> vol_segs = {{s.border, "VOLUME BAR:["}};
    for (const Seg& g : volume_fill_segs(s, hashes)) vol_segs.push_back(g);   // the look and the gradient of the Volume indicator
    vol_segs.push_back({s.vol_possible, std::string(static_cast<size_t>(kVolBar - hashes), '-')});
    vol_segs.push_back({s.border, "]"});
    vol_segs.push_back({"", " " + std::to_string(st.volume) + "%"});
    out[3] = seg_line(vol_segs, kSideW);
    out[4] = spaces(kSideW);
    return out;
}

// ===========================================================================================
// SEARCH (3 rows) and LISTS (7 rows)
// ===========================================================================================
std::vector<std::string> build_search(const Style& s, const UiModel& m, const RadioStatus& st, int W) {
    const int w = W - 8; // then a 1-col gap and the 7-wide mode box ("S" shuffle / "L" list)
    const int inner = w - 4;
    // The command legend lives in the cheat sheet ('?'). Only the audio
    // error stays: that is a status the user has to see, not a key legend.
    std::string hint;
    if (!st.device_ok) hint = "audio: " + (st.device_error.empty() ? std::string("no output device") : st.device_error) + "  [?] cheatsheet  [q] quit";
    else if (!m.notice.empty()) hint = m.notice;
    const std::string base = s.search_idle;
    const std::string row = field_row(base, m.search_lists ? "/p: " : "/s: ", m.search, m.search_edit, m.search_focus, inner, hint, s.legend, false, s.search);
    std::vector<std::string> out(3);
    const std::string btn_top = box_top(s, 7, "", s.border);
    std::string title = m.search_lists ? "SEARCH LISTS OF STATIONS" : "SEARCH STATIONS";
    if (m.sleep_running > 0) {
        const long long t = std::max(0LL, static_cast<long long>(m.sleep_left));
        char b[40];
        if (t >= 3600) std::snprintf(b, sizeof b, "  [SLEEP %lld:%02lld:%02lld]", t / 3600, (t / 60) % 60, t % 60);
        else std::snprintf(b, sizeof b, "  [SLEEP %lld:%02lld]", t / 60, t % 60);
        title += b;
    }
    out[0] = box_top(s, w, title, s.border) + " " + btn_top;
    out[1] = box_line(s, row, s.border) + " " + box_line(s, s.border + (m.shuffle ? " S " : " L ") + kReset, s.border);
    out[2] = box_bottom(s, w, "", s.border_bottom) + " " + box_bottom(s, 7, "", s.border_bottom);
    return out;
}

// Marquee for the hovered row, same math and speed as App::marquee_or_truncate (keep them in sync):
// hold on the start for 1.2 s, then scroll 4 columns per second, looping with a 4-column gap.
// Rows that fit, and every row that is not hovered, get the plain truncate_str() ("...") instead.
std::string marquee_or_truncate(int& marquee_row, double& marquee_since, double now, const std::string& text, int width, int row_id) {
    if (row_id != marquee_row) { marquee_row = row_id; marquee_since = now; }
    if (display_width(text) <= width || !marquee_enabled()) return pad_right(truncate_str(text, width), width);   // fits, or scrolling is off
    const double hold_secs = 1.2;
    const double cols_per_sec = 4.0;
    const std::string loop_text = text + "    ";
    const int period = display_width(loop_text);
    const double elapsed = now - marquee_since;
    int start_col = 0;
    if (elapsed > hold_secs && period > 0) start_col = static_cast<int>((elapsed - hold_secs) * cols_per_sec) % period;
    return pad_right(utf8_skip_take(loop_text + loop_text + loop_text, start_col, width), width);
}

// STATIONS | PRESETS split the full width 50/50 like the player's list | queue (1 column gap between
// the boxes, the odd column goes to PRESETS). `list_rows` = content rows of both boxes.
std::vector<std::string> build_lists(const Style& s, const UiModel& m, const RadioStatus& st, int W, int list_rows) {
    const std::vector<Station> none;
    const auto& stations = m.stations ? *m.stations : none;
    const int left_w = (W - 1) / 2;
    const int right_w = W - 1 - left_w;
    const int vis_n = static_cast<int>(m.visible.size());
    const int cursor = std::clamp(m.cursor, 0, std::max(0, vis_n - 1));
    const int first = follow_scroll(scroll_id(1), cursor, list_rows, vis_n);

    std::vector<std::string> left;
    // Title: the sort (SHIFT+t); while a station list is shown instead the small reminder that this is not the full list.
    // (The search text is not repeated here any more -- it is in the search box.)
    std::string pane_label;
    if (m.search_lists) pane_label = "STATION LISTS (ENTER: show the stations)";
    else if (!m.active_list.empty()) pane_label = "STATIONS (LIST: " + utf8_take(m.active_list, 24) + " - ESC: all)";
    else pane_label = std::string("STATIONS (") + (m.sort_az ? "sort: A-Z" : "sort: list order") + ")";   // SHIFT+t
    left.push_back(box_top(s, left_w, pane_label, s.border));
    const int inner = left_w - 4;
    // "NNN| name | genre | tail": number, separators and the 10-column tail are fixed, name/genre share the rest.
    const int tail_w = 8;   // "128k AAC"
    // While a station list is shown the rows are numbered by their position IN THE LIST, not by the station's number overall.
    const StationList* shown_list = nullptr;
    if (!m.active_list.empty()) for (const auto& l : m.lists) if (l.name == m.active_list) { shown_list = &l; break; }
    const int idx_w = static_cast<int>(std::to_string(std::max<size_t>(1, shown_list ? shown_list->items.size() : stations.size())).size()); // widest number
    const int flex = std::max(8, inner - (idx_w + 1 + 1 + 3 + 3 + tail_w));
    const int name_w = flex * 58 / 100;
    const int genre_w = flex - name_w;
    const int lvis_n = static_cast<int>(m.lvisible.size());
    const int lcursor = std::clamp(m.lcursor, 0, std::max(0, lvis_n - 1));
    const int lfirst = follow_scroll(scroll_id(2), lcursor, list_rows, lvis_n);
    for (int r = 0; r < list_rows; ++r) {
        if (m.search_lists) {   // "p:" -- the STATIONS pane lists the saved station lists instead
            const int li = lfirst + r;
            if (li >= lvis_n) {
                left.push_back(box_line(s, seg_line({{s.header, lvis_n == 0 && r == 0 ? (m.lists.empty() ? "no station lists yet - SHIFT+k creates one" : "no station list matches") : ""}}, inner), s.border));
                continue;
            }
            const StationList& l = m.lists[static_cast<size_t>(m.lvisible[static_cast<size_t>(li)])];
            const std::string cnt = std::to_string(l.items.size()) + (l.items.size() == 1 ? " station" : " stations");
            const int cw = 13;
            const int nw = std::max(5, inner - 3 - (2 * display_width(s.c.list_separator) + 3) - cw);
            const std::string text = pad_right(std::to_string(li + 1), 3) + s.c.list_separator + " " + pad_right(truncate_str(shown_text(l.name), nw), nw)
                                   + " " + s.c.list_separator + " " + pad_right(cnt, cw);
            const bool hov = li == lcursor && m.search_focus;
            left.push_back(box_line(s, seg_line({{hov ? s.list_cursor : s.list, text}}, inner), s.border));
            continue;
        }
        const int vi = first + r;
        if (vi >= vis_n) { left.push_back(box_line(s, spaces(inner), s.border)); continue; }
        const int idx = m.visible[static_cast<size_t>(vi)];
        const Station& stn = stations[static_cast<size_t>(idx)];
        std::string tail = stn.codec_hint;
        if (stn.bitrate_hint > 0) tail = std::to_string(stn.bitrate_hint) + "k " + stn.codec_hint;
        const std::string sep = s.c.list_separator;
        const bool hovered = vi == cursor;   // also while searching: Up/Down move it there and ENTER tunes it
        const std::string name_shown = hovered ? marquee_or_truncate(m.marquee_row, m.marquee_since, m.t_sec, shown_text(stn.name), name_w, idx)
                                               : pad_right(truncate_str(shown_text(stn.name), name_w), name_w);
        // Number left-aligned and padded on the right, like the player ("5  |"), no leading blanks.
        int number = idx + 1;
        if (shown_list) {
            const auto it = std::find(shown_list->items.begin(), shown_list->items.end(), idx);
            number = static_cast<int>(it - shown_list->items.begin()) + 1;
        }
        const std::string text = pad_right(std::to_string(number), idx_w) + sep + " " + name_shown + " " + sep + " "
                               + pad_right(truncate_str(stn.genre, genre_w), genre_w) + " " + sep + " " + pad_right(truncate_str(tail, tail_w), tail_w);
        std::string style = (idx == st.tuned_index && st.state != StreamState::Idle) ? s.list_playing : s.list;
        if (hovered) style = s.list_cursor;
        left.push_back(box_line(s, seg_line({{style, text}}, inner), s.border));
    }
    if (m.search_focus || vis_n == 0 || m.search_lists) m.marquee_row = -1;   // nothing hovered: the next hover restarts the scroll
    const int hidden = m.search_lists ? std::max(0, lvis_n - (lfirst + list_rows)) : std::max(0, vis_n - (first + list_rows));
    left.push_back(box_bottom(s, left_w, hidden > 0 ? "( " + std::to_string(hidden) + " more )" : "", s.border_bottom));

    // PRESETS (16 keys), filled row by row; as many columns as the box height needs (4 rows -> 4 columns).
    std::vector<std::string> right;
    right.push_back(box_top(s, right_w, "PRESETS (" + utf8_take(shown_text(active_preset_name(m)), std::max(1, right_w - 16)) + ")", s.border));
    const int pinner = right_w - 4;
    const int ncols = (kPresetCount + list_rows - 1) / list_rows;
    bool tuned_scrolls = false;
    for (int r = 0; r < list_rows; ++r) {
        std::vector<Seg> cols;
        for (int c = 0; c < ncols; ++c) {
            const int colw = pinner / ncols + (c < pinner % ncols ? 1 : 0);
            const int slot = r * ncols + c; // 0-based
            if (slot >= kPresetCount) { cols.push_back({"", spaces(colw)}); continue; }
            const std::string key(1, kPresetKeys[slot]);
            const int idx = slot < static_cast<int>(m.presets.size()) ? m.presets[static_cast<size_t>(slot)] : -1;
            if (idx < 0 || idx >= static_cast<int>(stations.size())) { cols.push_back({s.preset_inactive, pad_right(key + " -", colw)}); continue; }
            const bool on = idx == st.tuned_index && st.state != StreamState::Idle;
            const std::string& body = on ? s.list_playing : s.list;
            cols.push_back({s.preset_key + ansi_bg(s.c.list_bg), key});     // key letter in PRESETS KEY, station title like the list
            // the preset name (SHIFT+c) if the station has one; the tuned entry scrolls when it does not fit
            const std::string lbl = shown_text(stations[static_cast<size_t>(idx)].preset_label());
            const int lw = std::max(1, colw - 3);
            std::string shown;
            if (on && display_width(lbl) > lw) { shown = marquee_or_truncate(m.pmarquee_row, m.pmarquee_since, m.t_sec, lbl, lw, slot); tuned_scrolls = true; }
            else shown = utf8_take(lbl, lw);
            cols.push_back({body, pad_right(" " + shown, colw - 1)});
        }
        right.push_back(box_line(s, seg_line(cols, pinner), s.border));
    }
    right.push_back(box_bottom(s, right_w, "", s.border_bottom));
    if (!tuned_scrolls) m.pmarquee_row = -1;   // nothing scrolls: the next tuned entry that is too long starts from its beginning

    std::vector<std::string> out;
    for (size_t i = 0; i < left.size(); ++i) out.push_back(left[i] + " " + right[i]);
    return out;
}

// ===========================================================================================
// PRESETS MENU -- replaces the whole screen (like the player's playlist menu).
//   SEARCH STATION / PRESET   full width, 1 content row   ("/s:" stations, "/p:" presets)
//   SELECT PRESET             full width, 4 columns x 2 rows of named presets
//   STATIONS | PRESETS        two equal panes (60 / 60 at 120 columns), 16 content rows each
//   hints, status line, blank filler down to the terminal height
// The pane that has the keyboard gets a small "\u25C0" behind its title, in
// the header colour. box_edge() wraps the whole title in one border colour,
// so the triangle switches colour and then switches straight back to that
// same border colour (the caller's `s.border`), the same way the player
// recolours its own focus indicators (see focus_indicator() in app.cpp).
// ===========================================================================================
std::string pane_title(const Style& s, const std::string& t, bool focused) {
    return focused ? t + " " + s.focus + "\u25C0" + kReset + s.border : t;
}

// "SELECT PRESET": the user's named presets in a 4-column grid, scrolling by rows.
std::vector<std::string> build_preset_pane(const Style& s, const UiModel& m, int W) {
    const MenuModel& mn = m.menu;
    const int total = static_cast<int>(m.banks.size());
    const int pv = static_cast<int>(mn.pvisible.size());
    const int cursor = std::clamp(mn.pcursor, 0, std::max(0, pv - 1));
    const bool hover_on = mn.focus == MenuFocus::Presets
                       || (mn.focus == MenuFocus::Search && mn.target == SearchTarget::Presets);
    const int R = kPresetPaneRows, C = kPresetPaneCols;
    const int grid_rows = (pv + C - 1) / C;
    const int first_row = follow_scroll(scroll_id(3), cursor / C, R, grid_rows);
    const int inner = W - 4;

    std::vector<std::string> out;
    const std::string title = mn.psearch.empty() ? "SELECT PRESET"
                                                 : "SELECT PRESET (results for: " + utf8_take(mn.psearch, 20) + ")";
    out.push_back(box_top(s, W, pane_title(s, title, mn.focus == MenuFocus::Presets), s.border));
    for (int r = 0; r < R; ++r) {
        if (pv == 0) {
            out.push_back(box_line(s, seg_line({{s.header, r == 0 ? (total == 0 ? "no presets yet - SHIFT+n creates one" : "no preset matches") : ""}}, inner), s.border));
            continue;
        }
        std::vector<Seg> cells;
        for (int c = 0; c < C; ++c) {
            const int colw = inner / C + (c < inner % C ? 1 : 0);
            const int shown_w = c < C - 1 ? colw - 1 : colw;     // one plain gap between the cells
            const int vi = (first_row + r) * C + c;
            if (vi >= pv) { cells.push_back({"", spaces(colw)}); continue; }
            const int bi = mn.pvisible[static_cast<size_t>(vi)];
            const PresetBank& b = m.banks[static_cast<size_t>(bi)];
            std::string style = (bi == m.bank_active) ? s.list_playing : s.list;
            if (hover_on && vi == cursor) style = s.list_cursor;
            cells.push_back({style, pad_right(truncate_str(shown_text(b.name), shown_w), shown_w)});
            if (c < C - 1) cells.push_back({"", " "});
        }
        out.push_back(box_line(s, seg_line(cells, inner), s.border));
    }
    const int hidden = std::max(0, pv - (first_row + R) * C);
    out.push_back(box_bottom(s, W, hidden > 0 ? "( " + std::to_string(hidden) + " more )" : "", s.border_bottom));
    return out;
}

// Replaces the visible columns [x, x + pw) of an ANSI-coloured line with `panel` (exactly pw columns wide).
// What the line had set up to the panel's left edge (its colour, if no reset came since) and the escapes inside the covered part
// are put back behind the panel, so the colours after it stay right (without this the border to the right of a panel turns white).
std::string splice_line(const std::string& base, int x, const std::string& panel, int pw) {
    std::string out, held, straddle_right, active;
    bool done = false;
    int col = 0;
    const int xe = x + pw;
    auto emit_panel = [&]() {
        if (done) return;
        done = true;
        out += std::string(kReset) + panel + kReset + active + held + straddle_right;
    };
    size_t i = 0;
    while (i < base.size()) {
        const unsigned char c0 = static_cast<unsigned char>(base[i]);
        if (c0 == 0x1b) {
            size_t j = i + 1;
            if (j < base.size() && base[j] == '[') {
                ++j;
                while (j < base.size() && !(base[j] >= '@' && base[j] <= '~')) ++j;
                if (j < base.size()) ++j;
            }
            const std::string esc = base.substr(i, j - i);
            if (col < x) {
                out += esc;
                if (esc == "\x1b[0m" || esc == "\x1b[m") active.clear(); else active += esc;
            }
            else if (col < xe) held += esc;
            else { emit_panel(); out += esc; }
            i = j;
            continue;
        }
        const size_t n = std::min(base.size() - i, c0 < 0x80 ? size_t(1) : (c0 >> 5) == 6 ? size_t(2) : (c0 >> 4) == 14 ? size_t(3) : size_t(4));
        const std::string ch = base.substr(i, n);
        const int w = std::max(1, display_width(ch));
        if (col + w <= x) out += ch;
        else if (col >= xe) { emit_panel(); out += ch; }
        else {                                     // overlaps the panel: a wide glyph on an edge becomes blanks
            if (col < x) out += spaces(x - col);
            if (col + w > xe) straddle_right += spaces(col + w - xe);
        }
        col += w;
        i += n;
    }
    emit_panel();
    return out;
}

// The small name box (same construction as the music player's Oscilloscope overlay: titled box, plain
// rows, key legend in the bottom edge).
constexpr int kNameOverlayW = 44;
std::vector<std::string> build_name_overlay(const Style& s, const NameOverlay& n) {
    const int W = kNameOverlayW, inner = W - 4;
    auto row = [&](const std::vector<Seg>& segs) { return box_line(s, seg_line(segs, inner), s.border); };
    std::vector<std::string> lines;
    lines.push_back(box_top(s, W, n.rename ? "Rename preset" : "New preset", s.border));
    lines.push_back(row({}));
    lines.push_back(box_line(s, field_row("", "Name: ", n.text, n.edit, true, inner, "", "", false), s.border));
    lines.push_back(row({{n.error.empty() ? s.header : s.legend,
                          n.error.empty() ? "max " + std::to_string(kPresetNameMax) + " characters" : n.error}}));
    lines.push_back(box_bottom(s, W, "[ENTER] OK  [ESC] cancel", s.border_bottom));
    return lines;
}

// The key legends of the menus are small tree tables (see layout_legend_tree()): one row per group of keys, narrower terminals
// get the short labels. The rows come out exactly W columns wide, like every other row of a screen.
static void push_legend(std::vector<std::string>& out, const Style& s, const std::vector<LegendGroup>& groups, int W, int max_rows = 0) {
    const auto rows = layout_legend_tree(groups, W - 1, max_rows);
    for (size_t i = 0; i < rows.size(); ++i) {
        // the tree lines and the bar are border-coloured (the smooth border gradient recolours them row by row); the label keeps the header
        // colour, the keys use LEGEND (KEY-CMD), the descriptions COMMAND
        out.push_back(" " + paint_legend_row(rows[i], s.border, "\x1b[1m" + s.header, s.legend, W - 1, s.legend_cmd));
    }
}

std::vector<std::string> build_menu(const Style& s, const UiModel& m, const RadioStatus& st, int W, int rows) {
    const MenuModel& mn = m.menu;
    const std::vector<Station> none;
    const auto& stations = m.stations ? *m.stations : none;
    const int total = static_cast<int>(stations.size());
    const int vis_n = static_cast<int>(mn.visible.size());
    const int cursor = std::clamp(mn.cursor, 0, std::max(0, vis_n - 1));
    const bool search_presets = mn.target == SearchTarget::Presets;
    // The station cursor row is lit while the STATIONS pane (or a station search) owns the keyboard.
    const bool stations_hover = mn.focus == MenuFocus::Stations
                             || (mn.focus == MenuFocus::Search && !search_presets);
    std::vector<std::string> out;

    // --- SEARCH STATION / SEARCH PRESET ---------------------------------------------------
    {
        const int inner = W - 4;
        const int shown_n = search_presets ? static_cast<int>(mn.pvisible.size()) : vis_n;
        const int all_n = search_presets ? static_cast<int>(m.banks.size()) : total;
        const std::string counter = std::to_string(shown_n) + " / " + std::to_string(all_n);
        const std::string list_ansi = s.search_idle;
        const std::string row = field_row(list_ansi, search_presets ? "/p: " : "/s: ",
                                          search_presets ? mn.psearch : mn.search,
                                          search_presets ? mn.psearch_edit : mn.search_edit,
                                          mn.focus == MenuFocus::Search, inner, counter, s.header, true, s.search);
        out.push_back(box_top(s, W, pane_title(s, search_presets ? "SEARCH PRESET" : "SEARCH STATION", mn.focus == MenuFocus::Search), s.border));
        out.push_back(box_line(s, row, s.border));
        out.push_back(box_bottom(s, W, "", s.border_bottom));
    }

    // --- SELECT PRESET (4 columns) ----------------------------------------------------------
    for (auto& l : build_preset_pane(s, m, W)) out.push_back(l);

    // Both panes: equal width, the odd column (if any) goes to PRESETS. 120 columns -> 60 / 60.
    const int left_w = W / 2;
    const int right_w = W - left_w;
    const int L = kMenuListRows;
    const int first = follow_scroll(scroll_id(4), cursor, L, vis_n);

    // column layout shared by both panes: "K| name | genre | 128k AAC"
    const std::string sep = s.c.list_separator;
    const int sepw = display_width(sep);
    const int tail_w = 8;

    // --- STATIONS (full list, or the search results) ------------------------------------------
    std::vector<std::string> left;
    left.push_back(box_top(s, left_w, pane_title(s, mn.search.empty() ? "STATIONS" : "STATIONS (results for: " + utf8_take(mn.search, 20) + ")",
                                                 mn.focus == MenuFocus::Stations), s.border));
    {
        const int inner = left_w - 4;
        const int idx_w = static_cast<int>(std::to_string(std::max(1, total)).size());
        // idx|name|genre|tail + 1 blank + 1 column for the preset key of the station
        const int flex = std::max(8, inner - (idx_w + 3 * sepw + 5 + tail_w + 2));
        const int name_w = flex * 58 / 100;
        const int genre_w = flex - name_w;
        for (int r = 0; r < L; ++r) {
            const int vi = first + r;
            if (vi >= vis_n) { left.push_back(box_line(s, spaces(inner), s.border)); continue; }
            const int idx = mn.visible[static_cast<size_t>(vi)];
            const Station& stn = stations[static_cast<size_t>(idx)];
            std::string tail = stn.codec_hint;
            if (stn.bitrate_hint > 0) tail = std::to_string(stn.bitrate_hint) + "k " + stn.codec_hint;
            const bool hovered = vi == cursor && stations_hover;
            const std::string name_shown = hovered ? marquee_or_truncate(mn.marquee_row, mn.marquee_since, m.t_sec, shown_text(stn.name), name_w, idx)
                                                   : pad_right(truncate_str(shown_text(stn.name), name_w), name_w);
            const int slot = preset_slot_of(m.presets, idx);
            const std::string key = slot >= 0 ? std::string(1, kPresetKeys[slot]) : " ";
            const std::string text = pad_right(std::to_string(idx + 1), idx_w) + sep + " " + name_shown + " " + sep + " "
                                   + pad_right(truncate_str(stn.genre, genre_w), genre_w) + " " + sep + " "
                                   + pad_right(truncate_str(tail, tail_w), tail_w) + " ";
            std::string style = (idx == st.tuned_index && st.state != StreamState::Idle) ? s.list_playing : s.list;
            if (hovered) style = s.list_cursor;
            left.push_back(box_line(s, seg_line({{style, text}, {hovered || key == " " ? style : s.preset_key + ansi_bg(s.c.list_bg), key}}, inner), s.border));
        }
        if (vis_n == 0 || !stations_hover) mn.marquee_row = -1;   // nothing hovered: the next hover restarts the scroll
    }
    const int hidden = std::max(0, vis_n - (first + L));
    left.push_back(box_bottom(s, left_w, hidden > 0 ? "( " + std::to_string(hidden) + " more )" : "", s.border_bottom));

    // --- PRESETS: one line per key (the ACTIVE preset) -----------------------------------------
    std::vector<std::string> right;
    right.push_back(box_top(s, right_w, "PRESETS (" + utf8_take(shown_text(active_preset_name(m)), std::max(1, right_w - 16)) + ")", s.border));
    {
        const int inner = right_w - 4;
        const int flex = std::max(8, inner - (1 + 3 * sepw + 5 + tail_w));
        const int name_w = flex * 58 / 100;
        const int genre_w = flex - name_w;
        for (int r = 0; r < L; ++r) {
            if (r >= kPresetCount) { right.push_back(box_line(s, spaces(inner), s.border)); continue; }
            const std::string key(1, kPresetKeys[r]);
            const int idx = r < static_cast<int>(m.presets.size()) ? m.presets[static_cast<size_t>(r)] : -1;
            if (idx < 0 || idx >= total) {
                right.push_back(box_line(s, seg_line({{s.preset_inactive, key + sep + " -"}}, inner), s.border));
                continue;
            }
            const Station& stn = stations[static_cast<size_t>(idx)];
            std::string tail = stn.codec_hint;
            if (stn.bitrate_hint > 0) tail = std::to_string(stn.bitrate_hint) + "k " + stn.codec_hint;
            const std::string text = sep + " " + pad_right(truncate_str(shown_text(stn.name), name_w), name_w) + " " + sep + " "
                                   + pad_right(truncate_str(stn.genre, genre_w), genre_w) + " " + sep + " "
                                   + pad_right(truncate_str(tail, tail_w), tail_w);
            const bool on = idx == st.tuned_index && st.state != StreamState::Idle;
            right.push_back(box_line(s, seg_line({{s.preset_key + ansi_bg(s.c.list_bg), key}, {on ? s.list_playing : s.list, text}}, inner), s.border));
        }
    }
    right.push_back(box_bottom(s, right_w, "", s.border_bottom));
    for (size_t i = 0; i < left.size(); ++i) out.push_back(left[i] + right[i]);

    // --- hints (same style as the playlist menu), status line, filler ---------------------------
    push_legend(out, s, {
        {"Move", {"[TAB] Pane", "[\u2191\u2193\u2190\u2192] Navi.", "[ENTER] Tune / Open Preset\001Tune/Open", "[/] Search", "[p:] Search presets\001Presets", "[ESC] Clear / Exit\001Exit"}},
        {"Stations", {std::string("[") + kPresetKeys + "] Set preset (same key again clears it)\001Set preset", "[DEL] Clear preset\001Clear"}},
        {"Create/Edit", {"[SHIFT+n] New Preset\001New", "[SHIFT+c] Rename", "[SHIFT+d] Delete"}},
    }, W, 3);
    out.push_back(seg_line({{s.header, mn.flash}}, W));
    while (static_cast<int>(out.size()) < rows) out.push_back(spaces(W));
    out.resize(static_cast<size_t>(rows));

    // --- name overlay (Shift+N / Shift+C), drawn over the finished menu ---------------------------
    if (mn.name.open) {
        const auto panel = build_name_overlay(s, mn.name);
        const int ph = static_cast<int>(panel.size());
        const int x = std::max(0, (W - kNameOverlayW) / 2);
        const int y = std::clamp((rows - ph) / 2 - 2, 0, std::max(0, rows - ph));
        for (int i = 0; i < ph; ++i)
            out[static_cast<size_t>(y + i)] = splice_line(out[static_cast<size_t>(y + i)], x, panel[static_cast<size_t>(i)], kNameOverlayW);
    }
    return out;
}

// ===========================================================================================
// STATION LISTS menu (Shift+K) -- laid out like the player's playlist editor.
//   tab 1  CREATE / EDIT:        STATION LISTS (tab strip + name) / SEARCH ALL STATIONS /
//                                STATIONS (library) | LIST CONTENTS (the list being built)
//   tab 2  SAVED STATION LISTS:  STATION LISTS (tab strip) / SEARCH STATION LISTS / STATION LISTS
// ===========================================================================================

// The settings screen's tab strip (2 lines) for a pane that carries its own title on the border line:
//   ┌─ TITLE ───────┐  CREATE / EDIT  ┌──┐  [OTHER]  ┌────────...─┐
//   │               └─────────────────┘  └──────────┘          ... │
// The current tab is "[NAME]" in tab_current, the others in tab_other. `top_ansi` / `bot_ansi` colour the two lines.
std::vector<std::string> menu_tab_strip(const Style& s, int W, const std::string& title, const std::vector<std::string>& labels,
                                        int cur, const std::string& top_ansi, const std::string& bot_ansi,
                                        int min_inner = 0, const std::vector<Seg>& line2 = {}, int line2_cols = 0) {
    const std::string tab_cur = ansi_fg(s.c.tab_current), tab_oth = ansi_fg(s.c.tab_other);
    const std::string V = "│", Hz = "─";
    std::vector<Seg> top, bot;
    const int tw0 = display_width(title);
    const int n = static_cast<int>(labels.size());
    // The tab names sit at the right end: the title part grows so that exactly three "─" are left before the last corner.
    // (Exactly one tab carries the two "[ ]" characters, so the sum does not depend on which tab is current.)
    int tabs_w = 2;
    for (const auto& l : labels) tabs_w += display_width(l) + 4;
    tabs_w += 4 * std::max(0, n - 1);
    const int natural = 1 + 1 + tw0 + 1 + std::max(5, 20 - (tw0 + 3));
    const int first_inner = std::max({natural, min_inner, W - 2 - tabs_w - 5});
    const int dashes = first_inner - (1 + 1 + tw0 + 1);
    top.push_back({top_ansi, s.c.box_upper_left + "─ " + title + " " + repeat(Hz, dashes) + "╮"});
    bot.push_back({bot_ansi, V});
    if (line2.empty()) bot.push_back({"", spaces(first_inner)});
    else { for (const auto& g : line2) bot.push_back(g); bot.push_back({"", spaces(std::max(0, first_inner - line2_cols))}); }
    bot.push_back({bot_ansi, "╰"});
    int used = first_inner + 2;
    for (int i = 0; i < n; ++i) {
        const bool is_cur = i == cur;
        const std::string label = is_cur ? "[" + labels[static_cast<size_t>(i)] + "]" : labels[static_cast<size_t>(i)];
        const int tw = display_width(label) + 4;
        top.push_back({"", "  "}); top.push_back({is_cur ? tab_cur : tab_oth, label}); top.push_back({"", "  "});
        bot.push_back({bot_ansi, repeat(Hz, tw)});
        used += tw;
        if (i < n - 1) {
            top.push_back({top_ansi, "╭──╮"});
            bot.push_back({bot_ansi, "╯  ╰"});
            used += 4;
        } else {
            const int rest = std::max(0, W - used - 2);
            top.push_back({top_ansi, "╭" + repeat(Hz, rest) + s.c.box_upper_right});
            bot.push_back({bot_ansi, "╯" + spaces(rest) + V});
        }
    }
    return {seg_line(top, W), seg_line(bot, W)};
}

// One station pane (STATIONS or LIST CONTENTS): "N | name | genre | 128k AAC  m". `rows` are station indices;
// `numbers` = show the station's own number (library) or its position in the list (contents). `marks`
// (nullable) lights a dot at the row end for stations that are already in the list being built.
std::vector<std::string> lists_station_pane(const Style& s, const UiModel& m, const RadioStatus& st, int w, int L,
                                            const std::string& title, const std::vector<int>& rows, int cursor,
                                            bool hover, bool numbers, const std::vector<int>* marks,
                                            int& mq_row, double& mq_since, const std::string& empty_msg) {
    const std::vector<Station> none;
    const auto& stations = m.stations ? *m.stations : none;
    const int n = static_cast<int>(rows.size());
    cursor = std::clamp(cursor, 0, std::max(0, n - 1));
    const int first = follow_scroll(&mq_row, cursor, L, n);
    const int inner = w - 4;
    const std::string sep = s.c.list_separator;
    const int sepw = display_width(sep);
    const int tail_w = 8;
    const int idx_w = static_cast<int>(std::to_string(std::max<size_t>(1, numbers ? stations.size() : rows.size())).size());
    const int flex = std::max(8, inner - (idx_w + 3 * sepw + 5 + tail_w + 2));
    const int name_w = flex * 58 / 100;
    const int genre_w = flex - name_w;

    std::vector<std::string> out;
    out.push_back(box_top(s, w, title, s.border));
    bool any_hover = false;
    for (int r = 0; r < L; ++r) {
        const int vi = first + r;
        if (vi >= n) {
            out.push_back(box_line(s, seg_line({{s.header, n == 0 && r == 0 ? empty_msg : ""}}, inner), s.border));
            continue;
        }
        const int idx = rows[static_cast<size_t>(vi)];
        const Station& stn = stations[static_cast<size_t>(idx)];
        std::string tail = stn.codec_hint;
        if (stn.bitrate_hint > 0) tail = std::to_string(stn.bitrate_hint) + "k " + stn.codec_hint;
        const bool hovered = hover && vi == cursor;
        any_hover = any_hover || hovered;
        const std::string name_shown = hovered ? marquee_or_truncate(mq_row, mq_since, m.t_sec, shown_text(stn.name), name_w, numbers ? idx : vi)
                                               : pad_right(truncate_str(shown_text(stn.name), name_w), name_w);
        const bool in_list = marks && std::find(marks->begin(), marks->end(), idx) != marks->end();
        const std::string text = pad_right(std::to_string(numbers ? idx + 1 : vi + 1), idx_w) + sep + " " + name_shown + " " + sep + " "
                               + pad_right(truncate_str(stn.genre, genre_w), genre_w) + " " + sep + " "
                               + pad_right(truncate_str(tail, tail_w), tail_w) + " " + (in_list ? "•" : " ");
        std::string style = (idx == st.tuned_index && st.state != StreamState::Idle) ? s.list_playing : s.list;
        if (hovered) style = s.list_cursor;
        out.push_back(box_line(s, seg_line({{style, text}}, inner), s.border));
    }
    if (!any_hover) mq_row = -1;   // nothing hovered: the next hover restarts the scroll
    const int hidden = std::max(0, n - (first + L));
    out.push_back(box_bottom(s, w, hidden > 0 ? "( " + std::to_string(hidden) + " more )" : "", s.border_bottom));
    return out;
}

std::vector<std::string> build_lists_menu(const Style& s, const UiModel& m, const RadioStatus& st, int W, int rows) {
    const ListMenuModel& lm = m.lmenu;
    const std::vector<Station> none;
    const auto& stations = m.stations ? *m.stations : none;
    const int inner = W - 4;
    const std::string list_ansi = s.c.list_fg.empty() ? "" : ansi_fg(s.c.list_fg);
    std::vector<std::string> out;

    // --- STATION LISTS: the tab strip; on CREATE / EDIT the name field sits on its second line (like the player's PLAYLISTS pane) ---
    {
        const std::string red = "\x1b[41;37m";
        std::vector<Seg> segs;
        int cols = 0;
        const int field_w = kListNameMax + 1;                    // 25 characters + the caret block
        const std::string prefix = " Name: ";
        if (lm.tab == 0) {
            segs.push_back({list_ansi, prefix});
            cols += display_width(prefix);
            if (lm.focus == ListFocus::Name) {
                const EditPaint p = paint_edit_field(lm.name, lm.name_edit, field_w, red, true);
                segs.push_back({red, p.s}); cols += p.cols;
            } else {
                const std::string t = lm.name.empty() ? std::string("(untitled)") : utf8_take(lm.name, field_w);
                segs.push_back({lm.name.empty() ? s.legend : list_ansi, t}); cols += display_width(t);
            }
            if (lm.dirty) { segs.push_back({s.legend, " *"}); cols += 2; }
        }
        const int min_inner = static_cast<int>(prefix.size()) + field_w + 3;
        const auto strip = menu_tab_strip(s, W, "STATION LISTS", {"CREATE / EDIT", "SAVED STATION LISTS"}, lm.tab, s.border, s.border,
                                          min_inner, segs, cols);
        out.push_back(strip[0]);
        out.push_back(strip[1]);
    }
    out.push_back(box_bottom(s, W, "", s.border_bottom));
    const int top_rows = static_cast<int>(out.size());

    const int hint_rows = 3;                                  // two legend rows (or the prompt) + the status line
    if (lm.tab == 0) {
        // --- SEARCH ALL STATIONS ------------------------------------------------------------
        const int vis_n = static_cast<int>(lm.visible.size());
        const bool sf = lm.focus == ListFocus::Search;
        const std::string counter = std::to_string(vis_n) + " / " + std::to_string(stations.size());
        out.push_back(box_top(s, W, pane_title(s, "SEARCH ALL STATIONS", sf), s.border));
        out.push_back(box_line(s, field_row(s.search_idle, "Search: ", lm.search, lm.search_edit, sf, inner, counter, s.header, true, s.search), s.border));
        out.push_back(box_bottom(s, W, "", s.border_bottom));

        // --- STATIONS (all, or the search results)  |  LIST CONTENTS --------------------------
        const int L = std::max(4, rows - top_rows - 3 - 2 - hint_rows);
        const int left_w = W / 2, right_w = W - left_w;
        const std::string sort = lm.sort_az ? "sort: A-Z" : "sort: list order";
        const std::string ltitle = lm.search.empty() ? "STATIONS (" + sort + ")"
                                                     : "STATIONS (results for: " + utf8_take(lm.search, 16) + ", " + sort + ")";
        const bool stations_hover = lm.focus == ListFocus::Stations || lm.focus == ListFocus::Search;
        const auto left = lists_station_pane(s, m, st, left_w, L, pane_title(s, ltitle, lm.focus == ListFocus::Stations),
                                             lm.visible, lm.cursor, stations_hover, true, &lm.items,
                                             lm.marquee_row, lm.marquee_since, "no station matches");
        const int n_items = static_cast<int>(lm.items.size());
        std::string rtitle = "LIST CONTENTS (" + std::to_string(n_items) + ")";
        if (lm.focus == ListFocus::Contents && n_items > 0)
            rtitle += "  " + std::to_string(std::clamp(lm.item_cursor, 0, n_items - 1) + 1) + "/" + std::to_string(n_items);
        const auto right = lists_station_pane(s, m, st, right_w, L, pane_title(s, rtitle, lm.focus == ListFocus::Contents),
                                              lm.items, lm.item_cursor, lm.focus == ListFocus::Contents, false, nullptr,
                                              lm.marquee2_row, lm.marquee2_since, "empty - ENTER on a station adds it");
        for (size_t i = 0; i < left.size(); ++i) out.push_back(left[i] + right[i]);
    } else {
        // --- SEARCH STATION LISTS -------------------------------------------------------------
        const int total = static_cast<int>(m.lists.size());
        const int vn = static_cast<int>(lm.mvisible.size());
        const bool sf = lm.mfocus == ListManageFocus::Search;
        const std::string counter = std::to_string(vn) + " / " + std::to_string(total);
        out.push_back(box_top(s, W, pane_title(s, "SEARCH LISTS OF STATIONS", sf), s.border));
        out.push_back(box_line(s, field_row(s.search_idle, "Search: ", lm.msearch, lm.msearch_edit, sf, inner, counter, s.header, true, s.search), s.border));
        out.push_back(box_bottom(s, W, "", s.border_bottom));

        // --- STATION LISTS (the search results) -----------------------------------------------
        const int L = std::max(4, rows - top_rows - 3 - 2 - hint_rows);
        const int cursor = std::clamp(lm.mcursor, 0, std::max(0, vn - 1));
        const int first = follow_scroll(scroll_id(5), cursor, L, vn);
        const std::string sep = s.c.list_separator;
        const int idx_w = 3, count_w = 13;
        const int name_w = std::max(5, inner - idx_w - (2 * display_width(sep) + 3) - count_w);
        out.push_back(box_top(s, W, pane_title(s, "STATION LISTS (" + std::to_string(vn) + ")", lm.mfocus == ListManageFocus::List), s.border));
        bool any_hover = false;
        for (int r = 0; r < L; ++r) {
            const int vi = first + r;
            if (vi >= vn) {
                std::string msg;
                if (vn == 0 && r == L / 2) msg = lm.msearch.empty() ? "NO SAVED STATION LISTS YET" : "NO MATCHING STATION LISTS";
                const int pad = msg.empty() ? 0 : std::max(0, (inner - display_width(msg)) / 2);
                out.push_back(box_line(s, seg_line({{"", spaces(pad)}, {s.list, msg}}, inner), s.border));
                continue;
            }
            const StationList& l = m.lists[static_cast<size_t>(lm.mvisible[static_cast<size_t>(vi)])];
            const bool hovered = vi == cursor;
            any_hover = any_hover || hovered;
            const std::string name_shown = hovered ? marquee_or_truncate(lm.marquee_row, lm.marquee_since, m.t_sec, shown_text(l.name), name_w, vi)
                                                   : pad_right(truncate_str(shown_text(l.name), name_w), name_w);
            const std::string count = std::to_string(l.items.size()) + (l.items.size() == 1 ? " station" : " stations");
            const std::string text = pad_right(std::to_string(vi + 1), idx_w) + sep + " " + name_shown + " " + sep + " " + pad_right(count, count_w);
            std::string style = s.list;
            if (!lm.name.empty() && clean_list_name(lm.name) == l.name) style = s.list_playing;   // the one open in tab 1
            if (hovered) style = s.list_cursor;
            out.push_back(box_line(s, seg_line({{style, text}}, inner), s.border));
        }
        if (!any_hover) lm.marquee_row = -1;
        const int hidden = std::max(0, vn - (first + L));
        out.push_back(box_bottom(s, W, hidden > 0 ? "( " + std::to_string(hidden) + " more )" : "", s.border_bottom));
    }

    // --- hints / prompt, status line, filler --------------------------------------------------------
    if (lm.confirm_exit) {
        const std::string nm = lm.name.empty() ? std::string("(untitled)") : lm.name;
        out.push_back(seg_line({{"\x1b[43;30m", " Save changes to \"" + nm + "\" before exiting?   [Y]es   [N]o   [ESC] cancel "}}, W));
        out.push_back(spaces(W));
    } else if (lm.confirm_delete) {
        std::string nm;
        if (lm.mcursor >= 0 && lm.mcursor < static_cast<int>(lm.mvisible.size()))
            nm = m.lists[static_cast<size_t>(lm.mvisible[static_cast<size_t>(lm.mcursor)])].name;
        out.push_back(seg_line({{"\x1b[41;97m", " Delete station list \"" + nm + "\"? This can't be undone.   [Y]es   [N]o   [ESC] cancel "}}, W));
        out.push_back(spaces(W));
    } else {
        // Left/Right and ESC say what they do right now: in a text box that was typed into they move the caret /
        // leave the box, everywhere else they switch the tab / clear the search or close the menu.
        const bool box = lm.tab == 0 ? (lm.focus == ListFocus::Name || lm.focus == ListFocus::Search)
                                     : lm.mfocus == ListManageFocus::Search;
        const bool in_text = box && lm.typing;
        push_legend(out, s, {
            {"Move", {in_text ? "[\u2190\u2192] Cursor" : "[\u2190\u2192] Switch Tab\001Tab", "[TAB] Focus", "[\u2191\u2193] Navi.", "[ENTER] Add/Load\001Load", "[/] Search",
                      in_text ? "[ESC] Leave field" : "[ESC] Clear / Exit\001Exit"}},
            {"Stations", {"[DEL] Remove", "[4/5] Move \u2191\u2193", "[SHIFT+t] Sort", "[CTRL+s] Save"}},
        }, W, 2);
    }
    out.push_back(seg_line({{s.header, lm.flash}}, W));
    while (static_cast<int>(out.size()) < rows) out.push_back(spaces(W));
    out.resize(static_cast<size_t>(rows));
    return out;
}

// ===========================================================================================
// RADIO BROWSER menu (Shift+S): six input panes (3 rows x 2), RESULTS | STATION INFO underneath.
// ===========================================================================================
const char* const kBrowseTitles[kBrowseFields] = {
    "NAME", "TAGS (comma separated, all must match)", "COUNTRY (name or code, e.g. DE)",
    "STATE / REGION", "LANGUAGE (e.g. german)", "BITRATE (kbps: e.g. 128 or range via 64-192)"};

std::vector<std::string> build_browse(const Style& s, const UiModel& m, const RadioStatus& st, int W, int rows) {
    const BrowseModel& bm = m.browse;
    std::vector<std::string> out;
    const int left_w = W / 2;
    const int right_w = W - left_w;                       // equal panes, the odd column (if any) goes right
    const std::string list_ansi = s.c.list_fg.empty() ? "" : ansi_fg(s.c.list_fg);

    // --- the six input panes ---------------------------------------------------------------------
    auto field_box = [&](int i, int w) {
        const bool focused = static_cast<int>(bm.focus) == i;
        std::vector<std::string> b;
        b.push_back(box_top(s, w, pane_title(s, kBrowseTitles[i], focused), s.border));
        b.push_back(box_line(s, field_row(list_ansi, "", bm.text[i], bm.edit[i], focused, w - 4, "", s.header, false), s.border));
        b.push_back(box_bottom(s, w, "", s.border_bottom));
        return b;
    };
    for (int r = 0; r < 3; ++r) {
        const auto L = field_box(2 * r, left_w);
        const auto R = field_box(2 * r + 1, right_w);
        for (size_t i = 0; i < L.size(); ++i) out.push_back(L[i] + R[i]);
    }

    // --- RESULTS | STATION INFO --------------------------------------------------------------------
    const int fixed_rows = 9 + 3;                          // six panes + hint line + status line + "tuned in" line
    const int list_rows = std::max(6, rows - fixed_rows - 2);
    static const std::vector<BrowseStation> kNoResults;
    const std::vector<BrowseStation>& res = bm.results ? *bm.results : kNoResults;
    const int n = static_cast<int>(res.size());
    const int cursor = std::clamp(bm.cursor, 0, std::max(0, n - 1));
    const int first = follow_scroll(scroll_id(6), cursor, list_rows, n);
    const bool results_focus = bm.focus == BrowseFocus::Results;

    std::string title = "RESULTS";
    if (bm.state == BrowseState::Searching) title += " (searching ...)";
    else if (bm.state == BrowseState::Done) title += " (" + std::to_string(n) + (n >= kBrowseLimit ? "+" : "") + ")";
    else if (bm.state == BrowseState::Error) title += " (error)";

    // streams that are already in the station list (same stream = same_stream_url(), not just the same string)
    auto is_listed = [&](const BrowseStation& b) {
        if (!m.stations) return false;
        for (const auto& st : *m.stations) if (same_stream_url(st.url, b.stream_url()) || same_stream_url(st.url, b.url)) return true;
        return false;
    };

    std::vector<std::string> left;
    left.push_back(box_top(s, left_w, pane_title(s, title, results_focus), s.border));
    {
        const int inner = left_w - 4;
        const std::string sep = s.c.list_separator;
        const int sepw = display_width(sep);
        const int tail_w = 8;                              // "128k AAC"
        const int cc_w = 2;                                // country code
        const int flex = std::max(8, inner - (2 + 3 * (sepw + 2) + cc_w + tail_w));
        const int name_w = flex * 58 / 100;
        const int tags_w = flex - name_w;
        bool any_hover = false;
        for (int r = 0; r < list_rows; ++r) {
            const int vi = first + r;
            if (vi >= n) {
                std::string msg;
                if (n == 0 && r == 0) {
                    if (bm.state == BrowseState::Searching) msg = "Searching radio-browser.info ...";
                    else if (bm.state == BrowseState::Error) msg = bm.message;
                    else if (bm.state == BrowseState::Done) msg = "No stations found. Try fewer or broader terms.";
                    else msg = "Fill in one or more panes above and press ENTER.";
                }
                left.push_back(box_line(s, seg_line({{bm.state == BrowseState::Error ? s.legend : s.header, msg}}, inner), s.border));
                continue;
            }
            const BrowseStation& b = res[static_cast<size_t>(vi)];
            const bool hovered = vi == cursor && results_focus;
            any_hover = any_hover || hovered;
            const std::string name_shown = hovered ? marquee_or_truncate(bm.marquee_row, bm.marquee_since, m.t_sec, shown_text(b.name), name_w, vi)
                                                   : pad_right(truncate_str(shown_text(b.name), name_w), name_w);
            std::string tail = b.codec;
            if (b.bitrate > 0) tail = std::to_string(b.bitrate) + "k " + b.codec;
            const bool listed = is_listed(b);
            const std::string text = std::string(listed ? "\u2022" : " ") + " " + name_shown + " " + sep + " "
                                   + pad_right(utf8_take(b.countrycode, cc_w), cc_w) + " " + sep + " "
                                   + pad_right(truncate_str(tail, tail_w), tail_w) + " " + sep + " "
                                   + pad_right(truncate_str(b.tags, tags_w), tags_w);
            // Lit whatever way the station was tuned (this menu, the main STATIONS list, a preset ...): same_stream_url()
            // compares either of the result's addresses with the tuned one, ignoring http/https, "/" and tracking queries.
            const bool tuned = st.state != StreamState::Idle && !st.tuned_url.empty()
                            && (same_stream_url(b.stream_url(), st.tuned_url) || same_stream_url(b.url, st.tuned_url));
            // same look as the main screen's STATIONS pane: the tuned station is lit, the cursor row wins over it
            std::string style = tuned ? s.list_playing : s.list;
            if (!b.online && !tuned) style = s.header;     // stations that failed their last check are dimmed
            if (hovered) style = s.list_cursor;
            left.push_back(box_line(s, seg_line({{style, text}}, inner), s.border));
        }
        if (!any_hover) bm.marquee_row = -1;               // nothing hovered: the next hover restarts the scroll
    }
    const int hidden = std::max(0, n - (first + list_rows));
    left.push_back(box_bottom(s, left_w, hidden > 0 ? "( " + std::to_string(hidden) + " more )" : "", s.border_bottom));

    // station info of the hovered result (follows the cursor whichever pane has the keyboard)
    std::vector<std::string> right;
    right.push_back(box_top(s, right_w, "STATION INFO", s.border));
    {
        const int inner = right_w - 4;
        std::vector<std::string> info;
        auto kv = [&](const std::string& label, const std::string& value, int max_lines = 1) {
            const auto lines = wrap_lines(value.empty() ? "-" : value, std::max(8, inner - 11), max_lines);
            for (size_t i = 0; i < lines.size(); ++i)
                info.push_back(seg_line({{i == 0 ? s.key : "", pad_right(i == 0 ? label : "", 9)}, {"", i == 0 ? ": " : "  "}, {s.val, lines[i]}}, inner));
        };
        if (n > 0) {
            const BrowseStation& b = res[static_cast<size_t>(cursor)];
            std::string country = b.country;
            if (!b.countrycode.empty()) country += (country.empty() ? "" : " ") + std::string("(") + b.countrycode + ")";
            std::string tags = b.tags;
            for (size_t i = 0; i + 1 < tags.size(); ++i) if (tags[i] == ',' && tags[i + 1] != ' ') { tags.insert(i + 1, " "); }
            std::string rate = b.bitrate > 0 ? std::to_string(b.bitrate) + " kbps" : "";
            if (!b.codec.empty()) rate += (rate.empty() ? "" : " ") + b.codec;
            if (b.hls) rate += " (HLS)";
            kv("Name", shown_text(b.name), 2);
            kv("Country", country);
            kv("State", b.state);
            kv("Language", b.language);
            kv("Tags", tags, 3);
            kv("Bitrate", rate);
            kv("Votes", std::to_string(b.votes));
            kv("Clicks", std::to_string(b.clickcount) + " in the last 24 h");
            kv("Online", b.online ? "yes" + (b.lastchecktime.empty() ? std::string() : "  (checked " + b.lastchecktime + " UTC)")
                                  : "NO at the last check" + (b.lastchecktime.empty() ? std::string() : "  (" + b.lastchecktime + " UTC)"));
            kv("Homepage", b.homepage, 2);
            kv("Stream", b.stream_url(), 3);
            std::string listed;
            if (is_listed(b)) listed = "already in your station list";
            kv("In list", listed.empty() ? "no  ([a] adds it)" : listed);
        } else {
            info.push_back(seg_line({{s.header, "Hover a result to see its details here."}}, inner));
        }
        for (int r = 0; r < list_rows; ++r)
            right.push_back(box_line(s, r < static_cast<int>(info.size()) ? info[static_cast<size_t>(r)] : spaces(inner), s.border));
    }
    right.push_back(box_bottom(s, right_w, "", s.border_bottom));
    for (size_t i = 0; i < left.size(); ++i) out.push_back(left[i] + right[i]);

    // --- hints (same style as the PRESETS menu), status line, filler -------------------------------
    out.push_back(seg_line(legend_segs(s.legend, s.legend_cmd, "[TAB] Next pane | [ENTER] Search / Tune | [\u2191\u2193] Navi. | [/] Back to NAME | [ESC] Clear / Close | RESULTS: [a] Add to list"), W));
    std::string status = bm.flash;
    if (status.empty()) {
        if (bm.state == BrowseState::Searching) status = "searching ...";
        else if (bm.state == BrowseState::Error) status = bm.message;
        else if (bm.state == BrowseState::Done && n > 0) status = std::to_string(n) + " stations from " + bm.server + "  (most clicked first, offline ones hidden)";
    }
    out.push_back(seg_line({{s.header, status}}, W));
    // What is playing right now -- always shown, whether or not it is among the results.
    {
        std::string state_txt;
        switch (st.state) {
            case StreamState::Live:         state_txt = "LIVE " + fmt_hms(st.listening_sec); break;
            case StreamState::Buffering:    state_txt = "buffering"; break;
            case StreamState::Connecting:   state_txt = "connecting"; break;
            case StreamState::Reconnecting: state_txt = "reconnecting"; break;
            case StreamState::Failed:       state_txt = "offline"; break;
            default: break;
        }
        std::vector<Seg> row = {{s.key, "Tuned in: "}};
        if (st.state == StreamState::Idle || st.tuned_name.empty()) {
            row.push_back({s.header, "nothing"});
        } else {
            row.push_back({s.val, shown_text(st.tuned_name)});
            row.push_back({"", "  (" + state_txt + ")"});
            const std::string np = st.info.artist.empty() ? st.info.title : st.info.artist + " - " + st.info.title;
            if (!np.empty()) row.push_back({s.header, "  \u266A " + shown_text(np)});
        }
        out.push_back(seg_line(row, W));
    }
    while (static_cast<int>(out.size()) < rows) out.push_back(spaces(W));
    out.resize(static_cast<size_t>(rows));
    return out;
}

// ===========================================================================================
// CHEATSHEET ('?') -- full-screen, same construction as the music player's cheat sheet
// (App::build_cheatsheet_screen): one table of  KEY | DESCRIPTION  rows grouped in categories. A
// category's title is drawn in bold Header colour above a blank spacer row and sits on the category's
// first entry. Scrollable: scroll is counted in display lines (a category costs three: spacer +
// title + the row itself), so a title scrolls like any other line.
// Key labels are the DEFAULT keys (lowercase = the plain key, SHIFT+x = the capital); the REFERENCE settings tab rebinds most of them.
// ===========================================================================================
// How a key is shown in the cheat sheet and on the REFERENCE tab: a capital letter is a Shift press, so "T" reads
// "SHIFT+t" (the binding itself stays "T"); labels such as "SHIFT+t" / "CTRL+SHIFT+u" get the lower-case letter too.
std::string pretty_key(const std::string& k) {
    if (k == "@SWITCHKEY") return muisc::mode_key_label(g_mode_key_next);   // the key set in the player's REFERENCE tab, named for this keyboard
    if (k == "@SWITCHBACK") return muisc::mode_key_label(g_mode_key_prev);
    if (k == "(" || k == ")") return muisc::symbol_key_label(k);   // SHIFT+8 / SHIFT+9 on a German keyboard
    if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z') return std::string("SHIFT+") + static_cast<char>(k[0] + 32);
    std::string o = k;
    size_t p = 0;
    while ((p = o.find("SHIFT+", p)) != std::string::npos) {
        p += 6;
        if (p < o.size() && o[p] >= 'A' && o[p] <= 'Z' && (p + 1 >= o.size() || !std::isalpha(static_cast<unsigned char>(o[p + 1]))))
            o[p] = static_cast<char>(o[p] + 32);
    }
    return o;
}

// Word-wraps `text` into lines of at most `width` columns (a longer single word is cut).
std::vector<std::string> wrap_words(const std::string& text, int width) {
    std::vector<std::string> out;
    std::string cur;
    size_t i = 0;
    while (i <= text.size()) {
        const size_t j = text.find(' ', i);
        std::string word = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
        while (width > 0 && display_width(word) > width) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            const std::string head = utf8_take(word, width);
            if (head.empty()) break;
            out.push_back(head);
            word = word.substr(head.size());
        }
        if (cur.empty()) cur = word;
        else if (display_width(cur) + 1 + display_width(word) <= width) cur += " " + word;
        else { out.push_back(cur); cur = word; }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (!cur.empty() || out.empty()) out.push_back(cur);
    return out;
}

struct CheatRow { const char* header; const char* key; const char* desc; };
const CheatRow kCheatRows[] = {
    // --- System ---
    {"SYSTEM (MAIN UI)", "@SWITCHKEY", "Switch to the next mode (music player, radio, audio books: the order and the modes that are on are set in Settings -> GENERAL -> MODES; the radio is closed completely first)"},
    {nullptr, "@SWITCHBACK", "Switch to the previous mode"},
    {nullptr, "?", "This cheatsheet (toggle)"},
    {nullptr, "<", "The MENUS index: every menu and overlay by category with its key (UP/DOWN/LEFT/RIGHT select, ENTER opens, ESC closes). SHIFT+, on an English keyboard"},
    {nullptr, "q / CTRL+C", "Quit (CTRL+C copies instead while you type in a text field)"},
    {nullptr, "ESC", "Close cheatsheet / overlay / menu; clear and leave the search box"},
    {nullptr, "ENTER", "Confirm / select (tunes the hovered station; on a station list or preset it opens it and tunes in)"},
    {nullptr, "ARROW KEYS", "Navigate (context-dependent); in a text field LEFT / RIGHT move the caret"},
    // --- Playback ---
    {"PLAYBACK (MAIN UI)", "ENTER", "Tune the hovered station"},
    {nullptr, "n", "Next channel in the list (always, whatever the mode)"},
    {nullptr, "#", "Shuffle: tune a random next channel (always, whatever the mode)"},
    {nullptr, "b", "Previous channel, follows the mode: S = the one played before, L = the one before it in the list"},
    {nullptr, "m", "Switch the mode: S (shuffle) <-> L (list); shown in the box next to SEARCH (only affects B)"},
    {nullptr, "p", "Mute / unmute (box shows MUTE / TUNE); with no channel loaded it tunes the hovered one instead"},
    {nullptr, "x", "Stop the stream"},
    {nullptr, "SHIFT+r", "Reconnect the tuned station"},
    {nullptr, "+ / -", "Volume up / down (5 % steps)"},
    {"SOUND (MAIN UI)", "v", "Toggle loudness normalisation"},
    {nullptr, "SHIFT+v", "Loudness normalisation overlay (target level, max boost)"},
    {nullptr, "SHIFT+e", "Equalizer overlay (the player's 10 bands + presets; the preset key e is lowercase and unaffected)"},
    {nullptr, "</>/UP/DOWN", "Equalizer: select band (LEFT / RIGHT) / gain +1 / -1 dB"},
    {nullptr, ",/./TAB/0/R", "Equalizer: previous / next preset / zero the band / reset to Flat (SPACE on/off)"},
    {nullptr, "S/DEL/X", "Equalizer: save the curve as a custom preset (ENTER saves, ESC cancels) / delete the selected one (press twice)"},
    {"VISUALIZATIONS (MAIN UI)", ".", "Cycle the scope block: oscilloscope / sphere / spectrogram, like . in the player (rebindable; what is off on the GENERAL tab is skipped)"},
    {nullptr, "SHIFT+i", "Spectrogram options overlay (view: spectrogram / waterfall, style, motion, scale, frequencies, gain, range, window, colors, channels, time span, the waterfall's graphs; R = Audacity's defaults)"},
    {nullptr, "SHIFT+b", "Spectrum bars overlay (under the upper pane, scrolls on a low terminal): style, Hi-Fi resolution / channels / gap / height / adapt / frequency scale / gain / range / colour, bar motion (toggle; R = defaults)"},
    {nullptr, "SHIFT+u", "Spectrogram full screen with frequency labels and time ruler (toggle; ESC closes)"},
    {nullptr, "SHIFT+0", "Oscilloscope full screen: YT mode left, XY mode right, values in the border (toggle; ESC closes; the SHIFT+o options open on top of it). The key is the character = (SHIFT+0 on a German keyboard)"},
    {nullptr, "SHIFT+a / v / c / p", "Oscilloscope full screen: axes / grid on or off   axis values on or off   grid colour (small window: type 0-255, empty = colour of inactive list elements)   grid metrics (small window: time unit, grid divisions, steady readouts in the picture names: RMS level, peak hold, L/R correlation, L/R balance)"},
    {nullptr, "SHIFT+o", "Oscilloscope overlay (osci music mode, style, frame rate up to 165 -- all per style --, image protocol, image resolution, afterglow, dot threshold, tail, Line/Vec. Interpol., Z-Axis, Z depth / source, trace length, rotation, mono phase portrait; R resets)"},
    {nullptr, ")", "Scope window: the oscilloscope in its own window, drawn by the graphics card, braille or image style as set in the SHIFT+o overlay (toggle; needs SDL2). In it: F / F11 / double-click fullscreen, T always on top, ESC closes"},
    {nullptr, "(", "Spectrogram window: the spectrogram in its own window, scrolling smoothly at the monitor's refresh rate (toggle; needs SDL2; same keys inside it)"},
    {"RECORD AND SLEEP TIMER (MAIN UI)", "SHIFT+z", "Sleep timer: 15 / 30 / 60 / 90 / 120 min, stops the stream (optionally with a fade-out of the volume)"},
    {nullptr, "y", "Record: opens the RECORD overlay (from now on = the moment y was pressed, the last 1 / 5 / 10 / 15 / 30 min, everything buffered); y again stops and saves the MP3"},
    {nullptr, "UP/DOWN/ENTER", "Record overlay: choose the start / start recording (y does the same; ESC cancels)"},
    {"OVERLAYS AND MENUS (MAIN UI)", "h", "Open the LISTENING HISTORY: HISTORY / TOP CHANNELS / HABITS"},
    {nullptr, "SHIFT+l", "Open the big STATIONS overlay (the STATIONS pane with the whole screen)"},
    {nullptr, "s", "Open the RADIO SETTINGS (colours, modes and switches, animation and visualizer, paths, keys, about; saved to radio_config.txt)"},
    {nullptr, "SHIFT+s", "Open the RADIO BROWSER menu: search stations worldwide (radio-browser.info)"},
    {nullptr, "SHIFT+k", "Open the STATION LISTS menu: build, save and load named station lists"},
    // --- Timeshift ---
    {"TIMESHIFT (MAIN UI, RADIO BUFFER 5-60 MIN)", "SPACE", "Pause / resume the station (the stream keeps buffering, you catch up later)"},
    {nullptr, "[ / ]", "Jump back / forward 30 seconds (shown for 4 s at the bottom of the scope block)"},
    {nullptr, "{", "Jump back 5 minutes"},
    {nullptr, "}", "Back to live"},
    // --- Navigation ---
    {"NAVIGATION (MAIN UI)", "UP / DOWN", "Move the cursor in the STATIONS list"},
    {nullptr, "SHIFT+t", "Toggle the STATIONS sort: list order <-> name A-Z (also for the search results)"},
    {nullptr, "\"", "Latin letters on / off: Japanese kana, Korean and Chinese station names and titles shown in Latin letters (the same switch as in the music player; SHIFT+' on a US keyboard)"},
    // --- Presets ---
    {"PRESETS (MAIN UI)", "1 2 3 4 5 6 7 8 9 0", "Tune slot 1-10 of the active preset"},
    {nullptr, "E R T D F G", "Tune slot 11-16 of the active preset (lowercase)"},
    {nullptr, "SHIFT+LEFT/RIGHT", "Previous / next preset (wraps; saved right away)"},
    {nullptr, "SHIFT+p", "Open the PRESETS menu"},
    // --- Search ---
    {"SEARCH (MAIN UI)", "/", "Search stations (matches name, genre and country)"},
    {nullptr, "TYPE", "Filter the list while you type; BACKSPACE deletes"},
    {nullptr, "UP / DOWN", "Search box: move through the results"},
    {nullptr, "ENTER", "Search box: tune the hovered result and leave the box"},
    {nullptr, "ESC", "Search box: clear the search and leave the box. Main screen: clear the search, then leave a list"},
    {nullptr, "P:", "Typed into the empty box (or /p:): search the STATION LISTS; ENTER shows the hovered list in STATIONS and tunes its first station"},
    {nullptr, "S:", "Typed into the empty box: back to searching the stations"},
    // --- Text fields ---
    {"TEXT FIELDS (SEARCH BOXES, STATION EDITOR, RADIO BROWSER PANES)", "LEFT / RIGHT", "Move the caret"},
    {nullptr, "SHIFT+LEFT/RIGHT", "Mark text (extend the selection); typing or pasting replaces it"},
    {nullptr, "HOME / END", "Caret to the start / end of the text"},
    {nullptr, "BACKSPACE / DEL", "Delete the marked text, else the character before / under the caret"},
    {nullptr, "CTRL+C", "Copy the marked text (nothing marked: the whole field)"},
    {nullptr, "CTRL+X", "Cut the marked text"},
    {nullptr, "CTRL+V", "Paste at the caret (replaces the marked text)"},
    {nullptr, "CTRL+z / CTRL+y", "Undo the last change in a text box (a typed run, a deletion or a paste is one step)"},
    // --- Presets menu ---
    {"PRESETS MENU", "TAB", "Cycle focus: SEARCH -> SELECT PRESET -> STATIONS"},
    {nullptr, "ARROW KEYS", "Navigate the focused pane"},
    {nullptr, "ENTER", "STATIONS: tune the hovered station / SELECT PRESET: open the preset and tune its first filled slot"},
    {nullptr, "/", "Search stations (type p: into the empty box to search presets, s: for stations again)"},
    {nullptr, "SHIFT+LEFT/RIGHT", "Previous / next preset, from any pane except the search box (there it marks text)"},
    {nullptr, "ESC", "Clear the search / close the menu"},
    {nullptr, "?", "This cheatsheet (not while typing in the search box)"},
    {nullptr, "CTRL+C", "Quit (in the search box it copies instead)"},
    {"STATION LISTS MENU (SHIFT+k)", "LEFT/RIGHT", "Switch tab: CREATE / EDIT <-> SAVED STATION LISTS; in a name / search box after typing: move the caret"},
    {nullptr, MUISC_ALT_NAME_UC "+LEFT/RIGHT", "Switch tab from every pane, also while typing"},
    {nullptr, "TAB", "Cycle focus. Tab 1: name, search, STATIONS, LIST CONTENTS. Tab 2: search, list"},
    {nullptr, "ENTER", "Name: on to the search / search, STATIONS: add hovered station / LIST CONTENTS: tune it"},
    {nullptr, "ENTER", "Tab 2: search -> the list / STATION LISTS: load the hovered list into tab 1"},
    {nullptr, "CTRL+s", "Save the list under its name (from every pane of tab 1). Same name overwrites"},
    {nullptr, "SHIFT+t", "STATIONS: toggle the sort, list order <-> name A-Z (also for the search results)"},
    {nullptr, "DEL / BACKSPACE / D", "LIST CONTENTS: remove the hovered station from the list"},
    {nullptr, "4 / 5", "LIST CONTENTS: move the hovered station up / down"},
    {nullptr, "DEL", "Tab 2, STATION LISTS: delete the hovered list (asks first)"},
    {nullptr, "/", "Back to the search box of the tab (not while typing)"},
    {nullptr, "ESC", "Leave the box you typed in (LEFT/RIGHT switch tabs again). Then: a search with text is cleared, else the menu closes (unsaved changes: asks Y / N / ESC)"},
    {nullptr, "J / K", "Move the cursor down / up in a list pane"},
    {"BIG STATIONS OVERLAY (SHIFT+l)", "/", "Search like in the main screen (typo tolerant; p: searches the station lists, ENTER opens one and tunes its first station, s: back to stations)"},
    {nullptr, "UP / DOWN", "Move the cursor; ENTER tunes the hovered station; n / b / # / p / x / + / - work as in the main screen"},
    {nullptr, "A", "Add a station by its stream URL (TAB: URL <-> NAME; an empty NAME becomes the host name); saved to stations.txt"},
    {nullptr, "SHIFT+c", "Edit entry: name, PRESET NAME (a shorter second name for the PRESETS pane), URL, genre, country, codec, kbps (TAB / UP / DOWN: field, ENTER saves)"},
    {nullptr, "DEL", "Delete the hovered station (a warning asks Y / N first); it leaves stations.txt, the presets and the station lists"},
    {nullptr, "SHIFT+u", "Undo the last deletion or edit of a station (up to 20 steps)"},
    {nullptr, "SHIFT+t", "Sort: list order <-> name A-Z"},
    {nullptr, "ESC / SHIFT+l", "Clear the search, leave a list, then close the overlay"},
    {"LISTENING HISTORY MENU (h)", "LEFT / RIGHT / TAB / 1 2 3", "Switch tab: HISTORY / TOP CHANNELS / HABITS"},
    {nullptr, "UP / DOWN / J / K", "Move the cursor (HISTORY, TOP CHANNELS) or scroll (HABITS); HOME / END (or G / SHIFT+g) jump to the ends"},
    {nullptr, "R", "TOP CHANNELS: most <-> least listened first"},
    {nullptr, "Y", "HISTORY: search the hovered line (artist + title) on YouTube in a small overlay"},
    {nullptr, "ENTER / TAB / ESC", "YouTube overlay: search, or download the hovered result / edit the query <-> results / close"},
    {nullptr, "ESC / q / h", "Close the history"},
    {"RADIO BROWSER MENU (SHIFT+s)", "TAB", "Cycle focus: NAME -> TAGS -> COUNTRY -> STATE -> LANGUAGE -> BITRATE -> RESULTS"},
    {nullptr, "ENTER", "In a pane: run the search (focus jumps to RESULTS) / in RESULTS: tune the hovered station"},
    {nullptr, "UP / DOWN", "In a pane: jump to the pane above / below (down from the last row: RESULTS) / in RESULTS: move the cursor"},
    {nullptr, "LEFT / RIGHT", "In a pane: move the caret (text-field keys above)"},
    {nullptr, "A", "In RESULTS: add the hovered station to your station list (saved to stations.txt, marked with a dot)"},
    {nullptr, "/", "In RESULTS: back to the NAME pane"},
    {nullptr, "ESC", "In a pane: clear it; empty pane or RESULTS: close the menu"},
    {nullptr, "?", "This cheatsheet (RESULTS only; in a pane the key is typed)"},
    {nullptr, "CTRL+C", "Quit (in a pane it copies instead)"},
    {"RADIO BROWSER MENU: THE PANES", "NAME", "Part of the station name"},
    {nullptr, "TAGS", "Comma separated, every tag has to match: rock, 80s"},
    {nullptr, "COUNTRY", "A name (Germany) or a 2-letter code (DE)"},
    {nullptr, "STATE", "Region inside the country (part of the name is enough)"},
    {nullptr, "LANGUAGE", "As Radio Browser spells it: german, english"},
    {nullptr, "BITRATE", "kbps: 128 = at least 128, 64-192 = range, -192 = at most 192"},
    {"PRESETS MENU: STATIONS PANE", "1-0 / E R T D F G", "Set the hovered station as that slot (same key again clears it)"},
    {nullptr, "DEL / BACKSPACE", "Remove the hovered station from the active preset"},
    {nullptr, "J / K", "Move the cursor down / up"},
    {"PRESETS MENU: SELECT PRESET PANE", "ARROWS / H J K L", "Move in the 4-column grid"},
    {nullptr, "ENTER", "Open the hovered preset and tune its first filled slot"},
    {nullptr, "SHIFT+n", "New preset (opens the name box)"},
    {nullptr, "SHIFT+c", "Rename the hovered preset"},
    {nullptr, "SHIFT+d", "Delete the hovered preset (press twice; the last preset cannot be deleted)"},
    {nullptr, "ENTER / ESC", "Name box: confirm / cancel"},
    {"RADIO SETTINGS (s)", "TAB", "Next tab: COLORS / GENERAL / ANIMA/VIZ / PATHS / REFERENCE / ABOUT APP"},
    {nullptr, "UP / DOWN", "Move through the rows (LEFT / RIGHT or ENTER change a switch; ENTER edits a colour, a folder or a key)"},
    {nullptr, "LEFT / RIGHT (editing)", "Move the caret in the colour, folder or key being edited; TAB and the arrows stay in the field until ENTER (apply) or ESC (cancel)"},
    {nullptr, "ENTER (THEME row)", "COLORS tab: ENTER opens CHOOSE THEME (scrollable, ENTER applies, SHIFT+d deletes a theme of your own); the CREATE THEME button builds your own theme"},
    {nullptr, "e", "COLORS tab: edit the theme in use in the theme creator (a theme of your own can be overwritten, a built-in one needs a new name)"},
    {nullptr, "SHIFT+e / SHIFT+i", "COLORS tab: export the current colors as a theme file / import theme files from a folder"},
    {nullptr, "s", "Save radio_config.txt and close (ESC / q close without saving; with unsaved changes it asks first)"},
    {nullptr, "ESC / q", "Discard the changes made on the screen and close"},
    {nullptr, "GENERAL: MARQUEE SCROLLING", "A station name that is too long for its column scrolls; off: it is cut off with ..."},
    {nullptr, "GENERAL: TUNING NOISE", "Radio static that fades in when you tune another station and fades out when the stream plays"},
    {nullptr, "REFERENCE: ENTER", "Change the key of the hovered command; a key that is already taken is refused with the name of its owner"},
    {nullptr, "REFERENCE: DEL", "Restore the default key of the hovered command"},
    {nullptr, "REFERENCE: RESET", "The first row under the note: every key back to its default"},
    {nullptr, "CTRL+SHIFT+u", "Undo the last key change or reset (up to 5, newest first); the cursor jumps to the key that came back"},
};

// ===========================================================================================
// The help wizard (SHIFT+w): the same pages the music player shows (src/wizard.h), drawn in
// the MUSIC PLAYER's colours rather than the radio's, so the feature looks like one thing in
// both modes. The text, layout and state are all wizard::render()'s; this only paints them.
// ===========================================================================================

std::vector<std::string> build_wizard(const Style& s, const UiModel& m, int W, int rows) {
    const WizardPalette p = player_wizard_palette();
    const std::string wb  = ansi_fg(p.border);
    const std::string wbb = ansi_fg(p.border_bot);
    const int body_rows = std::max(1, rows - 2);        // the top and bottom border take two rows

    static const muisc::DiskArt disk;
    std::vector<std::string> disk_frame;
    if (wizard::shows_disk(m.wizard.page)) disk_frame = disk.frame(m.wizard_angle);
    const wizard::View v = wizard::render(m.wizard, W, body_rows, disk_frame);

    const std::string lead = spaces(v.left);
    const std::string trail = spaces(std::max(0, W - v.left - v.box_w));
    const std::string bar = wb + s.c.box_vertical + kReset;
    const std::string header = "\x1b[1m" + ansi_fg(p.header);
    const std::string legend = ansi_fg(p.legend);
    const std::string base = ansi_fg(p.list);
    std::string cursor = ansi_fg(p.cursor_fg) + ansi_bg(p.cursor_bg);
    if (cursor.empty()) cursor = "\x1b[7m";

    std::vector<std::string> out;
    // the title is drawn in the header colour; box_edge() brings the border colour back in front of the dashes
    out.push_back(lead + box_top(s, v.box_w, header + v.title + kReset, wb) + trail);
    for (const wizard::Row& row : v.rows) {
        std::string line = lead + bar;
        for (const wizard::Span& sp : row) {
            switch (sp.role) {
                case wizard::Role::Header: line += header + sp.text + kReset; break;
                case wizard::Role::Legend: line += legend + sp.text + kReset; break;
                case wizard::Role::Cursor: line += cursor + sp.text + kReset; break;
                case wizard::Role::Match:  line += header + "\x1b[4m" + sp.text + kReset; break;
                case wizard::Role::Disk:
                case wizard::Role::Scroll: line += gradient_fg(p.disk_top, p.disk_bottom, sp.t) + sp.text + kReset; break;
                default:                   line += base + sp.text + kReset; break;
            }
        }
        out.push_back(line + bar + trail);
    }

    std::string bottom = "[SHIFT+w / ESC] close";
    if (v.search) {
        bottom = "[ESC] clear / back  [ENTER] open the chapter  [\xE2\x86\x91\xE2\x86\x93] choose  [SHIFT+\xE2\x86\x91\xE2\x86\x93] jump 5";
        out.push_back(lead + box_bottom(s, v.box_w, bottom, wbb) + trail);
        return out;
    }
    bottom += "  [t] TOC  [SHIFT+s] search";
    if (v.can_back) bottom += "  [<-] back";
    if (v.can_next) bottom += "  [->] next";
    if (v.toc_page) bottom += "  [\xE2\x86\x91\xE2\x86\x93] select  [SHIFT+\xE2\x86\x91\xE2\x86\x93] jump  [ENTER] open";
    else if (v.scrollable) bottom += "  [\xE2\x86\x91\xE2\x86\x93] scroll  [SHIFT+\xE2\x86\x91\xE2\x86\x93] page";
    out.push_back(lead + box_bottom(s, v.box_w, bottom, wbb) + trail);
    return out;
}

std::vector<std::string> build_cheatsheet(const Style& s, const UiModel& m, int W, int rows) {
    std::vector<std::string> out;
    out.push_back(box_top(s, W, "CHEATSHEET", s.border));

    const int visible = std::max(1, rows - 2);        // the top and bottom border take two rows
    const int inner = std::max(0, W - 4);

    // Every entry becomes display lines first (a category: blank spacer + title; a long description wraps), so the scroll
    // range counts what is really drawn. The description column starts after the widest key + 2 spaces.
    int key_w = 0;
    for (const auto& r : kCheatRows) key_w = std::max(key_w, display_width(pretty_key(r.key)));
    key_w = std::min(key_w, std::max(10, inner / 2));
    const int desc_col = key_w + 2;
    const int desc_w = std::max(10, inner - desc_col);
    const std::string header_bold = "\x1b[1m" + s.legend;   // bold + Header colour, like the player's header_sgr()
    struct DLine { std::string plain, ansi; };
    std::vector<DLine> dl;
    std::vector<int> row_dline;
    std::vector<std::string> row_category;
    std::string category;
    for (const auto& r : kCheatRows) {
        row_dline.push_back(static_cast<int>(dl.size()) + (r.header ? 1 : 0));
        if (r.header) category = r.header;
        row_category.push_back(category);
        if (r.header) { dl.push_back({"", ""}); dl.push_back({r.header, header_bold}); }
        const std::string key = pretty_key(r.key);
        const auto parts = wrap_words(r.desc, desc_w);
        for (size_t i = 0; i < parts.size(); ++i)
            dl.push_back({(i == 0 ? pad_right(truncate_str(key, key_w), desc_col) : spaces(desc_col)) + parts[i], ""});
    }
    const int total = static_cast<int>(dl.size());
    const int max_scroll = std::max(0, total - visible);
    if (m.cheat_jump_row >= 0 && m.cheat_jump_row < static_cast<int>(row_dline.size()))
        m.cheat_scroll = std::max(0, row_dline[static_cast<size_t>(m.cheat_jump_row)] - 2);   // the category title above stays in view
    m.cheat_jump_row = -1;
    m.cheat_scroll = std::clamp(m.cheat_scroll, 0, max_scroll);

    if (m.cheat_searching) {
        // ----- SHIFT+s: the search view replaces the table -----
        const std::string mark = header_bold + "\x1b[4m";
        const std::vector<std::string> words = muisc::helpsearch::split_words(m.cheat_query);
        struct Res { int row; muisc::helpsearch::Hit hit; std::string doc; };
        std::vector<Res> res;
        if (!words.empty()) {
            size_t i = 0;
            for (const auto& r : kCheatRows) {
                // the key is padded to the table's key column, so the descriptions line up as in the table
                std::string doc = pad_right(truncate_str(pretty_key(r.key), key_w), desc_col) + r.desc + "  [" + row_category[i] + "]";
                muisc::helpsearch::Hit h = muisc::helpsearch::match(words, doc);
                if (muisc::helpsearch::worth_showing(h)) res.push_back({static_cast<int>(i), std::move(h), std::move(doc)});
                ++i;
            }
            std::stable_sort(res.begin(), res.end(), [](const Res& a, const Res& b) { return muisc::helpsearch::better(a.hit, b.hit); });
            if (res.size() > 300) res.resize(300);
        }
        m.cheat_result_rows.clear();
        for (const Res& r : res) m.cheat_result_rows.push_back(r.row);
        m.cheat_sel = std::clamp(m.cheat_sel, 0, std::max(0, static_cast<int>(res.size()) - 1));

        int used = 0;
        {   // the query: caret / marked range in reverse video
            std::string q = m.cheat_query;
            const size_t caret = std::min(m.cheat_qcaret, q.size()), anchor = std::min(m.cheat_qanchor, q.size());
            const size_t a = std::min(caret, anchor), b = std::max(caret, anchor);
            std::vector<Seg> segs = {{header_bold, "Search: "}};
            if (a != b) {
                segs.push_back({"", q.substr(0, a)});
                segs.push_back({"\x1b[7m", q.substr(a, b - a)});
                segs.push_back({"", q.substr(b)});
            } else {
                size_t n = 0;
                if (caret < q.size()) { n = 1; while (caret + n < q.size() && (static_cast<unsigned char>(q[caret + n]) & 0xC0) == 0x80) ++n; }
                segs.push_back({"", q.substr(0, caret)});
                segs.push_back({"\x1b[7m", n ? q.substr(caret, n) : std::string(" ")});
                segs.push_back({"", q.substr(caret + n)});
            }
            out.push_back(box_line(s, seg_line(segs, inner), s.border));
            ++used;
        }
        {
            int full = 0;
            for (const Res& r : res) if (r.hit.full()) ++full;
            std::string info;
            if (words.empty()) info = "Type what you look for: words in any order, typos and related words are found too.";
            else if (res.empty()) info = "Nothing found. Try fewer or other words.";
            else info = std::to_string(res.size()) + (res.size() == 1 ? " command" : " commands") + (full < static_cast<int>(res.size()) ? " (" + std::to_string(full) + " with all words, the rest with some)" : "");
            out.push_back(box_line(s, seg_line({{s.legend, info}}, inner), s.border));
            out.push_back(box_line(s, spaces(inner), s.border));
            used += 2;
        }
        const int text_w = std::max(10, inner - 2);
        auto wrap_offsets = [&](const std::string& str, int width, int cont_width, size_t max_lines) {
            std::vector<std::pair<size_t, size_t>> lines;
            size_t i = 0, line_start = std::string::npos, line_end = 0;
            int line_w = 0;
            while (i < str.size()) {
                const size_t gap_from = i;
                while (i < str.size() && str[i] == ' ') ++i;
                if (i >= str.size()) break;
                size_t j = i;
                while (j < str.size() && str[j] != ' ') ++j;
                const int ww = display_width(str.substr(i, j - i));
                const int gap = static_cast<int>(i - gap_from);
                if (line_start != std::string::npos && line_w + gap + ww > (lines.empty() ? width : cont_width)) {
                    lines.emplace_back(line_start, line_end);
                    line_start = std::string::npos;
                    if (lines.size() >= max_lines) return lines;
                }
                if (line_start == std::string::npos) { line_start = i; line_w = ww; }
                else line_w += gap + ww;
                line_end = j;
                i = j;
            }
            if (line_start != std::string::npos && lines.size() < max_lines) lines.emplace_back(line_start, line_end);
            return lines;
        };
        std::vector<std::vector<std::pair<size_t, size_t>>> blocks;
        for (const Res& r : res) blocks.push_back(wrap_offsets(r.doc, text_w, std::max(10, text_w - desc_col), 3));
        const int room = std::max(1, visible - used);
        m.cheat_first = std::clamp(m.cheat_first, 0, std::max(0, static_cast<int>(res.size()) - 1));
        if (m.cheat_sel < m.cheat_first) m.cheat_first = m.cheat_sel;
        for (;;) {
            int h = 0;
            for (int i = m.cheat_first; i <= m.cheat_sel && i < static_cast<int>(res.size()); ++i) h += static_cast<int>(blocks[static_cast<size_t>(i)].size());
            if (h <= room || m.cheat_first >= m.cheat_sel) break;
            ++m.cheat_first;
        }
        int rows_left = room;
        for (int i = m.cheat_first; i < static_cast<int>(res.size()) && rows_left > 0; ++i) {
            const auto& bl = blocks[static_cast<size_t>(i)];
            const Res& r = res[static_cast<size_t>(i)];
            for (size_t k = 0; k < bl.size() && rows_left > 0; ++k, --rows_left) {
                std::vector<Seg> segs = {{(k == 0 && i == m.cheat_sel) ? "\x1b[7m" : "", (k == 0 && i == m.cheat_sel) ? "\xE2\x96\xB6 " : "  "}};
                if (k > 0) segs.push_back({"", spaces(desc_col)});   // wrapped lines continue under the description column
                size_t pos = bl[k].first;
                for (const auto& rg : r.hit.ranges) {
                    if (rg.to <= bl[k].first || rg.from >= bl[k].second) continue;
                    const size_t a = std::max(rg.from, bl[k].first), b = std::min(rg.to, bl[k].second);
                    if (a > pos) segs.push_back({"", r.doc.substr(pos, a - pos)});
                    segs.push_back({mark, r.doc.substr(a, b - a)});
                    pos = b;
                }
                if (pos < bl[k].second) segs.push_back({"", r.doc.substr(pos, bl[k].second - pos)});
                out.push_back(box_line(s, seg_line(segs, inner), s.border));
            }
        }
        while (rows_left-- > 0) out.push_back(box_line(s, spaces(inner), s.border));
        out.push_back(box_bottom(s, W, "[ESC] clear / back  [ENTER] show in the table  [\xE2\x86\x91\xE2\x86\x93] choose", s.border_bottom));
        return out;
    }
    int shown = 0;
    for (int i = m.cheat_scroll; i < total && shown < visible; ++i, ++shown)
        out.push_back(box_line(s, seg_line({{dl[static_cast<size_t>(i)].ansi, dl[static_cast<size_t>(i)].plain}}, inner), s.border));
    while (shown < visible) { out.push_back(box_line(s, spaces(inner), s.border)); ++shown; }

    std::string bottom = "[? / ESC] close  [SHIFT+s] search";
    if (max_scroll > 0) bottom += "  [\u2191\u2193] scroll  [SHIFT+\u2191\u2193] Page  " + std::to_string(m.cheat_scroll + 1) + "/" + std::to_string(total);
    out.push_back(box_bottom(s, W, bottom, s.border_bottom));
    return out;
}

// ===========================================================================================
// The big STATIONS overlay (key L): the main STATIONS pane with the whole screen for its rows.
// ===========================================================================================
constexpr int kAddOverlayW = 76;
constexpr int kEditOverlayW = 76;
constexpr int kDelOverlayW = 64;

// The key legend of the big STATIONS overlay: a group continues on further rows when its keys do not fit one (broken between two
// commands, never inside one), so on a narrow terminal nothing is cut off. The pane gives the rows the legend needs.
static std::vector<LegendGroup> stations_legend_groups(const StationsOverlay& so) {
    std::vector<LegendGroup> g;
    g.push_back({"Move", {"[\u2191\u2193] Navi.", "[ENTER] Tune/Open", "[/] Search", "[p:] Lists", "[ESC] Clear / Exit\001Exit"}, 3});
    g.push_back({"Edit", {"[a] Add URL", "[SHIFT+c] Edit entry\001Edit", "[DEL] Delete\001Del",
                          so.undo_count > 0 ? "[SHIFT+u] Undo (" + std::to_string(so.undo_count) + ")\001Undo" : "[SHIFT+u] Undo"}, 3});
    g.push_back({"Playback", {"[n/b/#] Next / back / random\001Next/Back", "[p] Mute", "[x] Stop", "[+/-] Volume"}, 3});
    g.push_back({"View", {"[SHIFT+t] Sort", "[L] Close"}, 3});
    return g;
}

std::vector<std::string> build_stations_overlay(const Style& s, const UiModel& m, const RadioStatus& st, int W, int rows) {
    const StationsOverlay& so = m.stov;
    const std::vector<Station> none;
    const auto& stations = m.stations ? *m.stations : none;
    std::vector<std::string> out = build_search(s, m, st, W);                 // the same search box as the main screen
    // the legend as tall as it has to be (a third of the screen at most), then the status line
    const int legend_rows = static_cast<int>(layout_legend_tree(stations_legend_groups(so), W - 1, std::max(3, rows / 3)).size());
    const int hint_rows = legend_rows + 1;
    const int body = std::max(3, rows - static_cast<int>(out.size()) - 2 - hint_rows);   // rows between the pane's borders
    const int inner = W - 4;
    const std::string sep = s.c.list_separator;

    std::string label;
    if (m.search_lists) label = "STATION LISTS (ENTER: show the stations)";
    else if (!m.active_list.empty()) label = "STATIONS (LIST: " + utf8_take(m.active_list, 24) + " - ESC: all)";
    else label = std::string("STATIONS (") + (m.sort_az ? "sort: A-Z" : "sort: list order") + ")";
    const int total = m.search_lists ? static_cast<int>(m.lvisible.size()) : static_cast<int>(m.visible.size());
    out.push_back(box_top(s, W, label, s.border));

    const StationList* shown_list = nullptr;
    if (!m.active_list.empty()) for (const auto& l : m.lists) if (l.name == m.active_list) { shown_list = &l; break; }
    const int idx_w = static_cast<int>(std::to_string(std::max<size_t>(1, shown_list ? shown_list->items.size() : stations.size())).size());
    const int country_w = 16, tail_w = 8;
    const int flex = std::max(12, inner - (idx_w + 5 * (2 + static_cast<int>(display_width(sep))) + country_w + tail_w));
    const int name_w = flex * 40 / 100, pre_w = flex * 20 / 100, genre_w = flex - name_w - pre_w;

    if (m.search_lists) {
        const int lcursor = std::clamp(m.lcursor, 0, std::max(0, total - 1));
        const int first = follow_scroll(scroll_id(7), lcursor, body, total);
        for (int r = 0; r < body; ++r) {
            const int li = first + r;
            if (li >= total) {
                out.push_back(box_line(s, seg_line({{s.header, total == 0 && r == 0 ? (m.lists.empty() ? "no station lists yet - SHIFT+k creates one" : "no station list matches") : ""}}, inner), s.border));
                continue;
            }
            const StationList& l = m.lists[static_cast<size_t>(m.lvisible[static_cast<size_t>(li)])];
            const std::string cnt = std::to_string(l.items.size()) + (l.items.size() == 1 ? " station" : " stations");
            const int nw = std::max(5, inner - 3 - (2 * display_width(sep) + 3) - 13);
            const std::string text = pad_right(std::to_string(li + 1), 3) + sep + " " + pad_right(truncate_str(shown_text(l.name), nw), nw) + " " + sep + " " + pad_right(cnt, 13);
            out.push_back(box_line(s, seg_line({{li == lcursor && m.search_focus ? s.list_cursor : s.list, text}}, inner), s.border));
        }
        out.push_back(box_bottom(s, W, std::max(0, total - (first + body)) > 0 ? "( " + std::to_string(total - (first + body)) + " more )" : "", s.border_bottom));
        m.stov.marquee_row = -1;
    } else {
        // column captions (the first row of the pane)
        out.push_back(box_line(s, seg_line({{s.header, pad_right("NO", idx_w) + sep + " " + pad_right("NAME", name_w) + " " + sep + " " + pad_right("PRESET NAME", pre_w) + " " + sep + " "
                                                       + pad_right("GENRE", genre_w) + " " + sep + " " + pad_right("COUNTRY", country_w) + " " + sep + " " + pad_right("STREAM", tail_w)}}, inner), s.border));
        const int rowsn = body - 1;
        const int cursor = std::clamp(m.cursor, 0, std::max(0, total - 1));
        const int first = follow_scroll(scroll_id(8), cursor, rowsn, total);
        for (int r = 0; r < rowsn; ++r) {
            const int vi = first + r;
            if (vi >= total) { out.push_back(box_line(s, seg_line({{s.header, total == 0 && r == 0 ? "no station matches" : ""}}, inner), s.border)); continue; }
            const int idx = m.visible[static_cast<size_t>(vi)];
            const Station& stn = stations[static_cast<size_t>(idx)];
            std::string tail = stn.codec_hint;
            if (stn.bitrate_hint > 0) tail = std::to_string(stn.bitrate_hint) + "k " + stn.codec_hint;
            const bool hovered = vi == cursor;   // also while searching (Up/Down move it, ENTER tunes it)
            const std::string name_shown = hovered ? marquee_or_truncate(so.marquee_row, so.marquee_since, m.t_sec, shown_text(stn.name), name_w, idx)
                                                   : pad_right(truncate_str(shown_text(stn.name), name_w), name_w);
            int number = idx + 1;
            if (shown_list) number = static_cast<int>(std::find(shown_list->items.begin(), shown_list->items.end(), idx) - shown_list->items.begin()) + 1;
            const std::string text = pad_right(std::to_string(number), idx_w) + sep + " " + name_shown + " " + sep + " " + pad_right(truncate_str(stn.preset_name, pre_w), pre_w) + " " + sep + " "
                                   + pad_right(truncate_str(stn.genre, genre_w), genre_w) + " " + sep + " " + pad_right(truncate_str(stn.country, country_w), country_w) + " " + sep + " "
                                   + pad_right(truncate_str(tail, tail_w), tail_w);
            std::string style = (idx == st.tuned_index && st.state != StreamState::Idle) ? s.list_playing : s.list;
            if (hovered) style = s.list_cursor;
            out.push_back(box_line(s, seg_line({{style, text}}, inner), s.border));
        }
        if (m.search_focus || total == 0) m.stov.marquee_row = -1;
        const int hidden = std::max(0, total - (first + rowsn));
        out.push_back(box_bottom(s, W, hidden > 0 ? "( " + std::to_string(hidden) + " more )" : "", s.border_bottom));
    }
    push_legend(out, s, stations_legend_groups(so), W, std::max(3, rows / 3));
    out.push_back(seg_line({{s.header, utf8_take(so.flash, W)}}, W));
    while (static_cast<int>(out.size()) < rows) out.push_back(spaces(W));
    out.resize(static_cast<size_t>(rows));

    // --- the two small overlays, drawn over the finished screen
    auto splice_panel = [&](const std::vector<std::string>& panel, int pw) {
        const int ph = static_cast<int>(panel.size());
        const int x = std::max(0, (W - pw) / 2);
        const int y = std::clamp((rows - ph) / 2 - 2, 0, std::max(0, rows - ph));
        for (int i = 0; i < ph; ++i) out[static_cast<size_t>(y + i)] = splice_line(out[static_cast<size_t>(y + i)], x, panel[static_cast<size_t>(i)], pw);
    };
    const std::string base = s.c.list_fg.empty() ? "" : ansi_fg(s.c.list_fg);
    if (so.add_open) {
        const int pw = std::min(kAddOverlayW, W), pi = pw - 4;
        std::vector<std::string> panel;
        panel.push_back(box_top(s, pw, "Add a station by URL", s.border));
        panel.push_back(box_line(s, field_row(base, "URL:  ", so.add_url, so.add_edit[0], so.add_field == 0, pi, "", s.legend, false), s.border));
        panel.push_back(box_line(s, field_row(base, "NAME: ", so.add_name, so.add_edit[1], so.add_field == 1, pi, so.add_name.empty() && so.add_field != 1 ? "(optional)" : "", s.legend, false), s.border));
        panel.push_back(box_line(s, seg_line({{s.legend, so.add_error.empty() ? "http(s) stream address; an empty NAME becomes the host name" : so.add_error}}, pi), s.border));
        panel.push_back(box_bottom(s, pw, "[ENTER] add  [TAB] URL / NAME  [ESC] cancel", s.border_bottom));
        splice_panel(panel, pw);
    }
    if (so.edit_open) {
        static const char* const labels[StationsOverlay::kEditFields] = {"NAME:        ", "PRESET NAME: ", "URL:         ", "GENRE:       ", "COUNTRY:     ", "CODEC:       ", "KBPS:        "};
        static const char* const helps[StationsOverlay::kEditFields] = {
            "The station's name in the lists", "A shorter name for the PRESETS pane; empty = the station name", "http(s) stream address",
            "e.g. Jazz", "e.g. DE", "AAC, MP3 ... (shown until the stream tells)", "0 = unknown"};
        const int pw = std::min(kEditOverlayW, W), pi = pw - 4;
        std::vector<std::string> panel;
        panel.push_back(box_top(s, pw, "Edit entry", s.border));
        for (int f = 0; f < StationsOverlay::kEditFields; ++f)
            panel.push_back(box_line(s, field_row(base, labels[f], so.edit_text[f], so.edit_ed[f], so.edit_field == f, pi, "", s.legend, false), s.border));
        panel.push_back(box_line(s, seg_line({{s.legend, so.edit_error.empty() ? std::string(helps[std::clamp(so.edit_field, 0, StationsOverlay::kEditFields - 1)]) : so.edit_error}}, pi), s.border));
        panel.push_back(box_bottom(s, pw, "[ENTER] save  [TAB/\u2191\u2193] field  [ESC] cancel", s.border_bottom));
        splice_panel(panel, pw);
    }
    if (so.del_idx >= 0 && so.del_idx < static_cast<int>(stations.size())) {
        const int pw = std::min(kDelOverlayW, W), pi = pw - 4;
        const Station& d = stations[static_cast<size_t>(so.del_idx)];
        std::vector<std::string> panel;
        panel.push_back(box_top(s, pw, "Delete station", s.border));
        panel.push_back(box_line(s, seg_line({{s.header, utf8_take(d.name, pi)}}, pi), s.border));
        panel.push_back(box_line(s, seg_line({{"\x1b[41;97m", pad_right(utf8_take(" Removed from the stations file, all presets and lists.", pi), pi)}}, pi), s.border));
        panel.push_back(box_line(s, seg_line({{s.legend, "SHIFT+u takes it back as long as Mousiki runs."}}, pi), s.border));
        panel.push_back(box_bottom(s, pw, "[Y]es delete  [N]o / [ESC] keep", s.border_bottom));
        splice_panel(panel, pw);
    }
    return out;
}

// ===========================================================================================
// LISTENING HISTORY (Shift+H) -- laid out like the music player's history screen: a tab strip, one panel, a legend and a
// status line. The YouTube overlay (`y` on the HISTORY tab) is drawn over the finished screen.
// ===========================================================================================
constexpr int kYtOverlayW = 80;
constexpr int kYtResultRows = 8;

std::string fmt_clock_len(double sec) {   // 3:45 / 1:02:03 for a song length; "" when unknown
    if (sec < 0) return "";
    const long long t = static_cast<long long>(sec + 0.5);
    char buf[32];
    if (t >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else std::snprintf(buf, sizeof buf, "%lld:%02lld", t / 60, t % 60);
    return buf;
}

std::vector<std::string> build_history_menu(const Style& s, const UiModel& m, int W, int rows) {
    const HistoryModel& hm = m.hmenu;
    const int inner = W - 4;
    const std::string base = s.c.list_fg.empty() ? "" : ansi_fg(s.c.list_fg);
    std::vector<std::string> out;
    {
        const auto strip = menu_tab_strip(s, W, "LISTENING HISTORY", {"HISTORY", "TOP CHANNELS", "HABITS"}, hm.tab, s.border, s.border);
        out.push_back(strip[0]);
        out.push_back(strip[1]);
    }
    // one fused pane: strip (2 lines) + body + bottom border (carries the info text) + hint + status
    const int body = std::max(4, rows - 2 - 1 - 2);
    std::string info;                                         // sits on the bottom border line
    if (hm.tab == 0 && hm.hist) info = std::to_string(hm.hist->size()) + " lines (max " + std::to_string(kMaxHistoryEntries) + ")";
    else if (hm.tab == 1) info = hm.most_first ? "most listened first" : "least listened first";

    struct Line { std::string text; int style = 0; };        // 0 plain, 1 header, 2 section header, 3 rule
    std::vector<Line> lines;
    int cursor = -1;
    auto T = [](const std::string& str, int n) { return pad_right(truncate_str(shown_text(str), n), n); };
    auto R = [](const std::string& str, int n) { return pad_left(truncate_str(str, n), n); };
    // Marks the plain-text cells with spaces so the columns line up; colouring is only done per whole line.
    if (hm.tab == 0) {
        const int when_w = 11, len_w = 7, chan_w = 22;
        const int rest = std::max(10, inner - when_w - chan_w - len_w - 6);
        const int art_w = rest * 2 / 5, tit_w = rest - art_w;
        lines.push_back({T("WHEN", when_w) + "  " + T("CHANNEL", chan_w) + "  " + T("ARTIST", art_w) + " " + T("TITLE", tit_w) + " " + R("HEARD", len_w), 1});
        const size_t n = hm.hist ? hm.hist->size() : 0;
        if (n == 0) lines.push_back({"nothing recorded yet -- tune a station; a line is added whenever artist or title changes", 0});
        for (size_t i = 0; i < n; ++i) {
            const HistoryEntry& e = hm.hist->newest(i);
            const bool live = i == 0 && hm.hist->live_at_top();
            const std::string ttl = e.title.empty() && e.artist.empty() ? "(no title)" : e.title;
            lines.push_back({T(live ? "now" : muisc::format_when(e.at), when_w) + "  " + T(e.channel, chan_w) + "  " + T(e.artist, art_w) + " " +
                             T(ttl, tit_w) + " " + R(muisc::format_mmss(e.listened), len_w), 0});
        }
        if (n > 0) cursor = std::clamp(hm.cursor, 0, static_cast<int>(n) - 1) + 1;
    } else if (hm.tab == 1) {
        const int songs_w = 6, time_w = 10;
        const int chan_w = std::max(10, inner - songs_w - time_w - 4);
        lines.push_back({T("CHANNEL", chan_w) + "  " + R("LINES", songs_w) + "  " + R(std::string("LISTENED ") + (hm.most_first ? "▼" : "▲"), time_w), 1});
        lines.push_back({repeat("─", inner), 3});
        const std::vector<HistoryChannelRow> top = hm.hist ? hm.hist->top_channels(hm.most_first) : std::vector<HistoryChannelRow>{};
        if (top.empty()) lines.push_back({"nothing recorded yet", 0});
        for (const auto& r : top)
            lines.push_back({T(r.channel, chan_w) + "  " + R(std::to_string(r.songs), songs_w) + "  " + R(muisc::format_len(r.listened), time_w), 0});
        if (!top.empty()) cursor = std::clamp(hm.top_cursor, 0, static_cast<int>(top.size()) - 1) + 2;
    } else {
        const HistoryStats st = hm.hist ? hm.hist->stats(hm.now) : HistoryStats{};
        const int lbl_w = std::max(20, inner - 24);
        auto num1 = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return std::string(b); };
        auto cat = [&](const char* name) { if (!lines.empty()) lines.push_back({"", 0}); lines.push_back({name, 2}); };
        auto item = [&](const std::string& label, const std::string& value) { lines.push_back({T(label, lbl_w) + value, 0}); };
        cat("SESSIONS");
        item("Sessions (a gap of 30 min starts a new one)", std::to_string(st.sessions));
        item("Average session length", muisc::format_len(st.avg_session_sec));
        item("Songs per session", num1(st.songs_per_session));
        cat("TIME LISTENED");
        item("Listened today", muisc::format_len(st.today_sec));
        item("Average day with radio", muisc::format_len(st.avg_day_sec));
        item("Days with radio", std::to_string(st.days));
        item("Total listened", muisc::format_len(st.total_sec));
        cat("CHANNELS");
        item("History lines (lifetime)", std::to_string(st.entries));
        item("Different channels", std::to_string(st.channels));
        item("Favourite channel", st.top_channel.empty() ? std::string("-") : truncate_str(st.top_channel, 40));
        item("Busiest hour", st.busiest_hour < 0 ? std::string("-") : (st.busiest_hour < 10 ? "0" : "") + std::to_string(st.busiest_hour) + ":00");
        cat("LISTENING BY HOUR OF THE DAY");
        {
            static const char* lv[9] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
            double mx = 0;
            for (double v : st.hours) mx = std::max(mx, v);
            std::string bars = "  ", labs = "  ";
            for (int h = 0; h < 24; ++h) {
                const int l = mx > 0 ? static_cast<int>(std::ceil(st.hours[static_cast<size_t>(h)] / mx * 8.0 - 1e-9)) : 0;
                bars += std::string(" ") + lv[std::clamp(l, 0, 8)] + lv[std::clamp(l, 0, 8)] + " ";
                char b[8]; std::snprintf(b, sizeof b, " %02d ", h);
                labs += b;
            }
            lines.push_back({bars, 0});
            lines.push_back({labs, 0});
        }
        cat("LISTENING BY WEEKDAY");
        {
            // The bar and the AVERAGE column show the average time on one such weekday (HistoryStats::weekday_avg), the
            // CUMULATIVE column the lifetime total. Only the two value columns get a name, in the header colour.
            static const char* wd[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
            const int day_w = 11, avg_w = 10, cum_w = 10;   // "AVERAGE" / "CUMULATIVE" and values like "123h 45m"
            const int bar_w = std::clamp(inner - (2 + day_w + 2 + avg_w + 2 + cum_w), 10, 40);
            lines.push_back({spaces(2 + day_w + bar_w + 2) + T("AVERAGE", avg_w) + "  " + T("CUMULATIVE", cum_w), 2});
            double mx = 0;
            for (double v : st.weekday_avg) mx = std::max(mx, v);
            for (int d = 0; d < 7; ++d) {
                const double avg = st.weekday_avg[static_cast<size_t>(d)];
                const double cum = st.weekdays[static_cast<size_t>(d)];
                const int len = mx > 0 ? std::clamp(static_cast<int>(std::round(avg / mx * bar_w)), 0, bar_w) : 0;
                lines.push_back({"  " + pad_right(wd[d], day_w) + repeat("█", len) + spaces(bar_w - len) + "  " +
                                 T(muisc::format_len(avg), avg_w) + "  " + T(muisc::format_len(cum), cum_w), 0});
            }
        }
    }

    int start;
    if (cursor >= 0) start = std::clamp(cursor - (body - 1) / 2, 0, std::max(0, static_cast<int>(lines.size()) - body));
    else {
        hm.scroll = std::clamp(hm.scroll, 0, std::max(0, static_cast<int>(lines.size()) - body));
        start = hm.scroll;
    }
    int shown = 0;
    for (int k = start; k < static_cast<int>(lines.size()) && shown < body; ++k, ++shown) {
        const Line& ln = lines[static_cast<size_t>(k)];
        const std::string txt = pad_right(truncate_str(ln.text, inner), inner);
        if (ln.style == 3) out.push_back(box_line(s, s.legend + txt + kReset, s.border));
        else if (ln.style == 1 || ln.style == 2) out.push_back(box_line(s, s.header + txt + kReset, s.border));
        else if (k == cursor) out.push_back(box_line(s, s.list_cursor + txt + kReset, s.border));
        else out.push_back(box_line(s, base + txt + kReset, s.border));
    }
    for (; shown < body; ++shown) out.push_back(box_line(s, spaces(inner), s.border));
    out.push_back(box_bottom(s, W, info, s.border_bottom));

    std::string hint;
    if (hm.tab == 0) hint = "[←→/TAB] Tab  [1/2/3] Tab  [↑↓] Move  [HOME/END] Ends  [y] Find on YouTube + download  [ESC] Exit";
    else if (hm.tab == 1) hint = "[←→/TAB] Tab  [1/2/3] Tab  [↑↓] Move  [r] Flip sort  [ESC] Exit";
    else hint = "[←→/TAB] Tab  [1/2/3] Tab  [↑↓] Scroll  [ESC] Exit";
    out.push_back(seg_line(legend_segs(s.legend, s.legend_cmd, hint), W));
    out.push_back(seg_line({{"\x1b[38;5;246m", utf8_take(hm.flash, W)}}, W));
    while (static_cast<int>(out.size()) < rows) out.push_back(spaces(W));
    out.resize(static_cast<size_t>(rows));

    // --- the YouTube overlay -------------------------------------------------------------------
    if (hm.yt_open) {
        const int ow = std::min(kYtOverlayW, W);
        const int oi = ow - 4;
        std::vector<std::string> panel;
        panel.push_back(box_top(s, ow, "Find on YouTube + download", s.border));
        panel.push_back(box_line(s, field_row(s.search_idle, "Search: ", hm.yt_text, hm.yt_edit, hm.yt_in_field, oi, "", s.legend, false, s.search), s.border));
        std::string status;
        if (hm.yt.searching) status = "searching...";
        else if (!hm.yt.error.empty()) status = hm.yt.error;
        else if (hm.yt.searched) status = std::to_string(hm.yt.items.size()) + " results";
        else status = "[ENTER] searches";
        panel.push_back(box_line(s, pad_right(truncate_str(status, oi), oi), s.border));
        const int n = static_cast<int>(hm.yt.items.size());
        const int cur = std::clamp(hm.yt_cursor, 0, std::max(0, n - 1));
        const int first = follow_scroll(scroll_id(9), cur, kYtResultRows, n);
        for (int i = 0; i < kYtResultRows; ++i) {
            const int idx = first + i;
            if (idx >= n) { panel.push_back(box_line(s, spaces(oi), s.border)); continue; }
            const YtItem& it = hm.yt.items[static_cast<size_t>(idx)];
            const char* mark = it.state == 1 ? "[..]" : it.state == 2 ? "[ok]" : it.state == 3 ? "[!!]" : "[  ]";
            const std::string len = fmt_clock_len(it.duration_sec);
            const int title_w = std::max(8, oi - 5 - 1 - 8);
            std::string label = it.title + (it.uploader.empty() ? "" : "  (" + it.uploader + ")");
            std::string txt = std::string(mark) + " " + T(label, title_w) + " " + R(len, 8);
            const bool on = !hm.yt_in_field && idx == cur;
            panel.push_back(box_line(s, (on ? s.list_cursor : base) + pad_right(truncate_str(txt, oi), oi) + kReset, s.border));
        }
        std::string note;
        if (n > 0 && cur < n) {
            const YtItem& it = hm.yt.items[static_cast<size_t>(cur)];
            if (it.state == 2) note = "saved: " + it.note;
            else if (it.state == 3) note = "failed: " + it.note;
            else if (it.state == 1) note = "downloading...";
        }
        if (note.empty()) note = "to: " + hm.yt_dir;
        panel.push_back(box_line(s, s.legend + pad_right(truncate_str(note, oi), oi) + kReset, s.border));
        panel.push_back(box_bottom(s, ow, hm.yt_in_field ? "[ENTER] search  [↓/TAB] results  [ESC] close"
                                                           : "[ENTER] download  [↑↓] select  [TAB] edit  [ESC] close", s.border_bottom));
        const int ph = static_cast<int>(panel.size());
        const int x = std::max(0, (W - ow) / 2);
        const int y = std::clamp((rows - ph) / 2, 0, std::max(0, rows - ph));
        for (int i = 0; i < ph; ++i)
            out[static_cast<size_t>(y + i)] = splice_line(out[static_cast<size_t>(y + i)], x, panel[static_cast<size_t>(i)], ow);
    }
    return out;
}

// ===========================================================================================
// RADIO SETTINGS (key `s`) -- same look as the music player's settings screen: square frame in the gradient colours,
// the tab strip ("[GENERAL]" = current tab), one row per setting. COLORS: colour cells with a live preview on the
// right; GENERAL: toggles.
// ===========================================================================================
constexpr const char* kSettingsTabs[6] = {"COLORS", "GENERAL", "ANIMA/VIZ", "PATHS", "REFERENCE", "ABOUT APP"};

// The theme picker (also used to choose a template inside the creator): one theme per line with colour swatches.
std::vector<std::string> build_theme_picker(const RadioSettings& c, const SettingsModel& sm, int W, int rows) {
    const int inner = W - 4;
    const int H = std::max(rows, kUiRows);
    auto bar = [&](int y) {
        if (!c.border_smooth) return ansi_fg(y >= H - 3 ? c.border_bottom : c.border_top);   // rough: only the bottom border line changes
        const float t = H > 1 ? static_cast<float>(y) / static_cast<float>(H - 1) : 0.0f;
        return gradient_fg(c.border_top, c.border_bottom, std::round(t * 32.0f) / 32.0f);
    };
    const std::string legend = ansi_fg(c.legend), header = ansi_fg(c.header);
    std::vector<std::string> out;
    const std::string title = sm.theme_ui == 1 ? "─ CHOOSE THEME " : "─ CHOOSE THEME AS TEMPLATE ";
    out.push_back(bar(0) + "┌" + title + repeat("─", std::max(0, W - 2 - display_width(title))) + "┐" + kReset);
    const int n = static_cast<int>(sm.theme_list.size());
    const int visible = std::max(1, H - 4);
    const int first = std::clamp(sm.theme_sel - visible / 2, 0, std::max(0, n - visible));
    // the marker: the theme in use (picker), or the one the draft matches / is based on (template picker)
    int cur = sm.theme_ui == 1 ? theme_index(c, sm.theme_list) : -1;
    if (sm.theme_ui == 3) {
        cur = theme_index(sm.theme_draft, sm.theme_list);
        for (int k = 0; k < n && cur < 0; ++k) if (sm.theme_list[static_cast<size_t>(k)].name == sm.theme_base.name) cur = k;
    }
    static const struct { const char* key; bool bg; } kSw[] = {
        {"ColorBorderTop", false}, {"ColorBorderBottom", false}, {"ColorOnAirUpperLeft", false}, {"ColorOnAirBottomRight", false},
        {"ColorMetadataKey", false}, {"ColorMetadataVal", false}, {"ColorVolumeCurrent", false},
        {"ColorListInactiveFg", false}, {"ColorListPlayingFg", false}, {"ColorListCursorBg", true}};
    for (int r = 0; r < visible; ++r) {
        const int y = static_cast<int>(out.size());
        const std::string b = bar(y) + "│" + kReset;
        std::vector<Seg> segs;
        const int i = first + r;
        if (i < n) {
            const themes::Theme& t = sm.theme_list[static_cast<size_t>(i)];
            segs.push_back({header, i == cur ? "● " : "  "});
            // your own themes (not built in) are named in the header colour
            segs.push_back({(t.builtin ? std::string() : header) + (i == sm.theme_sel ? "\x1b[7m" : ""), pad_right(truncate_str(t.name, themes::kMaxNameLen), themes::kMaxNameLen)});
            segs.push_back({"", "  "});
            for (const auto& k : kSw) {
                const std::string* v = t.get(k.key, true);
                const std::string val = v ? *v : "";
                segs.push_back({k.bg ? ansi_bg(val) : ansi_fg(val), k.bg ? "    " : "████"});
                segs.push_back({"", " "});
            }
        }
        // more themes than rows: a scrollbar in the last column (a thumb as tall as the visible part is of the list)
        std::string sb = " ";
        {
        muisc::overlay::Thumb th;
        if (muisc::overlay::scroll_thumb(first, visible, n, th))
            sb = bar(y) + (r >= th.at && r < th.at + th.len ? "\u2588" : "\u2591") + kReset;   // border-gradient colour of that row
        }
        out.push_back(b + " " + seg_line(segs, inner - 2) + " " + sb + " " + b);
    }
    while (static_cast<int>(out.size()) < H - 3) {
        const std::string b = bar(static_cast<int>(out.size())) + "│" + kReset;
        out.push_back(b + " " + spaces(inner) + " " + b);
    }
    out.push_back(bar(H - 3) + "└" + repeat("─", std::max(0, W - 2)) + "┘" + kReset);
    if (sm.leave_prompt == 4 && sm.theme_sel >= 0 && sm.theme_sel < n)
        out.push_back(seg_line({{"\x1b[1;7m", " Do you want to delete theme \"" + truncate_str(sm.theme_list[static_cast<size_t>(sm.theme_sel)].name, themes::kMaxNameLen) + "\"? [y] Yes, delete  [n / ESC] No, keep it"}}, W));
    else
    out.push_back(seg_line(legend_segs(legend, ansi_fg(c.legend_cmd), std::string(sm.theme_ui == 1 ? "[↑↓] Select  [SHIFT+↑↓] Page  [SHIFT+p] Preview  [ENTER] Apply  [SHIFT+d] Delete your own theme  [ESC] Back"
                                                      : "[↑↓] Select  [SHIFT+↑↓] Page  [SHIFT+p] Preview  [ENTER] Use as template  [ESC] Back")), W));
    out.push_back(seg_line({{"\x1b[38;5;246m", utf8_take(sm.status, W)}}, W));
    return out;
}

// SHIFT+e / SHIFT+i overlays: export the current colours as a theme file / import theme files from a folder.
std::vector<std::string> build_theme_io(const RadioSettings& c, const SettingsModel& sm, int W, int rows) {
    const int inner = W - 4;
    const int H = std::max(rows, kUiRows);
    auto bar = [&](int y) {
        if (!c.border_smooth) return ansi_fg(y >= H - 3 ? c.border_bottom : c.border_top);
        const float t = H > 1 ? static_cast<float>(y) / static_cast<float>(H - 1) : 0.0f;
        return gradient_fg(c.border_top, c.border_bottom, std::round(t * 32.0f) / 32.0f);
    };
    const std::string legend = ansi_fg(c.legend), header = ansi_fg(c.header);
    const bool exp = sm.theme_ui == 4;
    std::vector<std::string> out;
    const std::string title = exp ? "─ EXPORT THEME " : "─ IMPORT THEMES ";
    out.push_back(bar(0) + "┌" + title + repeat("─", std::max(0, W - 2 - display_width(title))) + "┐" + kReset);
    auto row_of = [&](const std::vector<Seg>& segs) {
        const std::string b = bar(static_cast<int>(out.size())) + "│" + kReset;
        out.push_back(b + " " + seg_line(segs, inner) + " " + b);
    };
    auto field = [&](const char* label, const std::string& value, bool sel, int width) {
        std::vector<Seg> segs = {{"\x1b[1m" + header, pad_right(label, 15)}, {"", ": "}};
        if (sel && sm.editing) {
            const EditPaint p = paint_edit_field(sm.buffer, sm.edit, width, "\x1b[41;37m", true);
            segs.push_back({"\x1b[41;37m", p.s + spaces(std::max(0, width + 1 - p.cols))});
        } else {
            segs.push_back({sel ? "\x1b[7m" : "", pad_right(utf8_take(value, width), width + 1)});
        }
        row_of(segs);
    };
    row_of({});
    const int fw = std::max(20, W - 24);
    if (exp) {
        field("NAME", sm.theme_io_name, sm.theme_io_row == 0, themes::kMaxNameLen);
        field("FOLDER", sm.theme_io_dir, sm.theme_io_row == 1, fw);
        row_of({});
        row_of({{legend, utf8_take("Exports the colors now set as one theme file: " + (std::filesystem::path(sm.theme_io_dir) / themes::file_name_for(sm.theme_io_name)).string(), inner)}});
        row_of({{legend, "(radio and player colors are both kept in the file; import it with SHIFT+i)"}});
    } else {
        field("FOLDER", sm.theme_io_dir, sm.theme_io_row == 0, fw);
        row_of({});
        const int nf = static_cast<int>(sm.theme_io_files.size());
        row_of({{"\x1b[1m" + header, "Theme files found (" + std::to_string(nf) + ")"}});
        const int visible = std::max(1, H - 3 - static_cast<int>(out.size()));
        const int first = std::clamp(sm.theme_io_sel - visible / 2, 0, std::max(0, nf - visible));
        for (int r = 0; r < visible && first + r < nf; ++r) {
            const int i = first + r;
            const size_t nth = themes::read_theme_file(sm.theme_io_files[static_cast<size_t>(i)]).size();
            const std::string cnt = std::to_string(nth) + (nth == 1 ? " theme" : " themes");
            const bool sel = sm.theme_io_row == 1 && i == sm.theme_io_sel;
            row_of({{sel ? "\x1b[7m" : "", pad_right(utf8_take(sm.theme_io_files[static_cast<size_t>(i)].filename().string(), inner - 12), inner - 12)}, {legend, pad_left(cnt, 12)}});
        }
        if (nf == 0) row_of({{legend, "no theme files (*.theme, *.txt) in this folder"}});
    }
    while (static_cast<int>(out.size()) < H - 3) row_of({});
    out.push_back(bar(H - 3) + "└" + repeat("─", std::max(0, W - 2)) + "┘" + kReset);
    out.push_back(seg_line(legend_segs(legend, ansi_fg(c.legend_cmd), std::string(sm.editing ? "[ENTER] Apply  [ESC] Cancel  [←→] Cursor  [Ctrl+C/X/V] Copy/Cut/Paste"
                                   : exp ? "[↑↓] Field  [ENTER] Edit  [s] Export  [ESC] Cancel"
                                         : "[↑↓] Select  [ENTER] Import the file (or edit the folder)  [a] Import all  [ESC] Cancel")), W));
    out.push_back(seg_line({{"\x1b[38;5;246m", utf8_take(sm.status, W)}}, W));
    return out;
}

std::vector<std::string> build_settings_impl(const RadioSettings& c, const SettingsModel& sm, int W, int rows, const SettingsModel* cr) {
    const int inner = W - 4;
    const int H = std::max(rows, kUiRows);
    auto bar = [&](int y) {   // frame colour of line y: fades from border_top to border_bottom
        if (!c.border_smooth) return ansi_fg(y >= H - 3 ? c.border_bottom : c.border_top);   // rough: only the bottom border line changes
        const float t = H > 1 ? static_cast<float>(y) / static_cast<float>(H - 1) : 0.0f;
        return gradient_fg(c.border_top, c.border_bottom, std::round(t * 32.0f) / 32.0f);
    };
    const std::string legend = ansi_fg(c.legend), header = ansi_fg(c.header);
    const std::string tab_cur = ansi_fg(c.tab_current), tab_oth = ansi_fg(c.tab_other);
    const std::string V = "│", Hz = "─";
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(H));

    // --- tab strip (2 lines):  ┌─ SETTINGS ─────────┐  COLORS  ┌──┐  [GENERAL]  ┌─────...─┐
    //                           │                    └──────────┘  └────────────┘       ... │
    if (cr) {   // CREATE THEME: title, the NAME / TEMPLATE row, a divider; the COLORS rows of the draft follow
        const std::string title = "─ CREATE THEME ";
        out.push_back(bar(0) + "┌" + title + repeat(Hz, std::max(0, W - 2 - display_width(title))) + "┐" + kReset);
        const std::string b = bar(1) + V + kReset;
        std::vector<Seg> segs;
        segs.push_back({"\x1b[1m" + header, pad_right("NAME", 15)});
        segs.push_back({"", ": "});
        const bool on_name = cr->theme_row < 0 && cr->theme_col == 0;
        const bool on_tpl = cr->theme_row < 0 && cr->theme_col == 1;
        if (on_name && cr->editing) {
            const EditPaint p = paint_edit_field(cr->buffer, cr->edit, themes::kMaxNameLen, "\x1b[41;37m", true);
            segs.push_back({"\x1b[41;37m", p.s + spaces(std::max(0, themes::kMaxNameLen + 1 - p.cols))});
        } else {
            const bool empty = cr->theme_name.empty();
            segs.push_back({on_name ? "\x1b[7m" : (empty ? legend : ""),
                            pad_right(empty ? std::string("(type a name)") : cr->theme_name, themes::kMaxNameLen + 1)});
        }
        segs.push_back({"", "   "});
        segs.push_back({on_tpl ? "\x1b[7m" : "", " CHOOSE THEME AS TEMPLATE "});
        segs.push_back({legend, "  based on " + cr->theme_base.name});
        out.push_back(b + " " + seg_line(segs, inner) + " " + b);
        out.push_back(bar(2) + "├" + repeat(Hz, std::max(0, W - 2)) + "┤" + kReset);
    } else {
        std::vector<Seg> top, bot;
        const std::string b0 = bar(0), b1 = bar(1);
        top.push_back({b0, "┌─ SETTINGS " + repeat(Hz, 9) + "┐"});
        bot.push_back({b1, V}); bot.push_back({"", spaces(20)}); bot.push_back({b1, "└"});
        int used = 22;
        for (int i = 0; i < 6; ++i) {
            const bool cur = i == sm.tab;
            const std::string label = cur ? std::string("[") + kSettingsTabs[i] + "]" : std::string(kSettingsTabs[i]);
            const int tw = display_width(label) + 4;
            top.push_back({"", "  "}); top.push_back({cur ? tab_cur : tab_oth, label}); top.push_back({"", "  "});
            bot.push_back({b1, repeat(Hz, tw)});
            used += tw;
            if (i < 5) {
                top.push_back({b0, "┌──┐"});
                bot.push_back({b1, "┘  └"});
                used += 4;
            } else {
                const int rest = std::max(0, W - used - 2);
                top.push_back({b0, "┌" + repeat(Hz, rest) + "┐"});
                bot.push_back({b1, "┘" + spaces(rest) + V});
            }
        }
        out.push_back(seg_line(top, W));
        out.push_back(seg_line(bot, W));
    }
    auto line = [&](const std::vector<Seg>& segs) {
        const int y = static_cast<int>(out.size());
        const std::string b = bar(y) + V + kReset;
        out.push_back(b + " " + seg_line(segs, inner) + " " + b);
    };

    if (sm.tab == 1 || sm.tab == 2) {
        // ---- GENERAL and ANIMA/VIZ: rows under category headers (like the player's tabs); scrolls with the cursor
        struct DLine { std::vector<Seg> segs; int row = -1; };
        std::vector<DLine> dl;
        int rr = 0;
        rs_set_hifi_rows(c.bars_style == 1);
        for (const SettingsLine& l : rs_layout(sm.tab)) {
            if (l.header) {   // the layout carries its own blank lines (empty headers); the tab starts with one, like the other tabs
                if (rr == 0 && dl.empty()) dl.push_back({{}, -1});
                dl.push_back({l.text[0] ? std::vector<Seg>{{"\x1b[1m" + header, std::string("   ") + l.text}} : std::vector<Seg>{}, -1});
                continue;
            }
            const bool sel = rr == sm.row;
            std::string v = rs_row_value(c, sm.tab, rr);
            // the "disk" look of the Volume indicator is shown as two braille blocks of the animation itself
            if ((sm.tab == 1 || sm.tab == 2) && v == muisc::kVolIndicatorNames[muisc::kVolStyleDisk]) v = muisc::volume_disk_snippet(muisc::volume_clock_angle());
            std::vector<Seg> segs = {{"", "   " + pad_right(l.text, 26) + ":  "}, {sel ? "\x1b[7m" : "", v}};
            if ((sm.tab == 1 || sm.tab == 2) && std::string(l.text) == "Volume indicator") {   // two values: the look | gradient or not (LEFT / RIGHT switch, ENTER changes)
                segs = {{"", "   " + pad_right(l.text, 26) + ":  "}, {sel && sm.col == 0 ? "\x1b[7m" : "", v},
                        {"", spaces(std::max(0, 10 - display_width(v)))},
                        {sel && sm.col == 1 ? "\x1b[7m" : "", c.volume_gradient ? "gradient   " : "no gradient"}};
                if (sel) segs.push_back({tab_cur, "   [ENTER] change"});
            } else
            if (sel) segs.push_back({tab_cur, spaces(std::max(0, 20 - display_width(v))) + "< \u2194 >"});
            dl.push_back({segs, rr});
            ++rr;
        }
        const int avail = std::max(1, (H - 3) - static_cast<int>(out.size()));
        const int total = static_cast<int>(dl.size());
        int cur = 0;
        for (int i = 0; i < total; ++i) if (dl[static_cast<size_t>(i)].row == sm.row) cur = i;
        int first = std::clamp(cur - avail / 2, 0, std::max(0, total - avail));
        if (first <= 2 && cur < avail) first = 0;   // near the top: keep the first category header visible (as long as the cursor row still fits)
        else if (first > 0 && dl[static_cast<size_t>(first)].row >= 0 && dl[static_cast<size_t>(first) - 1].row < 0) first = std::max(0, first - 1);
        for (int i = first; i < total && i < first + avail; ++i) {
            std::vector<Seg> segs = dl[static_cast<size_t>(i)].segs;
            line(segs);
        }
        while (static_cast<int>(out.size()) < H - 3) line({});
    } else if (sm.tab == 4) {
        // ---- REFERENCE: the note, then the rebindable keys under category headers; scrolls with the cursor
        struct DLine { std::vector<Seg> segs; int action = -1; };
        std::vector<DLine> dl;
        dl.push_back({{}, -1});
        dl.push_back({{{legend, "   SEE CHEAT SHEET FOR FULL LIST OF COMMANDS, `?`"}}, -1});
        {   // selectable row 0: every key back to its default (Ctrl+Shift+U takes it back), right under the note
            const bool sel = sm.row == 0;
            dl.push_back({{{"", "   " + pad_right("Reset All Keys To Default", 26) + ":  "},
                           {sel ? "\x1b[7m" : "", pad_right("[ENTER] reset", 20)}}, 0});
        }
        for (int i = 0; i < kKeyActionCount; ++i) {
            if (kKeyActions[i].header) {
                dl.push_back({{}, -1});
                dl.push_back({{{"\x1b[1m" + header, std::string("   ") + kKeyActions[i].header}}, -1});
            }
            const bool sel = i + 1 == sm.row;
            std::vector<Seg> segs = {{"", "   " + pad_right(kKeyActions[i].label, 26) + ":  "}};
            if (sel && sm.editing) {
                const EditPaint p = paint_edit_field(sm.buffer, sm.edit, 19, "\x1b[41;37m", true);
                segs.push_back({"\x1b[41;37m", p.s + spaces(std::max(0, 20 - p.cols))});
            } else {
                segs.push_back({sel ? "\x1b[7m" : "", pad_right(pretty_key(key_binding(c, i)), 20)});
            }
            dl.push_back({segs, i + 1});
        }
        const int avail = std::max(1, (H - 3) - static_cast<int>(out.size()));
        int cur = 0;
        for (size_t i = 0; i < dl.size(); ++i) if (dl[i].action == sm.row) cur = static_cast<int>(i);
        const int total = static_cast<int>(dl.size());
        const int first = std::clamp(cur - avail / 2, 0, std::max(0, total - avail));
        for (int i = first; i < total && i < first + avail; ++i) line(dl[static_cast<size_t>(i)].segs);
    } else if (sm.tab == 5) {
        // ---- ABOUT APP: the music player's text, Up / Down scroll
        const int avail = std::max(1, (H - 3) - static_cast<int>(out.size()) - 1);
        line({});
        // centred like the music player's ABOUT tab: each line centred, the block sits vertically in the middle of the rows,
        // moved two rows up (a longer text scrolls and then starts at the top)
        const int total = static_cast<int>(about_app_text().size());
        int bw = 0;
        for (const std::string& s : about_app_text()) bw = std::max(bw, display_width(s));
        const int pad = std::max(0, (inner - bw) / 2);
        const int top = total >= avail ? 0 : std::max(0, (avail - total) / 2 - 2);
        const int first = total > avail ? std::clamp(sm.row, 0, total - avail) : 0;
        for (int r = 0; r < avail; ++r) {
            const int i = first + r - top;
            if (r < top || i < 0 || i >= total) line({});
            else line({{"", spaces(pad) + about_app_text()[static_cast<size_t>(i)]}});
        }
    } else if (sm.tab == 3) {
        // ---- PATHS: the folder fields, the "same as music player" toggle and the history folder (like the player's PATHS tab:
        // names in the header colour, values in the colour of inactive list rows, an empty field shows the default folder)
        line({});
        const StoragePaths sp = resolve_storage_paths(c);
        const std::string list_ink = ansi_fg(c.list_fg) + ansi_bg(c.list_bg);
        constexpr int kFieldW = 60;
        for (int r = 0; r < kPathRowCount; ++r) {
            const bool sel = r == sm.row;
            std::vector<Seg> segs = {{header, "   " + pad_right(kPathRows[r], 28)}, {"", ":  "}};
            if (path_row_is_bool(r)) {
                const std::string v = c.download_same_as_player ? "ON" : "OFF";
                segs.push_back({sel ? "\x1b[7m" : list_ink, v});
                if (sel) segs.push_back({tab_cur, spaces(std::max(0, 20 - display_width(v))) + "< \u2194 >"});
            } else {
                const std::string& val = *path_row_text(c, r);
                const bool unused = r == 2 && c.download_same_as_player;
                if (sel && sm.editing) {
                    const EditPaint p = paint_edit_field(sm.buffer, sm.edit, kFieldW - 1, "\x1b[41;37m", true);
                    segs.push_back({"\x1b[41;37m", p.s + spaces(std::max(0, kFieldW - p.cols))});
                } else if (val.empty() || unused) {
                    // the folder in effect: the base the radio's own sub-folders are created in
                    std::filesystem::path def;
                    if (r == 0) def = sp.stations_file.parent_path();
                    else if (r == 1) def = sp.presets_file.parent_path();
                    else if (r == 2) def = sp.download_dir.parent_path();
                    else if (r == 5) def.clear();   // no audio book folder yet
                    else def = sp.history_dir.parent_path();
                    const std::string shown = unused ? std::string("SAME PATH AS THE MUSIC PLAYER IS USED") : r == 5 ? std::string("NOT SET (audio books are off until a folder is given)") : def.string();
                    segs.push_back({sel ? "\x1b[7m" : list_ink, pad_right(utf8_take(shown, kFieldW), kFieldW)});
                } else {
                    segs.push_back({sel ? "\x1b[7m" : list_ink, pad_right(utf8_take(val, kFieldW), kFieldW)});
                }
            }
            line(segs);
            if (r != 2) line({});   // DOWNLOAD PATH and "Same folder as music player" belong together
        }
        line({{legend, "   Audio books: every folder in it with audio files is one book. A .cue file beside each book"}});
        line({{legend, "   records where you last stopped listening."}});
        line({{legend, "   When folder paths are changed, the respective files are only copied, never moved, and no deletions are performed."}});
    } else {
        // ---- COLORS
        const int crow = cr ? sm.row : color_row_of_tab(sm.row);   // the colour row the cursor is on (-1: THEME / SMOOTHNESS)
        auto cellv = [&](int r, int col) -> std::string {
            const std::string* f = color_field(c, r, col);
            if (!f) return "";
            return (sm.editing && crow == r && sm.col == col) ? sm.buffer : *f;
        };
        if (!cr) {   // THEME | CREATE THEME, on top
            const int ci = theme_index(c, sm.theme_list);
            const std::string tn = ci >= 0 ? sm.theme_list[static_cast<size_t>(ci)].name : std::string("(custom)");
            const bool on = sm.row == 0;
            line({{"\x1b[1m" + header, pad_right("THEME", 15)}, {"", pad_right("SELECT", 14) + ": "},
                  {on && sm.col == 0 ? "\x1b[7m" : "", pad_right(truncate_str(tn, themes::kMaxNameLen), themes::kMaxNameLen + 1)}, {"", "   "},
                  {on && sm.col == 1 ? "\x1b[7m" : "", " CREATE THEME "}});
            const std::string b = bar(static_cast<int>(out.size()));
            line({{b, repeat(Hz, inner)}});
        }
        auto preview = [&](int r) -> std::vector<Seg> {
            const ColorRowSpec& spec = kColorRows[r];
            const auto is = [&](std::string RadioSettings::* f) { return spec.field1 == f; };
            const std::string v1 = cellv(r, 0), v2 = cellv(r, 1);
            auto grad = [&](const std::string& a, const std::string& b) {
                std::vector<Seg> g;
                for (int i = 0; i < 26; ++i) g.push_back({gradient_fg(a, b, static_cast<float>(i) / 25.0f), "█"});
                return g;
            };
            if (is(&RadioSettings::border_top)) return grad(v1, v2);
            if (is(&RadioSettings::on_air_upper_left)) return grad(v1, v2);
            if (is(&RadioSettings::viz_left)) return grad(v1, v2);
            if (is(&RadioSettings::osci_left)) return grad(v1, v2);
            if (is(&RadioSettings::meta_key))
                return {{ansi_fg(v1.empty() ? c.list_fg : v1), "Station"}, {"", " : "}, {ansi_fg(v2.empty() ? c.list_fg : v2), "Deutschlandfunk"}};
            if (is(&RadioSettings::freq_line))
                return {{ansi_fg(v1), "──╦──┬──╦── "}, {ansi_fg(v2), "92  ·  96"}};
            if (is(&RadioSettings::volume_current))
                return {{"", "VOLUME BAR:["}, {ansi_fg(v1), muisc::volume_bar_fill(c.volume_indicator, 9, muisc::volume_clock_angle())}, {ansi_fg(v2), "-----------"}, {"", "]"}};
            auto styled = [&](const std::string& fg, const std::string& bg, bool cursor) {
                std::string a = ansi_fg(fg) + ansi_bg(bg);
                if (cursor && a.empty()) a = "\x1b[7m";
                return a;
            };
            if (is(&RadioSettings::search_fg))
                return {{ansi_fg(c.search_inactive_fg.empty() ? c.list_fg : c.search_inactive_fg) + ansi_bg(c.search_inactive_bg), "/s: "},
                        {ansi_fg(v1.empty() ? c.list_fg : v1) + ansi_bg(v2.empty() ? c.list_bg : v2), "QUERY"}};
            if (is(&RadioSettings::search_inactive_fg))
                return {{ansi_fg(v1.empty() ? c.list_fg : v1) + ansi_bg(v2), "/s:               "}};
            if (is(&RadioSettings::list_fg)) return {{styled(v1, v2, false), "3 | Station name      | Pop "}};
            if (is(&RadioSettings::list_playing_fg)) return {{styled(v1, v2, false), "3 | Station (tuned)    | Pop "}};
            if (is(&RadioSettings::list_cursor_fg)) return {{styled(v1, v2, true), "3 | Station (hovered)  | Pop "}};
            if (is(&RadioSettings::preset_inactive_fg)) return {{styled(v1, v2, false), "5 -"}};
            if (is(&RadioSettings::preset_key_fg)) return {{ansi_fg(v1), "1"}, {ansi_fg(c.list_fg) + ansi_bg(c.list_bg), " Station name"}};
            if (is(&RadioSettings::header)) return {{ansi_fg(v1), "tuning ...  buffering 40%"}};
            if (is(&RadioSettings::legend)) return {{ansi_fg(v1), "[TAB]"}, {ansi_fg(v2), " Pane  "}, {ansi_fg(v1), "[ENTER]"}, {ansi_fg(v2), " Tune"}};
            if (is(&RadioSettings::tab_current)) return {{ansi_fg(v1), "[COLORS]"}, {"", "  "}, {ansi_fg(v2), "GENERAL  ANIMA/VIZ"}};
            if (is(&RadioSettings::focus)) return {{ansi_fg(c.list_fg), "STATIONS "}, {ansi_fg(v1), "\u25C0"}};
            return {};
        };
        for (int r = 0; r < kColorRowCount; ++r) {
            if (r == kColorRowsAboveDivider) {
                const std::string b = bar(static_cast<int>(out.size()));
                line({{b, repeat(Hz, inner)}});
            }
            const ColorRowSpec& spec = kColorRows[r];
            std::vector<Seg> segs;
            segs.push_back({"\x1b[1m" + header, pad_right(spec.group, 15)});
            auto emit_cell = [&](int col, const std::string& label, int label_w, bool right_align) {
                segs.push_back({"", right_align ? pad_left(label, label_w) : pad_right(label, label_w)});
                segs.push_back({"", ": "});
                const std::string v = cellv(r, col);
                const bool sel = crow == r && sm.col == col;
                if (sel && sm.editing) {   // caret and marked range, like the path and key fields
                    const EditPaint p = paint_edit_field(sm.buffer, sm.edit, 4, "\x1b[41;37m", true);
                    segs.push_back({"\x1b[41;37m", p.s + spaces(std::max(0, 5 - p.cols))});
                }
                else
                    segs.push_back({sel ? "\x1b[7m" : "", pad_right(v, 5)});
                const bool is_bg = (col == 1) && spec.label2 && std::string(spec.label2) == "BG";
                segs.push_back({"", " "});
                segs.push_back({is_bg ? ansi_bg(v) : ansi_fg(v), is_bg ? "  " : "██"});
            };
            emit_cell(0, spec.label1, 14, false);
            if (spec.label2) {
                segs.push_back({"", "    "});
                emit_cell(1, spec.label2, 13, true);
            } else {
                segs.push_back({"", spaces(4 + 13 + 2 + 5 + 3)});
            }
            segs.push_back({"", "  "});
            for (auto& g : preview(r)) segs.push_back(g);
            line(segs);
            if (!cr && r == 0) {   // SMOOTHNESS: how the frame's colour runs from TOP to BOTTOM
                const bool on = sm.row == 2;
                std::vector<Seg> sg = {{"", pad_right("", 15)}, {"", pad_right("SMOOTHNESS", 14) + ": "},
                                       {on ? "\x1b[7m" : "", pad_right(c.border_smooth ? "smooth" : "rough", 7)}};
                if (on) sg.push_back({tab_cur, "< \u2194 >"});
                sg.push_back({legend, std::string(on ? "  " : "       ") + (c.border_smooth ? "frame fades from TOP to BOTTOM" : "only the bottom border line = BOTTOM")});
                line(sg);
            }
        }
    }
    const int body_end = H - 3;   // the square bottom border, the hint line and the status line follow
    while (static_cast<int>(out.size()) < body_end) line({});
    out.push_back(bar(H - 3) + "└" + repeat(Hz, std::max(0, W - 2)) + "┘" + kReset);
    std::string hint;
    if (sm.tab == 4 && sm.editing) hint = "[ENTER] Apply  [ESC] Cancel  [\u2190\u2192] Cursor  type one key (or SPACE / TAB / BACKSPACE)";
    else if (sm.tab == 4) hint = "[TAB] Switch  [ENTER] Change  [DEL] Default  [Ctrl+Shift+U] Undo  [s] Save settings  [ESC/q] Discard";
    else if (sm.tab == 5) hint = "[TAB] Switch  [\u2191\u2193] Scroll  [ESC/q] Back";
    else if (sm.tab == 3 && sm.editing) hint = "[ENTER] Apply  [ESC] Cancel  [\u2190\u2192] Cursor  type or paste a folder (empty = default)";
    else if (sm.tab == 3) hint = "[TAB] Switch  [ENTER] Edit / toggle  [s] Save settings  [ESC/q] Discard";
    else if (sm.tab >= 1) hint = "[TAB] Switch  [←→/ENTER] Change  [s] Save settings  [ESC/q] Discard";
    else if (cr && cr->editing) hint = cr->theme_row < 0 ? "[ENTER] Apply  [ESC] Cancel  [\u2190\u2192] Cursor  type the theme's name (25 characters at most)"
                                                       : "[ENTER] Apply  [ESC] Cancel  [\u2190\u2192] Cursor  type a palette number 0-255 (empty or 0 = terminal colour)";
    else if (cr) hint = "[ENTER] Edit/Open  [s] Save theme  [ESC] Discard";
    else if (sm.editing) hint = "[ENTER] Apply  [ESC] Cancel  [\u2190\u2192] Cursor  type a palette number 0-255 (empty or 0 = terminal colour)";
    else hint = "[TAB] Switch  [ENTER] Edit  [e] Edit/Save current state  [SHIFT+e/i] Ex-/Import theme  [s] Save settings  [ESC/q] Discard";
    if (sm.leave_prompt == 3)
        hint = " Do you want to overwrite theme \"" + truncate_str(themes::sanitize_name(sm.theme_name), themes::kMaxNameLen) + "\"? [y] Yes, overwrite  [n / ESC] No, keep editing";
    else if (sm.leave_prompt == 2) hint = " Unsaved theme! [s] Save it  [d] Discard it  [ESC] Keep editing";
    else if (sm.leave_prompt == 1) hint = " Unsaved changes! [s] Save and leave  [d] Discard and leave  [ESC] Keep editing";
    out.push_back(seg_line(sm.leave_prompt ? std::vector<Seg>{{"\x1b[1;7m", hint}} : legend_segs(legend, ansi_fg(c.legend_cmd), hint), W));
    out.push_back(seg_line({{"\x1b[38;5;246m", utf8_take(sm.status.empty() && sm.dirty ? std::string("Unsaved changes! Save with `s` or discard with `ESC`.") : sm.status, W)}}, W));
    return out;
}

// CHOOSE AUDIO DEVICE (overlay 9 over the main screen, or over the settings): the sound outputs of this computer, the one in use
// marked; keys in radio_main.cpp. The same rows as the music player's overlay (src/app_audiodevice.cpp).
constexpr int kAdevOverlayW = 64;
std::vector<std::string> build_adev_overlay(const RadioSettings& c, const SettingsModel& sm) {
    const int W = kAdevOverlayW, inner = W - 4;
    const std::string border = ansi_fg(c.border_top), border_bottom = ansi_fg(c.border_bottom);
    const std::string bar = border + c.box_vertical + kReset;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string x = truncate_str(plain, inner);
        const std::string body = x + spaces(std::max(0, inner - display_width(x)));
        return bar + " " + (hi ? "\x1b[7m" + body + kReset : body) + " " + bar;
    };
    auto entry = [&](bool cursor, const std::string& name, const std::string& right) {
        const std::string l = cursor ? "> " : "  ";
        const std::string n = truncate_str(name, std::max(4, inner - 2 - display_width(right) - 1));
        const int gap = std::max(1, inner - 2 - display_width(n) - display_width(right));
        return l + n + spaces(gap) + right;
    };
    auto top = [&](const std::string& title) {
        const std::string t = " " + title + " ";
        return border + c.box_upper_left + c.box_horizontal + t + repeat(c.box_horizontal, std::max(0, W - 3 - display_width(t))) + c.box_upper_right + kReset;
    };
    auto bottom = [&](const std::string& text) {
        const std::string t = " " + truncate_str(text, W - 8) + " ";
        const int dashes = std::max(0, W - 2 - display_width(t));
        return border_bottom + c.box_lower_left + repeat(c.box_horizontal, dashes / 2) + t + repeat(c.box_horizontal, dashes - dashes / 2) + c.box_lower_right + kReset;
    };
    std::string def_name;
    for (const auto& d : sm.adev_list) if (d.is_default) def_name = d.name;
    std::vector<std::string> lines;
    lines.push_back(top("Choose Audio Device"));
    lines.push_back(row("", false));
    lines.push_back(row(entry(sm.adev_sel == 0, "System Default" + (def_name.empty() ? std::string() : " (" + def_name + ")"), c.audio_device.empty() ? "in use" : ""), sm.adev_sel == 0));
    bool there = false;
    for (size_t i = 0; i < sm.adev_list.size(); ++i) {
        const int r = static_cast<int>(i) + 1;
        const bool chosen = !c.audio_device.empty() && c.audio_device == sm.adev_list[i].name;
        if (chosen) there = true;
        std::string mark = chosen ? "in use" : "";
        if (chosen && !sm.adev_active.empty() && sm.adev_active != sm.adev_list[i].name) mark = "not opened";
        lines.push_back(row(entry(sm.adev_sel == r, sm.adev_list[i].name, mark), sm.adev_sel == r));
    }
    if (!c.audio_device.empty() && !there) lines.push_back(row("  " + truncate_str(c.audio_device, inner - 18) + " (not found)", false));
    if (!sm.adev_err.empty()) lines.push_back(row("  " + sm.adev_err, false));
    lines.push_back(row("", false));
    for (const std::string& l : wrap_lines("The choice is the same for the music player, the radio and the audio books. The stream moves to the new device at once.", inner, 3))
        lines.push_back(row(l, false));
    lines.push_back(row("", false));
    lines.push_back(bottom("[UP/DOWN] [ENTER] use  [r] look again  [ESC] close"));
    return lines;
}

// SPECTRUM BARS OPTIONS (overlay 10, SHIFT+b): the rows of the music player's overlay (bars_rows.h). It sits under the upper pane
// (so the bars stay in view) and gets a scroll bar when the terminal is too low for all rows.
constexpr int kBarsOverlayW = 52;
std::vector<std::string> build_bars_overlay(const RadioSettings& c, const UiModel& m) {
    const int W = kBarsOverlayW, inner = W - 4;
    const std::string border = ansi_fg(c.border_top), border_bottom = ansi_fg(c.border_bottom);
    const std::vector<int> rows = muisc::bars_rows(c);
    const int n = static_cast<int>(rows.size());
    const int at = std::clamp(m.overlay_row, 0, n - 1);
    int scroll = m.bars_scroll;
    const int room = m.rows - (kPanelH + 2) - 5;   // the frame: top, blank, blank, hint, bottom
    const muisc::BarsWindow w = muisc::bars_window(n, at, scroll, room);
    const_cast<UiModel&>(m).bars_scroll = scroll;
    std::vector<std::string> lines;
    const std::string t = " Spectrum Bars ";
    lines.push_back(border + c.box_upper_left + c.box_horizontal + t + repeat(c.box_horizontal, std::max(0, W - 3 - display_width(t))) + c.box_upper_right + kReset);
    auto frame = [&](const std::string& body, const std::string& right) { return border + c.box_vertical + kReset + " " + body + " " + right; };
    const std::string bar = border + c.box_vertical + kReset;
    lines.push_back(frame(spaces(inner), bar));
    for (int i = 0; i < w.cap; ++i) {
        const int ri = w.first + i;
        const std::string label = muisc::bars_row_label(rows[static_cast<size_t>(ri)]), val = muisc::bars_value(c, rows[static_cast<size_t>(ri)]);
        const std::string l = std::string(ri == at ? "> " : "  ") + label;
        const int gap = std::max(1, inner - display_width(l) - display_width(val));
        const std::string body = truncate_str(l + spaces(gap) + val, inner);
        const std::string padded = body + spaces(std::max(0, inner - display_width(body)));
        const bool thumb = w.scrolls && i >= w.thumb_at && i < w.thumb_at + w.thumb_len;
        lines.push_back(frame(ri == at ? "\x1b[7m" + padded + kReset : padded, thumb ? border + "\xe2\x94\x83" + kReset : bar));
    }
    lines.push_back(frame(spaces(inner), bar));
    const std::string hint = truncate_str("[UP/DOWN] row  [LEFT/RIGHT] change  [r] reset", inner);
    lines.push_back(frame(hint + spaces(std::max(0, inner - display_width(hint))), bar));
    const std::string b = " [B / ESC] close ";
    const int dashes = std::max(0, W - 2 - display_width(b));
    lines.push_back(border_bottom + c.box_lower_left + repeat(c.box_horizontal, dashes / 2) + b + repeat(c.box_horizontal, dashes - dashes / 2) + c.box_lower_right + kReset);
    return lines;
}

std::vector<std::string> build_settings(const RadioSettings& c, const SettingsModel& sm, int W, int rows) {
    if (sm.theme_ui == 4 || sm.theme_ui == 5) return build_theme_io(c, sm, W, rows);
    if (sm.theme_ui == 1 || sm.theme_ui == 3) return build_theme_picker(c, sm, W, rows);
    if (sm.theme_ui == 2) {   // the COLORS rows of the draft, in the draft's own colours
        SettingsModel tm = sm;
        tm.theme_ui = 0; tm.tab = 0;
        tm.row = sm.theme_row; tm.col = sm.theme_col;
        tm.editing = sm.editing && sm.theme_row >= 0;
        return build_settings_impl(sm.theme_draft, tm, W, rows, &sm);
    }
    std::vector<std::string> out = build_settings_impl(c, sm, W, rows, nullptr);
    if (sm.adev_open && sm.adev_from_settings) {   // GENERAL -> AUDIO -> Choose Audio Device, centred over the settings
        const std::vector<std::string> panel = build_adev_overlay(c, sm);
        const int H = static_cast<int>(out.size());
        const int x = std::max(0, (W - kAdevOverlayW) / 2);
        const int y = std::clamp((H - static_cast<int>(panel.size())) / 2, 0, std::max(0, H - static_cast<int>(panel.size())));
        for (size_t i = 0; i < panel.size() && y + static_cast<int>(i) < H; ++i)
            out[static_cast<size_t>(y) + i] = splice_line(out[static_cast<size_t>(y) + i], x, panel[i], kAdevOverlayW);
    }
    // the overlays over the settings (REMOTE APP, FONT): the radio's colours and box glyphs
    muisc::remote_ui::Style st;
    st.border = ansi_fg(c.border_top);
    st.border_bottom = ansi_fg(c.border_bottom);
    st.header = "\x1b[1m" + ansi_fg(c.header);   // captions, like the labels of the settings rows
    st.legend = ansi_fg(c.legend);
    st.text = ansi_fg(c.list_fg);
    st.value = st.text;
    st.change = "\x1b[41;37m";
    st.cursor = "\x1b[7m";
    st.ul = c.box_upper_left; st.ur = c.box_upper_right; st.ll = c.box_lower_left; st.lr = c.box_lower_right;
    st.h = c.box_horizontal;  st.v = c.box_vertical;
    if (c.border_smooth) st.border_at = [&c](float t) { return gradient_fg(c.border_top, c.border_bottom, t); };
    if (sm.remote_ov.open) {   // GENERAL -> REMOTE / UPDATES -> Remote App: the REMOTE APP overlay, centred over the settings
        const muisc::remote_ui::Values v{c.remote_app, c.remote_port, c.remote_pin};
        int w = 0;
        const int H = static_cast<int>(out.size());
        const std::vector<std::string> panel = muisc::remote_ui::build(sm.remote_ov, v, st, W - 2, H, w);
        const int x = std::max(0, (W - w) / 2);
        const int y = std::clamp((H - static_cast<int>(panel.size())) / 2, 0, std::max(0, H - static_cast<int>(panel.size())));
        for (size_t i = 0; i < panel.size() && y + static_cast<int>(i) < H; ++i)
            out[static_cast<size_t>(y) + i] = splice_line(out[static_cast<size_t>(y) + i], x, panel[i], w);
    }
    return out;
}

// ===========================================================================================
// Overlays over the main screen: SHIFT+o oscilloscope tuning, SHIFT+v loudness normalisation (same rows, ranges and
// keys as the music player's).
// ===========================================================================================
constexpr int kOsciOverlayW = 50;
constexpr int kNormOverlayW = 54;
constexpr int kSleepOverlayW = 52;
constexpr int kMenusOverlayW = 100;
constexpr int kUpdateOverlayW = 56;
constexpr int kRecordOverlayW = 56;
constexpr int kEqOverlayW = 58;
constexpr int kSpectroOverlayW = 52;
constexpr int kSleepMinutes[5] = {15, 30, 60, 90, 120};

std::string fmt_loudness(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", v);
    return buf;
}

} // namespace (reopened below)

const MenusEntry kMenusEntries[] = {
    {"STATIONS & PRESETS", "Presets menu",             "Presets"},
    {"STATIONS & PRESETS", "Station lists",            "Lists"},
    {"STATIONS & PRESETS", "Radio browser",            "Browser"},
    {"STATIONS & PRESETS", "Big stations overlay",     "Stations"},
    {"STATIONS & PRESETS", "Listening history",        "History"},
    {"SOUND",              "Equalizer",                "EqMenu"},
    {"SOUND",              "Normalization",            "NormMenu"},
    {"SOUND",              "Sleep timer",              "SleepTimer"},
    {"SOUND",              "Record",                   "Record"},
    {"VISUALS",            "Oscilloscope options",     "OsciMenu"},
    {"VISUALS",            "Oscilloscope full screen", "ScopeBig"},
    {"VISUALS",            "Spectrogram options",      "SpectroMenu"},
    {"VISUALS",            "Spectrum bars options",    "BarsMenu"},
    {"VISUALS",            "Spectrogram full screen",  "SpectroFull"},
    {"VISUALS",            "Scope window",             "ScopeWindow"},
    {"VISUALS",            "Spectrogram window",       "SpectroWindow"},
    {"SYSTEM",             "Settings",                 "Settings"},
    {"SYSTEM",             "Help wizard",              "Wizard"},
    {"SYSTEM",             "Cheat sheet",              "Cheatsheet"},
};
const int kMenusCount = static_cast<int>(sizeof(kMenusEntries) / sizeof(kMenusEntries[0]));

namespace {
struct MenusCell { int entry = -1; const char* header = nullptr; };

// Two columns of cells (an entry, a header or a blank line), split at the category boundary that balances them.
std::vector<std::vector<MenusCell>> menus_layout() {
    std::vector<std::vector<MenusCell>> cats;
    for (int i = 0; i < kMenusCount; ++i) {
        if (i == 0 || std::string(kMenusEntries[i].cat) != kMenusEntries[i - 1].cat) cats.push_back({MenusCell{-1, kMenusEntries[i].cat}});
        cats.back().push_back(MenusCell{i, nullptr});
    }
    int total = 0;
    for (const auto& c : cats) total += static_cast<int>(c.size()) + 1;
    int best = 1, best_diff = 1 << 20, acc = 0;
    for (size_t k = 0; k + 1 < cats.size(); ++k) {
        acc += static_cast<int>(cats[k].size()) + 1;
        const int diff = std::abs(acc - (total - acc));
        if (diff < best_diff) { best_diff = diff; best = static_cast<int>(k) + 1; }
    }
    std::vector<std::vector<MenusCell>> cols(2);
    for (size_t k = 0; k < cats.size(); ++k) {
        auto& col = cols[static_cast<int>(k) < best ? 0 : 1];
        if (!col.empty()) col.push_back(MenusCell{});
        col.insert(col.end(), cats[k].begin(), cats[k].end());
    }
    return cols;
}

// The bound key of an action as it reads on the keyboard: SHIFT+x for a capital, the symbol keys by layout.
std::string menus_key_label(const RadioSettings& c, const char* id) {
    const int code = key_of(c, id);
    if (code >= 'A' && code <= 'Z') return std::string("SHIFT+") + static_cast<char>(code + 32);
    if (code == ' ') return "SPACE";
    if (code <= 0) return "-";
    return muisc::symbol_key_label(std::string(1, static_cast<char>(code)));
}
} // namespace

int menus_move(int sel, int dir) {
    const auto cols = menus_layout();
    sel = std::clamp(sel, 0, kMenusCount - 1);
    if (dir == 0) return (sel + kMenusCount - 1) % kMenusCount;
    if (dir == 1) return (sel + 1) % kMenusCount;
    int col = 0; size_t row = 0;
    for (int c = 0; c < 2; ++c)
        for (size_t r = 0; r < cols[c].size(); ++r) if (cols[c][r].entry == sel) { col = c; row = r; }
    const int oc = col == 0 ? 1 : 0;
    auto at = [&](size_t r) { return r < cols[oc].size() ? cols[oc][r].entry : -1; };
    for (size_t d = 0; d < std::max(cols[0].size(), cols[1].size()); ++d) {
        if (row >= d && at(row - d) >= 0) return at(row - d);
        if (at(row + d) >= 0) return at(row + d);
    }
    return sel;
}

namespace {

// A bottom border with its text centred in it (the dashes are split evenly on both sides).
std::string box_bottom_centred(const Style& s, int w, const std::string& text, const std::string& ansi) {
    const std::string t = truncate_str(text, w - 8);
    const int dashes = std::max(0, w - 2 - display_width(t) - 2);
    const int left = dashes / 2, right = dashes - left;
    return ansi + s.c.box_lower_left + repeat(s.c.box_horizontal, left) + " " + t + " " + ansi + repeat(s.c.box_horizontal, right) + s.c.box_lower_right + kReset;
}

// The legend of a floating option panel: all rows in the bottom border colour; with several rows the middle one sits ON the bottom
// border line (rows/2 rows inside the panel, the rest hanging below it) so the block is centred on the border.
void push_legend_block(const Style& s, std::vector<std::string>& lines, int W, const std::vector<std::string>& rows) {
    const int inner = W - 4;
    const size_t before = rows.size() / 2;
    auto centred = [&](const std::string& plain) {
        const std::string t = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(t)) / 2);
        return spaces(left) + t + spaces(std::max(0, inner - display_width(t) - left));
    };
    for (size_t i = 0; i < before; ++i)
        lines.push_back(box_line(s, s.border_bottom + centred(rows[i]) + kReset, s.border));
    lines.push_back(box_bottom_centred(s, W, rows[before], s.border_bottom));
    for (size_t i = before + 1; i < rows.size(); ++i)
        lines.push_back(" " + s.border_bottom + " " + centred(rows[i]) + " " + kReset + " ");
}

// overlay 8: "a newer version is out" -- [ESC] closes it, [o] opens the release page (radio_main.cpp)
std::vector<std::string> build_update_overlay(const Style& s, const UiModel& m) {
    const int W = kUpdateOverlayW, inner = W - 4;
    const std::string bar = s.border + s.c.box_vertical + kReset;
    auto row = [&](const std::string& plain) {
        const std::string x = truncate_str(plain, inner);
        return bar + " " + x + spaces(inner - display_width(x)) + " " + bar;
    };
    std::string ver = m.update_latest;
    while (!ver.empty() && (ver[0] == 'v' || ver[0] == 'V')) ver.erase(0, 1);
    std::string url = m.update_url;
    if (url.compare(0, 8, "https://") == 0) url.erase(0, 8);
    std::vector<std::string> lines;
    lines.push_back(box_top(s, W, "New version", s.border));
    lines.push_back(row(""));
    lines.push_back(row("Mousiki " + ver + " is out."));
    lines.push_back(row("You are using " + m.update_current + "."));
    lines.push_back(row(""));
    lines.push_back(row(url));
    lines.push_back(row(""));
    lines.push_back(box_bottom(s, W, "[ESC] Close  [o] Go to release", s.border_bottom));
    return lines;
}

std::vector<std::string> build_overlay(const Style& s, const UiModel& m, const RadioStatus& st) {
    if (m.overlay == 8) return build_update_overlay(s, m);
    if (m.overlay == 9) return build_adev_overlay(s.c, m.settings);
    if (m.overlay == 10) return build_bars_overlay(s.c, m);
    const RadioSettings& c = s.c;
    const bool osci = m.overlay == 1;
    const bool sleep = m.overlay == 3;
    const bool eqo = m.overlay == 4;
    const bool rec = m.overlay == 5;
    const bool spo = m.overlay == 6;
    const bool menus = m.overlay == 7;
    const int W = menus ? kMenusOverlayW : spo ? kSpectroOverlayW : rec ? kRecordOverlayW : eqo ? kEqOverlayW : sleep ? kSleepOverlayW : osci ? kOsciOverlayW : kNormOverlayW;
    const int inner = W - 4;
    auto row = [&](const std::string& plain, bool hi, const std::string& ansi = "") {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return box_line(s, (hi ? "\x1b[7m" : ansi) + body + kReset, s.border);
    };
    std::vector<std::string> lines;
    if (menus) {
        // The MENUS index (<): two columns of categories, each entry with its key; ENTER opens it
        const int cw = (inner - 3) / 2, cw2 = inner - 3 - cw;
        const auto cols = menus_layout();
        const size_t nrows = std::max(cols[0].size(), cols[1].size());
        const int sel = std::clamp(m.overlay_row, 0, kMenusCount - 1);
        // every cell keeps two empty columns on its left and right (the highlight covers them too)
        auto cell = [&](const std::vector<MenusCell>& col, size_t r, int cw_full) {
            const int cw = cw_full - 4;
            if (r >= col.size() || (col[r].entry < 0 && !col[r].header)) return spaces(cw_full);
            const MenusCell& cl = col[r];
            if (cl.header) return "  \x1b[1m" + s.header + pad_right(cl.header, cw) + kReset + "  ";
            const MenusEntry& e = kMenusEntries[cl.entry];
            const std::string key = "[" + menus_key_label(c, e.id) + "]";
            const std::string left = std::string(cl.entry == sel ? "> " : "  ") + e.title;
            const int gap = std::max(1, cw - display_width(left) - display_width(key));
            if (cl.entry == sel) return "\x1b[7m  " + pad_right(truncate_str(left + spaces(gap) + key, cw), cw) + "  " + kReset;
            return "  " + pad_right(left, cw - display_width(key)) + s.legend + key + kReset + "  ";
        };
        lines.push_back(box_top(s, W, "MENUS & OVERLAYS", s.border));
        lines.push_back(box_line(s, spaces(inner), s.border));
        for (size_t r = 0; r < nrows; ++r)
            lines.push_back(box_line(s, cell(cols[0], r, cw) + " " + s.border + "\u2502" + kReset + " " + cell(cols[1], r, cw2), s.border));
        lines.push_back(box_line(s, spaces(inner), s.border));
        lines.push_back(box_bottom(s, W, "[\u2191\u2193\u2190\u2192] Select  [ENTER] Open  [ESC / " + menus_key_label(c, "Menus") + "] Close", s.border_bottom));
        return lines;
    }
    if (spo) {
        // Spectrogram options (SHIFT+i): the same rows as the player's overlay
        lines.push_back(box_top(s, W, "Spectrogram", s.border));
        for (const int r : spectro_visible_rows(c.spectro)) {   // View (spectrogram / waterfall) first; the waterfall has its own rows
            const bool on = r == m.overlay_row;
            std::string val = spectro_row_value(c.spectro, r);
            if (r == kSrStyle && c.spectro.style == 1 && m.gfx_proto == GfxProto::None) val = "image (no Sixel/Kitty here)";
            lines.push_back(row(std::string(on ? "> " : "  ") + pad_right(spectro_row_label(r), 22) + pad_left(val, inner - 24), on));
        }
        lines.push_back(row("", false));
        push_legend_block(s, lines, W, {"[UP/DOWN] select  [LEFT/RIGHT] change", "[R] Audacity defaults  [SHIFT+u] full screen", "[SHIFT+i / ESC] close"});
        return lines;
    }
    if (rec) {
        // Record (y): from the moment the overlay opened, or that much earlier out of the timeshift buffer.
        auto clock = [](double sec) {
            const long long t = std::max(0LL, static_cast<long long>(sec + 0.5));
            char b[24];
            if (t >= 3600) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
            else std::snprintf(b, sizeof b, "%lld:%02lld", t / 60, t % 60);
            return std::string(b);
        };
        lines.push_back(box_top(s, W, "Record", s.border));
        const int n = static_cast<int>(sizeof(kRecordBack) / sizeof(kRecordBack[0]));
        for (int i = 0; i < n; ++i) {
            const int b = kRecordBack[i];
            std::string label, note;
            if (b == 0) { label = "From now on"; note = "(when this opened)"; }
            else if (b < 0) { label = "Everything buffered"; note = clock(m.rec_avail_sec); }
            else {
                label = "The last " + std::to_string(b / 60) + (b == 60 ? " minute" : " minutes");
                if (m.rec_avail_sec + 0.5 < b) note = "only " + clock(m.rec_avail_sec);
            }
            const bool on = i == m.overlay_row;
            const std::string left = std::string(on ? "> " : "  ") + label;
            lines.push_back(row(left + spaces(std::max(1, inner - display_width(left) - display_width(note))) + note, on));
        }
        lines.push_back(row("The recording runs until [y] is pressed again; it is", false, "\x1b[90m"));
        lines.push_back(row("taken from the timeshift buffer (GENERAL tab).", false, "\x1b[90m"));
        lines.push_back(box_bottom(s, W, "[UP/DOWN] [ENTER/y] record  [ESC] cancel", s.border_bottom));
    } else if (eqo) {
        // Same panel as the music player's (build_eq_panel): 13 rows of 2 dB, band values and names, status row, legends.
        const EqUi& u = m.eq;
        constexpr int kCell = 5;
        const std::string R = kReset, HI = "\x1b[7m";
        auto framed = [&](const std::string& content) { return box_line(s, content, s.border); };
        auto centre = [&](const std::string& t) {
            const int w = display_width(t);
            return pad_right(std::string(static_cast<size_t>(std::max(0, (kCell - w) / 2)), ' ') + t, kCell);
        };
        const EqGains& g = c.eq_gains;
        const bool on = c.eq_enabled;
        const int preset = eq_current_preset(c, u);
        char pre[24];
        { const float pa = eq_auto_preamp_db(g); std::snprintf(pre, sizeof pre, pa > -0.05f ? "0.0 dB" : "%+.1f dB", static_cast<double>(pa)); }
        lines.push_back(box_top(s, W, "Equalizer", s.border));
        lines.push_back(row(std::string("Preset: ") + (preset >= 0 ? eq_preset_name(c, preset) : std::string("Custom")) + "   EQ: " + (on ? "ON" : "OFF") + "   Preamp: " + pre, false));
        const int step = 2, half_rows = 12 / step;
        const std::string full = "\u2588\u2588\u2588", low = "\u2584\u2584\u2584", up = "\u2580\u2580\u2580";
        for (int i = -half_rows; i <= half_rows; ++i) {
            const int L = -i * step;
            std::string content;
            if (L == 0 || L == 12 || L == -12 || L == 6 || L == -6) { char a[8]; std::snprintf(a, sizeof a, "%+d", L); content = pad_left(a, 3) + " "; }
            else content = "    ";
            for (int b = 0; b < kEqBands; ++b) {
                const float v = g[static_cast<size_t>(b)];
                const bool sel = b == u.band;
                const std::string colour = sel ? s.header : (on ? std::string() : s.border);
                std::string glyph;
                if (L == 0) { content += (sel ? s.header : s.border) + "\u2500\u2500\u2500\u2500\u2500" + R; continue; }
                else if (L > 0) { if (v >= static_cast<float>(L)) glyph = full; else if (v > static_cast<float>(L - step)) glyph = low; }
                else { if (v <= static_cast<float>(L)) glyph = full; else if (v < static_cast<float>(L + step) && v < 0) glyph = up; }
                if (glyph.empty()) content += "     ";
                else content += " " + colour + glyph + (colour.empty() ? "" : R) + " ";
            }
            lines.push_back(framed(content));
        }
        std::string vals = "    ", names = "    ";
        for (int b = 0; b < kEqBands; ++b) {
            char num[8];
            std::snprintf(num, sizeof num, "%+d", static_cast<int>(std::lround(g[static_cast<size_t>(b)])));
            const std::string v = centre(g[static_cast<size_t>(b)] == 0.0f ? "0" : num);
            const std::string n = centre(kEqLabels[static_cast<size_t>(b)]);
            vals += b == u.band ? HI + v + R : v;
            names += b == u.band ? HI + n + R : n;
        }
        lines.push_back(framed(vals));
        lines.push_back(framed(names));
        if (u.naming) {
            const std::string prefix = "Save as: ";
            const EditPaint p = paint_edit_field(u.name, u.name_edit, std::max(1, inner - display_width(prefix) - 1), "", true);
            lines.push_back(framed(prefix + p.s + std::string(static_cast<size_t>(std::max(0, inner - display_width(prefix) - p.cols)), ' ')));
            push_legend_block(s, lines, W, {"Type a name (max " + std::to_string(kEqNameMaxBytes) + " characters)", "An existing custom name is replaced", "[ENTER] save  [ESC] cancel"});
        } else {
            lines.push_back(row(u.status, false));
            push_legend_block(s, lines, W, {"[</>] band  [UP/DOWN] gain  [,/.] preset  [0] zero", "[S] save preset  [DEL/X] delete preset", "[SPACE] on/off  [R] flat  [SHIFT+e / ESC] close"});
        }
    } else if (sleep) {
        auto clock = [](double sec) {
            const long long t = std::max(0LL, static_cast<long long>(sec));
            char b[24];
            if (t >= 3600) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
            else std::snprintf(b, sizeof b, "%lld:%02lld", t / 60, t % 60);
            return std::string(b);
        };
        auto two = [&](bool cur, const std::string& l, const std::string& r) {
            const std::string left = std::string(cur ? "> " : "  ") + l;
            return left + spaces(std::max(1, inner - display_width(left) - display_width(r))) + r;
        };
        lines.push_back(box_top(s, W, "Sleep Timer", s.border));
        for (int i = 0; i < 5; ++i) {
            const bool running = m.sleep_running == kSleepMinutes[i];
            const double fade = std::clamp(kSleepMinutes[i] * 6.0, 30.0, 600.0);
            lines.push_back(row(two(i == m.overlay_row, std::to_string(kSleepMinutes[i]) + " minutes",
                                    running ? clock(m.sleep_left) + " left" : (c.sleep_fade ? "fade " + clock(fade) : "")), i == m.overlay_row));
        }
        lines.push_back(row(two(m.overlay_row == 5, "Fade out", c.sleep_fade ? "on" : "off"), m.overlay_row == 5));
        lines.push_back(row(two(m.overlay_row == 6, "Off", m.sleep_running == 0 ? "(no timer)" : ""), m.overlay_row == 6));
        lines.push_back(row(c.sleep_fade ? "The volume glides down to 0 over the last part," : "The stream stops when the time is up.", false, "\x1b[90m"));
        lines.push_back(row(c.sleep_fade ? "then the stream stops (fade = 10 % of the time)." : "", false, "\x1b[90m"));
        lines.push_back(box_bottom_centred(s, W, "[UP/DOWN] [ENTER] set  [ESC] close", s.border_bottom));
    } else if (osci) {
        lines.push_back(box_top(s, W, c.osci_style == 1 ? "Oscilloscope - image" : "Oscilloscope - braille", s.border));
        const std::vector<int> ids = osci_visible_rows(c);
        for (int i = 0; i < static_cast<int>(ids.size()); ++i) {
            const bool on = i == m.overlay_row;
            lines.push_back(row(std::string(on ? "> " : "  ") + pad_right(osci_row_label(ids[static_cast<size_t>(i)]), 22)
                                + pad_left(osci_row_value(c, ids[static_cast<size_t>(i)]), 18), on));
        }
        if (c.scope_mode == 2) {
            lines.push_back(row("The scope is switched off (ANIMA/VIZ tab: Osci/sphere)", false, "\x1b[90m"));
            lines.push_back(box_bottom_centred(s, W, "[R] reset  [SHIFT+o / ESC] close", s.border_bottom));
        } else push_legend_block(s, lines, W, {"[UP/DOWN] select   [LEFT/RIGHT] change", "[R] reset  [SHIFT+o / ESC] close"});
    } else {
        const std::string vals[3] = {c.normalize ? "on" : "off", fmt_loudness(c.normalize_target_lufs) + " LUFS",
                                     fmt_loudness(c.normalize_max_boost_db) + " dB"};
        std::string live;
        char num[64];
        if (st.state == StreamState::Idle) live = "No station tuned";
        else if (std::isnan(st.lufs)) live = "Stream: measuring loudness...";
        else if (!c.normalize) { std::snprintf(num, sizeof num, "Stream %.1f LUFS   normalization is off", static_cast<double>(st.lufs)); live = num; }
        else { std::snprintf(num, sizeof num, "Stream %.1f LUFS   applied gain %+.1f dB", static_cast<double>(st.lufs), static_cast<double>(st.norm_gain_db)); live = num; }
        lines.push_back(box_top(s, W, "Loudness normalization", s.border));
        for (int i = 0; i < 3; ++i) {
            const bool on = i == m.overlay_row;
            lines.push_back(row(std::string(on ? "> " : "  ") + pad_right(kNormKnobs[i].label, 18) + pad_left(vals[i], 12), on));
        }
        lines.push_back(row(live, false, "\x1b[90m"));
        push_legend_block(s, lines, W, {"[UP/DOWN] select   [LEFT/RIGHT] change", "[SPACE] on/off  [R] reset  [SHIFT+v / ESC] close"});
    }
    return lines;
}

} // namespace

std::string active_preset_name(const UiModel& m) {
    if (m.banks.empty()) return "Default";
    return m.banks[static_cast<size_t>(std::clamp(m.bank_active, 0, static_cast<int>(m.banks.size()) - 1))].name;
}

int preset_slot_of(const std::vector<int>& presets, int station_index) {
    for (size_t i = 0; i < presets.size(); ++i) if (presets[i] == station_index) return static_cast<int>(i);
    return -1;
}

// The spectrogram's "gradient" colours: the two ends of the OSCI gradient (Settings -> COLORS), like the oscilloscope's.
void spectro_gradient_from(const Style& s) {
    int r0 = 255, g0 = 255, b0 = 255, r1 = 255, g1 = 255, b1 = 255;
    palette_rgb(s, 0, 0.0f, r0, g0, b0);
    palette_rgb(s, 0, 1.0f, r1, g1, b1);
    auto c8 = [](int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); };
    spectro_set_gradient({c8(r0), c8(g0), c8(b0)}, {c8(r1), c8(g1), c8(b1)});
}

// SHIFT+u: the spectrogram over the whole screen: station and song as the title, frequency labels on the left (like
// Audacity's ruler), a time ruler on top; the options overlay (SHIFT+i) can sit on top of it.
std::vector<std::string> build_spectro_full(const Style& s, const UiModel& m, const RadioStatus& st, int W, int rows_total) {
    const RadioSettings& c = s.c;
    const SpectroSettings& sp = c.spectro;
    SpectroAnalyzer& a = spectro();
    m.spectro_used = true;
    spectro_gradient_from(s);
    a.update(sp);
    const int label_w = 6;
    const int cols = std::max(4, W - 2 - label_w - 1);
    const int rows = std::max(2, rows_total - 3);
    const bool image = sp.style == 1 && m.gfx_proto != GfxProto::None;
    const int unit = image ? std::max(1, m.cell_h) : 4;
    std::vector<std::string> lab(static_cast<size_t>(rows));
    const bool wf = sp.view == 1;
    if (!wf)
        for (const auto& l : spectro_axis(sp, a.rate(), rows * unit, unit))
            if (l.first >= 0 && l.first < rows) lab[static_cast<size_t>(l.first)] = l.second;
    std::string title = wf ? "WATERFALL" : "SPECTROGRAM";
    const std::string name = st.tuned_name.empty() ? st.info.station : st.tuned_name;
    if (!name.empty()) title += " - " + shown_text(name);
    if (!st.info.title.empty()) title += ": " + shown_text(st.info.artist.empty() ? st.info.title : st.info.artist + " - " + st.info.title);
    const std::string bar = s.border + c.box_vertical + kReset;
    std::vector<std::string> out;
    out.push_back(box_top(s, W, truncate_str(title, W - 6), s.border));
    if (wf) {   // the waterfall has no time axis: the frequencies of the front graph take the ruler's row
        std::string ruler(static_cast<size_t>(cols), ' ');
        int cwp = 10;
        if (image) cwp = std::max(1, m.cell_w);
        if (sp.labels)
            for (const auto& l : spectro_axis_x(sp, a.rate(), cols * (image ? cwp : 2), image ? cwp : 2))
                for (size_t k = 0; k < l.second.size() && static_cast<size_t>(l.first) + k < ruler.size(); ++k) ruler[static_cast<size_t>(l.first) + k] = l.second[k];
        out.push_back(bar + s.legend + spaces(label_w + 1) + ruler + kReset + bar);
    } else {
        std::string ruler(static_cast<size_t>(cols), ' ');
        const int step = sp.span <= 5 ? 1 : sp.span <= 10 ? 2 : sp.span <= 30 ? 5 : 10;
        for (int t = 0; t <= sp.span; t += step) {
            const int x = static_cast<int>(static_cast<long long>(t) * (cols - 1) / std::max(1, sp.span));
            const std::string txt = sp.motion == 1 ? (t == sp.span ? "now" : "-" + std::to_string(sp.span - t) + "s") : std::to_string(t) + "s";
            const int x0 = std::clamp(x - (t == 0 ? 0 : t == sp.span ? static_cast<int>(txt.size()) - 1 : static_cast<int>(txt.size()) / 2), 0, std::max(0, cols - static_cast<int>(txt.size())));
            for (size_t k = 0; k < txt.size() && x0 + static_cast<int>(k) < cols; ++k) ruler[static_cast<size_t>(x0) + k] = txt[k];
        }
        out.push_back(bar + s.legend + spaces(label_w + 1) + ruler + kReset + bar);
    }
    std::vector<std::string> body;
    if (image) { m.spectro_area[0] = 2; m.spectro_area[1] = 1 + label_w + 1; m.spectro_area[2] = cols; m.spectro_area[3] = rows; }
    else body = a.braille(sp, cols, rows);
    for (int r = 0; r < rows; ++r) {
        const std::string l = sp.labels ? lab[static_cast<size_t>(r)] : std::string();
        std::string line = bar + s.legend + spaces(std::max(0, label_w - display_width(l))) + l + kReset + " ";
        line += image ? gfx_hole(cols) : (r < static_cast<int>(body.size()) ? body[static_cast<size_t>(r)] : spaces(cols));
        out.push_back(line + bar);
    }
    out.push_back(box_bottom(s, W, "[SHIFT+i] options  [SHIFT+u / ESC] close  " + spectro_row_value(sp, kSrScale) + " " +
                                   std::to_string(sp.min_freq) + "-" + std::to_string(std::min(sp.max_freq, a.rate() / 2)) + " Hz", s.border_bottom));
    if (m.overlay == 6) {
        const auto panel = build_overlay(s, m, st);
        const int ph = static_cast<int>(panel.size());
        const int x = std::max(0, (W - kSpectroOverlayW) / 2);
        const int y = std::clamp((static_cast<int>(out.size()) - ph) / 2, 0, std::max(0, static_cast<int>(out.size()) - ph));
        for (int i = 0; i < ph && y + i < static_cast<int>(out.size()); ++i)
            out[static_cast<size_t>(y + i)] = splice_line(out[static_cast<size_t>(y + i)], x, panel[static_cast<size_t>(i)], kSpectroOverlayW);
        m.spectro_skip[0] = y; m.spectro_skip[1] = x; m.spectro_skip[2] = ph; m.spectro_skip[3] = kSpectroOverlayW;
    }
    return out;
}

// SHIFT+0: the oscilloscope over the whole screen, YT on the left and XY on the right, each in its own phosphor buffer (fed from the
// engine's scope), with an optional grid (SHIFT+a) in an adjustable colour (SHIFT+c). The options overlay can sit on top of it.
RadioScope& big_scope(int i) { static RadioScope sc[2]; return sc[i & 1]; }

constexpr int kScopeColorW = 48;
constexpr int kScopeMetricsW = 52;

// The window of the grid metrics (SHIFT+p in the oscilloscope overlay): what the axis values are measured in, the divisions of the
// grid and the measurement each picture's name shows -- separately for the YT and the XY picture.
std::vector<std::string> build_scope_metrics_panel(const Style& s, const UiModel& m) {
    const RadioSettings& c = s.c;
    const int W = kScopeMetricsW, inner = W - 4;
    const std::string bar = s.border + c.box_vertical + kReset;
    auto row = [&](const std::string& plain, bool hi, bool head) {
        const std::string x = truncate_str(plain, inner);
        const std::string body = x + spaces(inner - display_width(x));
        return bar + " " + (hi ? "\x1b[7m" + body + kReset : head ? s.legend + body + kReset : body) + " " + bar;
    };
    std::vector<std::string> lines;
    lines.push_back(box_top(s, W, "Grid metrics", s.border));
    for (const auto& r : scope_metric_panel_rows(c.scope_metrics, m.scope_metrics_row, inner)) {
        const bool head = !r.second && r.first.size() > 1 && r.first[0] != ' ' && r.first[0] != '>';
        lines.push_back(row(r.first, r.second, head));
    }
    lines.push_back(row("Readouts are averaged and update twice a second.", false, false));
    lines.push_back(row("[UP/DOWN] select  [LEFT/RIGHT] change", false, false));
    lines.push_back(box_bottom(s, W, "[R] reset  [SHIFT+p / ESC] close", s.border_bottom));
    return lines;
}

// The small window for typing the grid colour (SHIFT+c in the oscilloscope overlay): the digits typed so far and a swatch.
std::vector<std::string> build_scope_color_panel(const Style& s, const UiModel& m) {
    const RadioSettings& c = s.c;
    const int W = kScopeColorW, inner = W - 4;
    const std::string bar = s.border + c.box_vertical + kReset;
    auto row = [&](const std::string& plain) {
        const std::string x = truncate_str(plain, inner);
        return bar + " " + x + spaces(inner - display_width(x)) + " " + bar;
    };
    const std::string shown = m.scope_big_color_buf.empty() ? (c.scope_grid_color.empty() ? c.list_fg : c.scope_grid_color)
                                                            : std::to_string(std::clamp(std::atoi(m.scope_big_color_buf.c_str()), 0, 255));
    std::vector<std::string> lines;
    lines.push_back(box_top(s, W, "Grid color", s.border));
    lines.push_back(row("Color (0-255): " + m.scope_big_color_buf + "█"));
    const std::string label = "Preview: ";
    std::string blocks;
    for (int i = 0; i < 8; ++i) blocks += "█";
    lines.push_back(bar + " " + label + ansi_fg(shown) + blocks + kReset + spaces(inner - static_cast<int>(label.size()) - 8) + " " + bar);
    lines.push_back(row("Empty = color of inactive list elements"));
    lines.push_back(box_bottom(s, W, "[ENTER] ok  [ESC] cancel", s.border_bottom));
    return lines;
}

std::vector<std::string> build_scope_big(const Style& s, const UiModel& m, const RadioStatus& st, RadioEngine& engine, int W, int rows_total) {
    const RadioSettings& c = s.c;
    const int inner = W - 2;
    const int avail = std::max(3, rows_total - 2);   // between the top and the bottom border
    const bool grid = c.scope_grid, values = c.scope_values;
    const ScopeBigGeom g = scope_big_geometry(inner, avail, values);
    RadioScope& yt = big_scope(0);
    RadioScope& xy = big_scope(1);
    yt.copy_ring_from(engine.scope());
    xy.copy_ring_from(engine.scope());
    // Osci style "image" on a terminal with Kitty graphics / Sixel: two real pixel pictures (their own values, osci_set[1])
    const bool image = c.osci_style == 1 && m.gfx_proto != GfxProto::None && g.cols > 0 && g.rows > 0;
    RadioScope::Params px = scope_params(c, m.dt, image ? 1 : 0);
    px.mode = 0;
    RadioScope::Params py = px;
    py.mode = 1;
    const OsciSet& os = c.osci_set[image ? 1 : 0];
    const int pal = os.palette;
    auto color_of = [&](int tx, int cols, uint8_t hue, float k) {
        if (pal == 0) return dim_ansi(s.osci_at(cols > 1 ? static_cast<float>(tx) / static_cast<float>(cols - 1) : 0.0f), k);
        int r = 255, gg = 255, b = 255;
        palette_rgb(s, pal, hue / 255.0f, r, gg, b);
        return rgb_seq(r, gg, b, k);
    };
    const std::string grid_ansi = ansi_fg(c.scope_grid_color.empty() ? c.list_fg : c.scope_grid_color);
    const ScopeMetrics& sm = c.scope_metrics;
    std::vector<std::string> left, right;
    if (image) {
        // The terminal draws the pictures itself: the boxes are placeholder cells (the main loop writes the pictures from
        // m.gfx / m.gfx2), the grid is part of each picture.
        const int cw0 = std::max(4, m.cell_w), ch0 = std::max(8, m.cell_h);
        const int cwq = std::clamp(osci_detail_cap(c.image_detail, true) / g.cols, 4, cw0);
        const int scale = std::clamp(c.image_scale, 1, 3);
        const int cws = std::max(2, std::min(cwq, cw0 / scale));
        const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
        int gr = 128, gg2 = 128, gb = 128;
        color_rgb(c.scope_grid_color.empty() ? c.list_fg : c.scope_grid_color, gr, gg2, gb);
        for (int k = 0; k < 2; ++k) {
            GfxFrame& F = k == 0 ? m.gfx : m.gfx2;
            RadioScope& sc = k == 0 ? yt : xy;
            const RadioScope::Params& pp = k == 0 ? py : px;
            F.id = k; F.scale = scale; F.out_cw = cw0; F.out_ch = ch0; F.crop = 0; F.cut_w = 0;
            F.cols = g.cols; F.rows = g.rows; F.w = g.cols * cws; F.h = g.rows * chs;
            F.row = 1 + g.top + 1;
            F.col = 1 + g.margin + g.lab + (k == 0 ? 0 : g.cols + g.gap + g.lab);
            sc.render_image(F.w, F.h, pp, F.level, F.hue);
            if (pal == 0) {
                for (int y = 0; y < F.h; ++y)
                    for (int x = 0; x < F.w; ++x) F.hue[static_cast<size_t>(y) * F.w + x] = static_cast<uint8_t>(x * 255 / std::max(1, F.w - 1));
            }
            for (int h2 = 0; h2 < 256; ++h2) {
                int r = 255, g3 = 255, b = 255;
                palette_rgb(s, pal, static_cast<float>(h2) / 255.0f, r, g3, b);
                F.pal[static_cast<size_t>(h2)] = {static_cast<uint8_t>(r), static_cast<uint8_t>(g3), static_cast<uint8_t>(b)};
            }
            if (grid) F.grid = k == 0 ? scope_grid_pixels(F.w, F.h, g.cols, g.rows, 1, sm.yt_time_div, sm.yt_value_div)
                                      : scope_grid_pixels(F.w, F.h, g.cols, g.rows, 0, sm.xy_div, sm.xy_div);
            else F.grid.clear();
            F.grid_rgb = {static_cast<uint8_t>(gr), static_cast<uint8_t>(gg2), static_cast<uint8_t>(gb)};
            F.active = true;
            F.fresh = m.gfx_due;
        }
        left.assign(static_cast<size_t>(g.rows), gfx_hole(g.cols));
        right = left;
    } else {
        left = scope_big_pane(yt, py, g.cols, g.rows, grid, 1, grid_ansi, color_of, sm.yt_time_div, sm.yt_value_div);
        right = scope_big_pane(xy, px, g.cols, g.rows, grid, 0, grid_ansi, color_of, sm.xy_div, sm.xy_div);
    }

    ScopeBigInfo info;   // the radio's scope always runs at 48 kHz
    info.m = sm;
    info.rotate = os.rotate;
    info.samples = std::clamp(os.trace, 128, 8192);
    info.ms = info.samples * 1000.0 / 48000.0;
    info.music = px.fixed_gain;
    {
        const RadioScope::Measure me = yt.measure(px);
        info.rms_db = me.rms_db;
        info.peak_db = me.peak_db;
        info.corr = me.corr;
        info.bal_db = me.bal_db;
    }
    const std::vector<std::string> body = scope_big_compose(g, inner, avail, left, right, s.legend, grid_ansi, info);
    std::string title = "OSCILLOSCOPE";
    const std::string name = st.tuned_name.empty() ? st.info.station : st.tuned_name;
    if (!name.empty()) title += " - " + shown_text(name);
    if (!st.info.title.empty()) title += ": " + shown_text(st.info.artist.empty() ? st.info.title : st.info.artist + " - " + st.info.title);
    const std::string bar = s.border + c.box_vertical + kReset;
    std::vector<std::string> out;
    out.push_back(box_top(s, W, truncate_str(title, W - 6), s.border));
    for (const std::string& line : body) out.push_back(bar + line + bar);
    const std::string foot_head = "[SHIFT+o] options  [" + muisc::symbol_key_label("=") + "/ESC] close  ";
    const std::string foot = scope_pick_head({foot_head + "[SHIFT+a] grid  [SHIFT+v] values  [SHIFT+c] color  [SHIFT+p] metrics",
                                              foot_head + "[SHIFT+a/v/c/p] grid / values / color / metrics"}, W - 6);
    out.push_back(box_bottom(s, W, truncate_str(foot, W - 6), s.border_bottom));
    std::vector<std::string> panel;
    int panel_w = kOsciOverlayW;
    int panel_rect[4] = {0, 0, 0, 0};   // x, y, w, h in cells, 0-based
    if (m.overlay == 1) panel = build_overlay(s, m, st);   // the oscilloscope options on top of it
    else if (m.scope_big_color_edit) { panel = build_scope_color_panel(s, m); panel_w = kScopeColorW; }
    else if (m.scope_metrics_open) { panel = build_scope_metrics_panel(s, m); panel_w = kScopeMetricsW; }
    if (!panel.empty()) {
        const int ph = static_cast<int>(panel.size());
        const int x = std::max(0, (W - panel_w) / 2);
        // the oscilloscope options start on the second row and only grow downwards (braille / image style have different row counts)
        const int y = m.overlay == 1 ? std::clamp(1, 0, std::max(0, static_cast<int>(out.size()) - ph))
                                     : std::clamp((static_cast<int>(out.size()) - ph) / 2, 0, std::max(0, static_cast<int>(out.size()) - ph));
        for (int i = 0; i < ph && y + i < static_cast<int>(out.size()); ++i)
            out[static_cast<size_t>(y + i)] = splice_line(out[static_cast<size_t>(y + i)], x, panel[static_cast<size_t>(i)], panel_w);
        m.spectro_skip[0] = y; m.spectro_skip[1] = x; m.spectro_skip[2] = ph; m.spectro_skip[3] = panel_w;
        panel_rect[0] = x; panel_rect[1] = y; panel_rect[2] = panel_w; panel_rect[3] = ph;
    }
    // image style: a panel cuts a hole into the pictures it covers (pixels cleared; Sixel keeps those cells too), and a
    // changed hole means new pictures
    for (int k = 0; k < 2 && image; ++k) {
        GfxFrame& F = k == 0 ? m.gfx : m.gfx2;
        int cut[4] = {-1, -1, -1, -1};
        if (panel_rect[2] > 0 && panel_rect[3] > 0) {
            const int c0 = std::max(panel_rect[0], F.col), c1 = std::min(panel_rect[0] + panel_rect[2], F.col + F.cols);
            const int r0 = std::max(panel_rect[1], F.row), r1 = std::min(panel_rect[1] + panel_rect[3], F.row + F.rows);
            if (c0 < c1 && r0 < r1) { cut[0] = c0 - F.col; cut[1] = r0 - F.row; cut[2] = c1 - c0; cut[3] = r1 - r0; }
        }
        if (cut[2] > 0) {
            F.cut_c = cut[0]; F.cut_r = cut[1]; F.cut_w = cut[2]; F.cut_h = cut[3];
            const int xa = F.cut_c * F.w / F.cols, xb = (F.cut_c + F.cut_w) * F.w / F.cols;
            const int ya = F.cut_r * F.h / F.rows, yb = (F.cut_r + F.cut_h) * F.h / F.rows;
            for (int y2 = ya; y2 < yb && y2 < F.h; ++y2)
                for (int x2 = xa; x2 < xb && x2 < F.w; ++x2) {
                    F.level[static_cast<size_t>(y2) * F.w + x2] = 0;
                    if (!F.grid.empty()) F.grid[static_cast<size_t>(y2) * F.w + x2] = 0;
                }
        } else F.cut_w = 0;
        if (!std::equal(cut, cut + 4, m.scope_cut_last + k * 4)) F.fresh = true;
        std::copy(cut, cut + 4, m.scope_cut_last + k * 4);
    }
    return out;
}

std::vector<std::string> render_radio_frame_inner(const UiModel& m, const RadioStatus& st, RadioEngine& engine, const RadioSettings& cfg, bool peek) {
    Style s(cfg);
    std::vector<std::string> out;
    m.gfx.active = false;   // set again below when the scope is drawn as an image
    m.gfx2.active = false;
    m.spectro_used = false;
    m.spectro_area[0] = -1;
    m.spectro_skip[2] = m.spectro_skip[3] = 0;
    if (m.wizard_open) return build_wizard(s, m, std::clamp(m.cols, kUiCols, kMaxCols), std::max(m.rows, kUiRows));
    if (m.cheat_open) return build_cheatsheet(s, m, std::clamp(m.cols, kUiCols, kMaxCols), std::max(m.rows, kUiRows));
    if (m.hmenu.open) return build_history_menu(s, m, std::clamp(m.cols, kUiCols, kMaxCols), std::max(m.rows, kUiRows));
    if (m.settings.open && !peek) return build_settings(cfg, m.settings, std::clamp(m.cols, kUiCols, kMaxCols), std::max(m.rows, kUiRows));
    if (m.lmenu.open) return build_lists_menu(s, m, st, std::clamp(m.cols, kUiCols, kMaxCols), std::max(m.rows, kUiRows));
    if (m.browse.open) return build_browse(s, m, st, std::clamp(m.cols, kUiCols, kMaxCols), std::max(m.rows, kUiRows));
    if (m.menu.open) return build_menu(s, m, st, std::clamp(m.cols, kUiCols, kMaxCols), std::max(m.rows, kUiRows));
    if (m.stov.open) return build_stations_overlay(s, m, st, std::clamp(m.cols, kUiCols, kMaxCols), std::max(m.rows, kUiRows));
    if (m.scope_big) return build_scope_big(s, m, st, engine, std::clamp(m.cols, kUiCols, kMaxCols), std::max(m.rows, kUiRows));
    if (m.spectro_full) return build_spectro_full(s, m, st, std::clamp(m.cols, kUiCols, kMaxCols), std::max(m.rows, kUiRows));

    // Sizing: width follows the terminal (clamped), the frame is as tall as the terminal, and only
    // the list boxes take the rows that are left over.
    const int W = std::clamp(m.cols, kUiCols, kMaxCols);
    const int rows = std::max(m.rows, kUiRows);
    constexpr int kFixedRows = (kPanelH + 2) + 5 + 3;                 // info panel + band + search = 24
    const int list_rows = std::max(4, rows - kFixedRows - 2);        // 2 = the list boxes' own borders

    // --- info panel (fixed height). Three blocks: ON AIR sign | station info (+ spectrum) | scope or sphere. A block
    // that is switched off (GENERAL tab) is left out; the scope takes all the free width, and when it is off the
    // other blocks are centred in the free space so the pane looks filled.
    const bool show_sign = cfg.on_air_ascii;   // the pulse wave lives behind the sign: no sign, no pulse
    const bool show_scope = cfg.scope_mode != 2;
    std::vector<int> bars;
    if (cfg.element_visualizer || cfg.scope_mode == 1) {
        auto& fft = engine.fft();
        fft.set_fluidity(cfg.visualizer_fluidity);
        fft.set_degradation_speed(cfg.visualizer_degradation_speed);
        fft.set_viscosity(cfg.visualizer_viscosity);
        bars = fft.compute_bars(std::min(kMetaW, 48), m.dt);
    }
    // Sign on: sign | station info | scope (scope takes the free width; with the scope off the sign and the info are
    // centred). Sign off (like the player's disk off): the station info moves to the left edge and the scope is
    // centred in the rest of the pane at its usual width.
    const int avail = W - 2;
    const int sep_w = show_sign ? kSepW : 0;
    const int fixed_w = (show_sign ? kLeftW : 0) + kMetaW + sep_w;
    const int rest_w = std::max(1, avail - fixed_w);
    const int natural_scope = std::max(1, avail - kLeftW - kSepW - kMetaW);
    const int scope_w = !show_scope ? 0 : show_sign ? rest_w : std::min(rest_w, natural_scope);
    const int extra = (show_scope || !show_sign) ? 0 : rest_w;
    const int ex_first = extra / 2;
    const int ex_second = extra - extra / 2;
    auto centred = [&](const std::string& row, int nat, int total) {
        const int l = std::max(0, (total - nat) / 2);
        return spaces(l) + row + spaces(std::max(0, total - nat - l));
    };
    out.push_back(box_top(s, W, "", s.border));
    std::vector<std::string> left, meta, scope;
    if (show_sign) left = build_left(s, m, st);
    std::vector<std::string> hifi_lines;
    if (cfg.element_visualizer && cfg.bars_style == 1) {   // the Hi-Fi style: stereo braille columns at the frame rate
        HifiView hv;
        hv.cells = std::min(kMetaW, 48); hv.rows = std::clamp(kPanelH - 11, 2, 4);
        hv.resolution = cfg.hifi_resolution; hv.channels = cfg.hifi_channels; hv.color = cfg.hifi_color; hv.scale = cfg.hifi_scale; hv.adapt = cfg.hifi_adapt; hv.split = cfg.hifi_split;
        hv.fluidity = cfg.hifi_fluidity; hv.degradation = cfg.hifi_degradation;
        hv.viscosity = cfg.hifi_viscosity; hv.dt = m.dt;
        hv.fscale = cfg.hifi_fscale; hv.gain = cfg.hifi_gain; hv.range = cfg.hifi_range;
        spectro_gradient_from(s);
        const auto& pal = spectro_palette(cfg.spectro.scheme);
        hifi_lines = hifi_rows(hifi(), hv, [&](float t) { return s.viz_at(t); },
            [&](float f) {
                const auto& c = pal[static_cast<size_t>(std::clamp(48 + static_cast<int>(f * 207.0f), 0, 255))];
                return "\x1b[38;2;" + std::to_string(c[0]) + ";" + std::to_string(c[1]) + ";" + std::to_string(c[2]) + "m";
            });
        for (auto& l : hifi_lines) l += spaces(kMetaW - hv.cells);
    }
    meta = build_meta(s, st, cfg.element_visualizer ? &bars : nullptr, hifi_lines.empty() ? nullptr : &hifi_lines);
    const bool off_air = st.state == StreamState::Idle || st.state == StreamState::Failed;
    bool spectro_img = false;
    if (show_scope && !off_air && cfg.scope_mode == 3) {   // the spectrogram (like Audacity's), braille or a picture
        m.spectro_used = true;
        spectro_gradient_from(s);
        spectro().update(cfg.spectro);
        if (cfg.spectro.style == 1 && m.gfx_proto != GfxProto::None) {
            spectro_img = true;
            scope.assign(kPanelH, gfx_hole(scope_w));
        } else scope = spectro().braille(cfg.spectro, scope_w, kPanelH);
    } else if (show_scope) scope = off_air ? build_satellite(s, m.t_sec, scope_w)
                          : cfg.scope_mode == 1 ? build_sphere(s, bars, m.dt, scope_w)
                                                : build_scope(s, m, engine, false, scope_w);
    if (show_scope && !m.scope_notice.empty() && !scope.empty()) {   // timeshift jumps: a note at the bottom of the scope block
        const std::string t = truncate_str(m.scope_notice, std::max(1, scope_w - 2));
        const int tw = display_width(t);
        const int l = std::max(0, (scope_w - tw) / 2);
        scope.back() = spaces(l) + s.header + t + kReset + spaces(std::max(0, scope_w - l - tw));
    }
    for (int i = 0; i < kPanelH; ++i) {
        const size_t r = static_cast<size_t>(i);
        std::string body;
        if (show_sign) body += centred(left[r], kLeftW, kLeftW + ex_first);
        if (show_sign) body += s.border + " : : " + kReset;
        body += centred(meta[r], kMetaW, kMetaW + (show_sign ? ex_second : 0));
        if (show_scope) body += show_sign ? scope[r] : centred(scope[r], scope_w, rest_w);
        else if (!show_sign) body += spaces(rest_w);
        out.push_back(s.border + cfg.box_vertical + kReset + body + s.border + cfg.box_vertical + kReset);
    }
    out.push_back(box_bottom(s, W, "", s.border_bottom));

    // --- frequency band + buttons (the band takes the width, the button pane is fixed, 1 blank column between) ---
    auto tuner = build_tuner(s, m, st, W - kSideW - kBandGap);
    auto side = build_side(s, st);
    for (size_t i = 0; i < 5; ++i) out.push_back(tuner[i] + spaces(kBandGap) + side[i]);

    // --- search, lists ---
    for (auto& l : build_search(s, m, st, W)) out.push_back(l);
    for (auto& l : build_lists(s, m, st, W, list_rows)) out.push_back(l);
    // The image sits on the scope block: its cell position (row 1 = under the top border) ...
    int scope_x = 0;
    if (spectro_img) {
        m.spectro_area[0] = 1;
        m.spectro_area[1] = 1 + (show_sign ? kLeftW + ex_first + kSepW : 0) + kMetaW + (show_sign ? ex_second : 0) + (show_sign ? 0 : std::max(0, (rest_w - scope_w) / 2));
        m.spectro_area[2] = scope_w;
        m.spectro_area[3] = kPanelH - (m.scope_notice.empty() ? 0 : 1);   // the timeshift note keeps its row
    }
    if (m.gfx.active && show_scope) {
        scope_x = 1 + (show_sign ? kLeftW + ex_first + kSepW : 0) + kMetaW + (show_sign ? ex_second : 0) + (show_sign ? 0 : std::max(0, (rest_w - scope_w) / 2));
        m.gfx.col = scope_x; m.gfx.row = 1; m.gfx.crop = 0;
    }
    if (m.overlay != 0) {   // SHIFT+o / SHIFT+v overlay, centred over the finished screen
        const auto panel = build_overlay(s, m, st);
        const int pw = m.overlay == 10 ? kBarsOverlayW : m.overlay == 9 ? kAdevOverlayW : m.overlay == 8 ? kUpdateOverlayW : m.overlay == 7 ? kMenusOverlayW : m.overlay == 6 ? kSpectroOverlayW : m.overlay == 5 ? kRecordOverlayW : m.overlay == 4 ? kEqOverlayW : m.overlay == 3 ? kSleepOverlayW : m.overlay == 1 ? kOsciOverlayW : kNormOverlayW;
        const int ph = static_cast<int>(panel.size());
        const int x = std::max(0, (W - pw) / 2);
        const int y = m.overlay == 10 ? std::clamp(kPanelH + 2, 0, std::max(0, rows - ph))   // the spectrum bars overlay: under the upper pane, so the bars stay in view
                    : m.overlay == 1 ? std::clamp(1, 0, std::max(0, rows - ph))   // oscilloscope options: second row, only grows downwards
                                     : std::clamp((rows - ph) / 2 - (m.overlay == 7 ? 0 : 2), 0, std::max(0, rows - ph));
        for (int i = 0; i < ph; ++i)
            out[static_cast<size_t>(y + i)] = splice_line(out[static_cast<size_t>(y + i)], x, panel[static_cast<size_t>(i)], pw);
        m.spectro_skip[0] = y; m.spectro_skip[1] = x; m.spectro_skip[2] = ph; m.spectro_skip[3] = pw;
        // ... and the columns an overlay covers stay free (an image is not covered by text drawn over it)
        if (m.gfx.active && y < 1 + kPanelH && y + ph > 1 && x < scope_x + scope_w && x + pw > scope_x)
            m.gfx.crop = std::clamp(x + pw - scope_x, 0, scope_w);
    }
    if (m.gfx.active && m.gfx.crop != m.gfx_last_crop) { m.gfx.fresh = true; }
    m.gfx_last_crop = m.gfx.active ? m.gfx.crop : -1;
    return out;
}

// BorderGradient=smooth: the finished frame's border lines get the colour of their row, fading from ColorBorderTop (first
// line with a border) to ColorBorderBottom (last one). The panel builders keep drawing with the two plain colours.
std::vector<std::string> render_radio_frame(const UiModel& m, const RadioStatus& st, RadioEngine& engine, const RadioSettings& cfg_in) {
    // SHIFT+p in the theme picker: the normal main screen, drawn with the highlighted theme and kept still (the first frame
    // is stored and handed out again until the preview closes, the theme changes or the window is resized)
    const SettingsModel& sm0 = m.settings;
    const bool peek = sm0.open && sm0.theme_peek && (sm0.theme_ui == 1 || sm0.theme_ui == 3) && sm0.theme_sel >= 0 &&
                      sm0.theme_sel < static_cast<int>(sm0.theme_list.size());
    static std::vector<std::string> peek_cache;
    static int peek_key = -1, peek_cols = 0, peek_rows = 0;
    if (peek && !peek_cache.empty() && peek_key == sm0.theme_sel && peek_cols == m.cols && peek_rows == m.rows) return peek_cache;
    if (!peek) peek_cache.clear();
    RadioSettings themed;
    if (peek) { themed = cfg_in; theme_apply(themed, sm0.theme_list[static_cast<size_t>(sm0.theme_sel)]); }
    const RadioSettings& cfg = peek ? themed : cfg_in;
    std::vector<std::string> out = render_radio_frame_inner(m, st, engine, cfg, peek);
    auto keep = [&](std::vector<std::string> v) {
        if (peek) { peek_cache = v; peek_key = sm0.theme_sel; peek_cols = m.cols; peek_rows = m.rows; }
        return v;
    };
    if (!cfg.border_smooth || (m.settings.open && !peek)) return keep(std::move(out));   // the settings screen paints its own gradient
    std::vector<std::string> glyphs = {"│", "─", "╭", "╮", "╰", "╯", "├", "┤", "┬", "┴", "┼", "┌", "└"};
    for (const std::string* g : {&cfg.box_upper_left, &cfg.box_upper_right, &cfg.box_lower_left, &cfg.box_lower_right, &cfg.box_vertical, &cfg.box_horizontal})
        if (!g->empty()) glyphs.push_back(*g);
    return keep(border_gradient::apply(out, ansi_fg(cfg.border_top), ansi_fg(cfg.border_bottom), glyphs,
                                       [&](float t) { return gradient_fg(cfg.border_top, cfg.border_bottom, t); }));
}

// The scope window (SHIFT+9): the values and colours of the style in use (braille or image), and "station: artist - title" as its title.
void fill_scope_window_config(const RadioSettings& cfg, const RadioStatus& st, ScopeWinConfig& c) {
    const Style s(cfg);
    const bool braille = cfg.osci_style == 0;   // the style set in the SHIFT+o overlay: braille = the terminal's dot grid, scaled up
    const OsciSet& o = cfg.osci_set[braille ? 0 : 1];
    c.braille = braille ? 1 : 0;
    c.mode = static_cast<uint8_t>(std::clamp(cfg.osci_mode, 0, 4));
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
        palette_rgb(s, o.palette, static_cast<float>(h) / 255.0f, r, g, b);
        c.pal[h][0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
        c.pal[h][1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
        c.pal[h][2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
    }
    std::string title = st.tuned_name.empty() ? st.info.station : st.tuned_name;
    const std::string song = st.info.artist.empty() ? st.info.title : st.info.artist + " - " + st.info.title;
    if (!song.empty() && song != " - ") title += (title.empty() ? "" : ": ") + song;
    std::snprintf(c.title, sizeof c.title, "%s", title.c_str());
}

// The spectrogram window (SHIFT+8): the SHIFT+i options, the colours of the scheme in use, the same title.
void fill_spectro_window_config(const RadioSettings& cfg, const RadioStatus& st, SpectroWinConfig& c) {
    spectro_win_pack(cfg.spectro, c);
    spectro_gradient_from(Style(cfg));
    const auto& pal = spectro_palette(cfg.spectro.scheme);
    for (int h = 0; h < 256; ++h) for (int k = 0; k < 3; ++k) c.pal[h][k] = pal[static_cast<size_t>(h)][static_cast<size_t>(k)];
    ScopeWinConfig sc;   // the title as the scope window has it
    fill_scope_window_config(cfg, st, sc);
    std::memcpy(c.title, sc.title, sizeof c.title);
}


// ===========================================================================================
// THE REMOTE APP'S PHONE SCREEN (src/remote_server.h): narrow and tall, built from the radio's own pieces.
// ===========================================================================================
std::vector<std::string> build_phone_screen(const UiModel& m, const RadioStatus& st, const RadioSettings& cfg, int W, int H,
                                            std::vector<muisc::remote::net::Zone>& zones) {
    const Style s(cfg);
    W = std::max(W, 40);
    const int inner = W - 4;
    auto vis = [](const std::string& x) {
        std::string plain;
        for (size_t k = 0; k < x.size(); ++k) {
            if (x[k] == '\x1b' && k + 1 < x.size() && x[k + 1] == '[') {
                k += 2;
                while (k < x.size() && !(x[k] >= 0x40 && x[k] <= 0x7e)) ++k;
                continue;
            }
            plain += x[k];
        }
        return display_width(plain);
    };
    const bool tuned = st.state != StreamState::Idle && st.state != StreamState::Failed;

    int meta_rows = 5, gap = 1;
    auto need = [&]() { return 1 + 1 + kPanelH + 1 + meta_rows + 1 + gap + 3 + 1 + gap + 9; };
    while (need() > H && meta_rows > 3) --meta_rows;
    if (need() > H) gap = 0;
    while (need() > H && meta_rows > 1) --meta_rows;
    while (need() < H && meta_rows < 8) ++meta_rows;
    const int top_pad = std::max(0, H - need()) / 2;

    std::vector<std::string> out;
    auto row = [&](const std::string& content) {
        out.push_back(box_line(s, content + spaces(inner - vis(content)) + kReset, s.border));
    };
    auto sep = [&](const std::string& label) {
        const std::string head = "├" + s.c.box_horizontal + " " + label + " ";
        out.push_back(s.border + head + s.border + repeat(s.c.box_horizontal, std::max(0, W - 1 - display_width(head))) + "┤" + kReset);
    };
    struct Btn { std::string key, label, cmd, arg; };
    auto boxes = [&](const std::vector<Btn>& b) {
        const int n = static_cast<int>(b.size());
        const int bw = std::max(10, (W - 2) / n);
        const int left = (W - bw * n) / 2;
        std::string l0 = spaces(left), l1 = spaces(left), l2 = spaces(left);
        const int y = static_cast<int>(out.size());
        for (int i = 0; i < n; ++i) {
            l0 += box_top(s, bw, b[i].key, s.border);
            const std::string lab = truncate_str(b[i].label, bw - 4);
            const int pad = bw - 4 - display_width(lab);
            l1 += box_line(s, s.border + spaces(pad / 2) + lab + spaces(pad - pad / 2) + kReset, s.border);
            l2 += box_bottom(s, bw, "", s.border_bottom);
            if (!b[i].cmd.empty()) zones.push_back({left + i * bw, y, bw, 3, b[i].cmd, b[i].arg});
        }
        const std::string tail = spaces(W - left - bw * n);
        out.push_back(l0 + tail);
        out.push_back(l1 + tail);
        out.push_back(l2 + tail);
    };
    auto gradient = [&](std::vector<std::string> v) {
        if (!cfg.border_smooth) return v;
        std::vector<std::string> glyphs = {"\u2502", "\u2500", "\u256d", "\u256e", "\u2570", "\u256f", "\u251c", "\u2524", "\u252c", "\u2534", "\u253c", "\u250c", "\u2514"};
        for (const std::string* g : {&cfg.box_upper_left, &cfg.box_upper_right, &cfg.box_lower_left, &cfg.box_lower_right, &cfg.box_vertical, &cfg.box_horizontal})
            if (!g->empty()) glyphs.push_back(*g);
        return border_gradient::apply(v, ansi_fg(cfg.border_top), ansi_fg(cfg.border_bottom), glyphs,
                                      [&](float t) { return gradient_fg(cfg.border_top, cfg.border_bottom, t); });
    };

    // ---- the STATIONS list (the box of the main screen opens it): tap a station and it is tuned ----
    if (m.phone_view == 1) {
        const std::vector<Station> none;
        const std::vector<Station>& all = m.stations ? *m.stations : none;
        const int n = static_cast<int>(all.size());
        const int per = muisc::remote::net::phone_list_rows(H);
        const int pages = std::max(1, (n + per - 1) / per);
        const int page = std::clamp(m.phone_page, 0, pages - 1);
        out.push_back(box_top(s, W, "STATIONS (" + std::to_string(n) + ")  page " + std::to_string(page + 1) + "/" + std::to_string(pages), s.border));
        zones.push_back({0, 0, W, 1, "@menu", ""});
        row(m.phone_note.empty() ? s.legend + "tap a station to tune it" + kReset : "\x1b[38;5;246m" + truncate_str(m.phone_note, inner) + kReset);
        row("");
        for (int i = 0; i < per; ++i) {
            const int k = page * per + i;
            if (k >= n) { row(""); row(""); continue; }
            const Station& stn = all[static_cast<size_t>(k)];
            const bool on = k == st.tuned_index && tuned;
            const std::string name_col = on ? s.list_playing : s.val;
            row(s.key + pad_right(std::to_string(k + 1), 4) + kReset + name_col + truncate_str((on ? "\u25b6 " : "") + shown_text(stn.name), inner - 4) + kReset);
            std::string info = stn.genre;
            const std::string codec = (stn.bitrate_hint > 0 ? std::to_string(stn.bitrate_hint) + "k " : std::string()) + stn.codec_hint;
            if (!codec.empty()) info += (info.empty() ? "" : "  |  ") + codec;
            if (!stn.country.empty()) info += (info.empty() ? "" : "  |  ") + stn.country;
            row(spaces(4) + s.legend + truncate_str(info, inner - 4) + kReset);
            zones.push_back({0, static_cast<int>(out.size()) - 2, W, 2, "tune", std::to_string(k + 1)});
        }
        out.push_back(box_bottom(s, W, "", s.border_bottom));
        while (static_cast<int>(out.size()) < H - 3) out.push_back(spaces(W));
        boxes({{"", "\u25b2 UP", "remote-scroll", "-1"}, {"", "BACK", "remote-view", "0"}, {"", "DOWN \u25bc", "remote-scroll", "+1"}});
        if (static_cast<int>(out.size()) > H) out.resize(static_cast<size_t>(H));
        return gradient(out);
    }

    for (int i = 0; i < top_pad; ++i) out.push_back(spaces(W));

    // title + mode tabs
    out.push_back(box_top(s, W, "MOUSIKI PLUS", s.border));
    zones.push_back({0, static_cast<int>(out.size()) - 1, W, 1, "@menu", ""});
    {
        // the modes that are on, in the order of the mode key; the one in use in brackets; a tap on another one goes to it
        static const char* const kNames[3] = {"PLAYER", "RADIO", "BOOKS"};
        const muisc::ModeConfig mc = muisc::read_mode_config();
        int mypos = 0;
        for (int i = 0; i < 3; ++i) if (mc.order[i] == 1) mypos = i;
        std::string tabs;
        struct Tab { int x, w; const char* arg; };
        std::vector<Tab> hits;
        for (int i = 0; i < 3; ++i) {
            const int m = mc.order[i];
            if (m != 1 && !mc.use[m]) continue;
            const std::string name = m == 1 ? std::string("[") + kNames[m] + "]" : std::string(kNames[m]);
            if (!tabs.empty()) tabs += "  ";
            hits.push_back({vis(tabs), static_cast<int>(name.size()), m == 1 ? "" : (i == (mypos + 1) % 3 ? "1" : "-1")});
            tabs += ansi_fg(m == 1 ? cfg.tab_current : cfg.tab_other) + name + kReset;
        }
        const std::string hint = s.legend + "Switch >>" + kReset;
        row(tabs + spaces(inner - vis(tabs) - vis(hint)) + hint);
        const int ty = static_cast<int>(out.size()) - 1;
        for (const Tab& t : hits) if (*t.arg) zones.push_back({2 + t.x, ty, t.w, 1, "switch-mode", t.arg});
        zones.push_back({2 + inner - vis(hint), ty, vis(hint), 1, "switch-mode", ""});
    }

    // the ON AIR / OFF AIR sign with its colour wave and pulse (the left block of the main screen)
    {
        const std::vector<std::string> sign = build_left(s, m, st);
        const int left = std::max(0, (inner - kLeftW) / 2);
        const int y0 = static_cast<int>(out.size());
        for (const std::string& r : sign) row(spaces(left) + r);
        zones.push_back({2 + left, y0, kLeftW, kPanelH, "play-pause", ""});   // the sign: pause / resume
    }

    // station info
    sep("STATION");
    {
        std::vector<std::string> lines;
        const int vw = std::max(4, inner - 12);
        auto kv = [&](const char* label, const std::string& value) {
            if (value.empty()) return;
            lines.push_back(s.key + pad_right(label, 10) + kReset + ": " + s.val + truncate_str(value, vw) + kReset);
        };
        if (st.state != StreamState::Idle) {
            const StreamInfo& in = st.info;
            kv("Station", shown_text(in.station));
            kv("Title", shown_text(in.title));
            kv("Artist", in.artist);
            kv("Genre", in.genre);
            std::string bitrate = in.bitrate_kbps > 0 ? std::to_string(in.bitrate_kbps) + " kbps" + (in.codec.empty() ? "" : " " + in.codec) : in.codec;
            kv("Bitrate", bitrate);
            kv("Country", in.country);
            kv("Stream", in.host);
        }
        const int mid = (meta_rows - 1) / 2;
        for (int i = 0; i < meta_rows; ++i) {
            if (st.state == StreamState::Idle) {
                const std::string msg = "Currently No Station Tuned";
                row(i == mid ? spaces((inner - display_width(msg)) / 2) + s.list + msg + kReset : std::string());
            } else row(i < static_cast<int>(lines.size()) ? lines[static_cast<size_t>(i)] : std::string());
        }
    }
    // bottom border: the state (live / paused / behind live), like the player's time stamps
    {
        std::string state;
        switch (st.state) {
            case StreamState::Live:         state = st.timeshift_paused ? "PAUSED" : st.timeshift_shifted ? "TIMESHIFT" : "LIVE"; break;
            case StreamState::Connecting:   state = "CONNECTING"; break;
            case StreamState::Buffering:    state = "BUFFERING"; break;
            case StreamState::Reconnecting: state = "RECONNECTING"; break;
            case StreamState::Failed:       state = "OFFLINE"; break;
            default:                        state = "IDLE"; break;
        }
        std::vector<Seg> foot = {{s.border, "[ " + state + " ]"}};
        if (st.timeshift_shifted && st.behind_sec >= 1.0) {
            const int b = static_cast<int>(st.behind_sec);
            char t[32];
            std::snprintf(t, sizeof t, "[ -%d:%02d ]", b / 60, b % 60);
            foot.push_back({"", s.c.box_horizontal});
            foot.push_back({s.border, t});
        }
        if (st.muted) { foot.push_back({"", s.c.box_horizontal}); foot.push_back({s.border, "[ MUTED ]"}); }
        out.push_back(box_edge(s, false, W, foot, s.border_bottom));
    }

    for (int i = 0; i < gap; ++i) out.push_back(spaces(W));
    // the dummy buttons of the main screen, working: previous station, pause / resume (timeshift), next station
    boxes({{"", "<<<", "prev", ""}, {"", !tuned ? "PLAY" : st.timeshift_paused ? "PLAY" : "PAUSE", "play-pause", ""}, {"", ">>>", "next", ""}});
    {
        const int bars = 20, hashes = std::clamp(st.volume * bars / 100, 0, bars);
        std::string fill;
        for (const Seg& g : volume_fill_segs(s, hashes)) fill += g.ansi + g.text + kReset;
        const std::string v = s.border + "VOLUME BAR:[" + kReset + fill +
                              s.vol_possible + std::string(static_cast<size_t>(bars - hashes), '-') + kReset + s.border + "]" + kReset + " " +
                              (st.muted ? std::string("muted") : std::to_string(st.volume) + "%");
        const int left = std::max(0, (W - vis(v)) / 2);
        out.push_back(spaces(left) + v + spaces(W - left - vis(v)));
        zones.push_back({left, static_cast<int>(out.size()) - 1, vis(v), 1, "toggle-mute", ""});
    }
    for (int i = 0; i < gap; ++i) out.push_back(spaces(W));
    boxes({{"#", "RANDOM", "shuffle", ""}, {"p", st.muted ? "UNMUTE" : "MUTE", "toggle-mute", ""}, {"x", "STOP", "stop", ""}});
    boxes({{"-", "VOL -", "volume-down", ""}, {"}", "LIVE", "live", ""}, {"+", "VOL +", "volume-up", ""}});
    boxes({{"m", m.shuffle ? "MODE: S" : "MODE: L", "playback-mode", ""}, {"SHIFT+l", "STATIONS", "remote-view", "1"},
           {"", "MANUAL", "@manual", ""}});

    while (static_cast<int>(out.size()) < H) out.push_back(spaces(W));
    if (static_cast<int>(out.size()) > H) out.resize(static_cast<size_t>(H));
    // a screen too short for everything: what was cut off cannot be tapped either
    zones.erase(std::remove_if(zones.begin(), zones.end(), [&](const muisc::remote::net::Zone& z) { return z.y + z.h > H || z.x + z.w > W; }), zones.end());
    if (!cfg.border_smooth) return out;
    std::vector<std::string> glyphs = {"│", "─", "╭", "╮", "╰", "╯", "├", "┤", "┬", "┴", "┼", "┌", "└"};
    for (const std::string* g : {&cfg.box_upper_left, &cfg.box_upper_right, &cfg.box_lower_left, &cfg.box_lower_right, &cfg.box_vertical, &cfg.box_horizontal})
        if (!g->empty()) glyphs.push_back(*g);
    return border_gradient::apply(out, ansi_fg(cfg.border_top), ansi_fg(cfg.border_bottom), glyphs,
                                  [&](float t) { return gradient_fg(cfg.border_top, cfg.border_bottom, t); });
}

} // namespace muisc::radio
