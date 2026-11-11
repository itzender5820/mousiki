// Radio mode screen (part of radio_ui.cpp, split for the build time; the helpers are declared in radio_ui_internal.h).
#include "radio_ui_internal.h"

namespace muisc::radio {
namespace detail {


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
                      const std::string& query_ansi) {
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
                                    const std::vector<std::string>* hifi_lines) {
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

RadioScope::Params scope_params(const RadioSettings& c, double dt, int style) {   // style: 0 braille, 1 image, -1 the one in use
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
    // "c:" / "cs:" in the SEARCH box: the pane shows the command history with its replies / the scripts that match (radio_ui_cli.cpp)
    const bool cli_pane = search_is_cli(m.search) || search_is_cs(m.search);
    std::string cli_foot;
    std::vector<std::string> cli_rows;
    if (cli_pane) cli_rows = cli_pane_rows(s, m, left_w - 4, list_rows, pane_label, cli_foot);
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
        if (cli_pane) { left.push_back(box_line(s, cli_rows[static_cast<size_t>(r)], s.border)); continue; }
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
    left.push_back(box_bottom(s, left_w, cli_pane ? cli_foot : hidden > 0 ? "( " + std::to_string(hidden) + " more )" : "", s.border_bottom));

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
void push_legend(std::vector<std::string>& out, const Style& s, const std::vector<LegendGroup>& groups, int W, int max_rows) {
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
                                        int min_inner, const std::vector<Seg>& line2, int line2_cols) {
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

} // namespace detail
} // namespace muisc::radio
