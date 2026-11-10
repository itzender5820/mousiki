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
#include "radio_ui_internal.h"
#include "string_util.h"

namespace muisc::radio {

namespace detail {



// The bound key of an action as it reads on the keyboard: SHIFT+x for a capital, the symbol keys by layout.
std::string menus_key_label(const RadioSettings& c, const char* id) {
    const int code = key_of(c, id);
    if (code >= 'A' && code <= 'Z') return std::string("SHIFT+") + static_cast<char>(code + 32);
    if (code == ' ') return "SPACE";
    if (code <= 0) return "-";
    return muisc::symbol_key_label(std::string(1, static_cast<char>(code)));
}


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
    const int W = spo ? kSpectroOverlayW : rec ? kRecordOverlayW : eqo ? kEqOverlayW : sleep ? kSleepOverlayW : osci ? kOsciOverlayW : kNormOverlayW;
    const int inner = W - 4;
    auto row = [&](const std::string& plain, bool hi, const std::string& ansi = "") {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return box_line(s, (hi ? "\x1b[7m" : ansi) + body + kReset, s.border);
    };
    std::vector<std::string> lines;
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

} // namespace detail
using namespace detail;


int menus_move(int sel, int dir, bool classic) { return classic ? muisc::org::move_classic(muisc::org::radio_tree(), sel, dir) : muisc::org::move(muisc::org::radio_tree(), sel, dir); }
int menus_classic_sel(int sel) { return muisc::org::classic_sel(muisc::org::radio_tree(), sel); }
const muisc::org::Node& menus_node(int sel) {
    const muisc::org::Tree t = muisc::org::radio_tree();
    return t.n[muisc::org::clamp_sel(t, sel)];
}

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
    // the scale and range sit at the upper left, in front of the title, so the bottom line is free for the keys
    title = spectro_row_value(sp, kSrScale) + " " + std::to_string(sp.min_freq) + "-" + std::to_string(std::min(sp.max_freq, a.rate() / 2)) + " Hz  |  " + title;
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
    {
        const std::string keys = "[SHIFT+i] Options  [SHIFT+u / ESC] Close";
        const std::string foot = scope_pick_head({"[LEFT/RIGHT] Image / Braille  [UP/DOWN] Spectrogram / Waterfall  " + keys,
                                                  "[\u2190\u2192] Image / Braille  [\u2191\u2193] Spectro / Waterfall  " + keys, keys}, W - 6);
        out.push_back(box_bottom_centred(s, W, foot, s.border_bottom));
    }
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
                                                            : std::to_string(std::clamp(muisc::atoi_safe(m.scope_big_color_buf.c_str()), 0, 255));
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
    const int avail = std::max(3, rows_total - 3);   // between the top border and the legend line (the last row) -- the bottom border follows
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
    std::vector<std::string> body = scope_big_compose(g, inner, avail, left, right, s.legend, grid_ansi, info);
    std::string title = "OSCILLOSCOPE";
    const std::string name = st.tuned_name.empty() ? st.info.station : st.tuned_name;
    if (!name.empty()) title += " - " + shown_text(name);
    if (!st.info.title.empty()) title += ": " + shown_text(st.info.artist.empty() ? st.info.title : st.info.artist + " - " + st.info.title);
    const std::string bar = s.border + c.box_vertical + kReset;
    std::vector<std::string> out;
    out.push_back(box_top(s, W, truncate_str(title, W - 6), s.border));
    for (const std::string& line : body) out.push_back(bar + line + bar);
    {   // the key legend: two centred lines -- what changes the picture in the last row, the rest in the bottom border
        const std::string l1 = scope_pick_head({"[LEFT/RIGHT] Image / Braille   [UP/DOWN] Osci music mode on / off   [SHIFT+a] Grid   [SHIFT+v] Values",
                                                "[LEFT/RIGHT] Image / Braille   [UP/DOWN] Osci music on / off   [SHIFT+a] Grid   [SHIFT+v] Values",
                                                "[\u2190\u2192] Image / Braille  [\u2191\u2193] Osci music  [SHIFT+a/v] Grid / Values"}, inner);
        const int w1 = display_width(l1), left1 = std::max(0, (inner - w1) / 2);
        out.push_back(bar + s.legend + std::string(static_cast<size_t>(left1), ' ') + l1 + kReset + std::string(static_cast<size_t>(std::max(0, inner - left1 - w1)), ' ') + bar);
    }
    const std::string foot_head = "[SHIFT+o] Options  [" + muisc::symbol_key_label("=") + " / ESC] Close  ";
    const std::string foot = scope_pick_head({foot_head + "[SHIFT+c] Grid color  [SHIFT+p] Grid metrics",
                                              foot_head + "[SHIFT+c/p] Color / Metrics",
                                              "[" + muisc::symbol_key_label("=") + " / ESC] Close"}, W - 6);
    out.push_back(box_bottom_centred(s, W, foot, s.border_bottom));
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
    if (m.overlay == 7 && !s.c.menu_classic) {   // the MENUS index (<): the organigram over the whole screen
        const muisc::org::Tree tree = muisc::org::radio_tree();
        const int sel = muisc::org::clamp_sel(tree, m.overlay_row);
        muisc::org::Theme th;
        th.line = s.border; th.path = s.header; th.key = s.legend;
        th.key_text = [&](const muisc::org::Node& n) { return n.action ? menus_key_label(s.c, n.action) : std::string(); };
        const muisc::org::Node& nd = tree.n[sel];
        std::string info = nd.title;
        if (nd.action) info += "   [" + menus_key_label(s.c, nd.action) + "]";
        info += !nd.action ? "   -   ENTER or DOWN goes in" : muisc::org::children_of(tree, sel).empty() ? "   -   ENTER opens it" : "   -   ENTER opens it, DOWN goes to its menus";
        const int H = static_cast<int>(out.size());
        const auto body = muisc::org::render(tree, sel, W - 4, H - 2, m.t_sec, th, info);
        std::vector<std::string> panel;
        panel.push_back(box_top(s, W, "MENUS & OVERLAYS", s.border));
        for (const auto& r : body) panel.push_back(box_line(s, r, s.border));
        panel.push_back(box_bottom(s, W, std::string("[\u2190\u2192] Neighbours  [\u2191] Back  [\u2193] In  [ENTER] Open  [SHIFT+T] Classic  ") + "[ESC / " + menus_key_label(s.c, "Menus") + "] Close", s.border_bottom));
        for (size_t i = 0; i < panel.size() && i < out.size(); ++i) out[i] = panel[i];
        m.spectro_skip[0] = 0; m.spectro_skip[1] = 0; m.spectro_skip[2] = static_cast<int>(out.size()); m.spectro_skip[3] = W;
        if (m.gfx.active) m.gfx.crop = scope_w;   // (the whole screen is the organigram: no picture under it)
    } else
    if (m.overlay != 0) {   // SHIFT+o / SHIFT+v overlay, centred over the finished screen
        std::vector<std::string> panel;
        int pw = 0;
        if (m.overlay == 7) {   // the MENUS index, classic view: a regular overlay as big as its two columns need
            const muisc::org::Tree tree = muisc::org::radio_tree();
            muisc::org::Theme th;
            th.line = s.border; th.path = s.header; th.key = s.legend;
            th.key_text = [&](const muisc::org::Node& n) { return n.action ? menus_key_label(s.c, n.action) : std::string(); };
            const std::string legend = std::string("[\u2191\u2193] Entry  [\u2190\u2192] Column  [ENTER] Open  [SHIFT+T] Tree  [ESC / ") + menus_key_label(s.c, "Menus") + "] Close";
            const auto pnl = muisc::org::render_classic(tree, m.overlay_row, th, std::max(8, rows - 2), std::max(24, W - 6));
            int legend_w = 0;
            for (size_t i = 0; i < legend.size();) { const unsigned char ch = static_cast<unsigned char>(legend[i]); i += ch < 0x80 ? 1 : ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : 2; ++legend_w; }
            const int inner = std::max(pnl.inner_w, std::min(W - 4, legend_w + 2));
            pw = inner + 4;
            panel.push_back(box_top(s, pw, "MENUS & OVERLAYS", s.border));
            for (const auto& r : pnl.rows) panel.push_back(box_line(s, r + std::string(static_cast<size_t>(inner - pnl.inner_w), ' '), s.border));
            panel.push_back(box_bottom(s, pw, legend, s.border_bottom));
        } else {
            panel = build_overlay(s, m, st);
        pw = m.overlay == 10 ? kBarsOverlayW : m.overlay == 9 ? kAdevOverlayW : m.overlay == 8 ? kUpdateOverlayW : m.overlay == 6 ? kSpectroOverlayW : m.overlay == 5 ? kRecordOverlayW : m.overlay == 4 ? kEqOverlayW : m.overlay == 3 ? kSleepOverlayW : m.overlay == 1 ? kOsciOverlayW : kNormOverlayW;
        }
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
