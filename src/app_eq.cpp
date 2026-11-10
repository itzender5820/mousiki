// app_eq.cpp
// Part of class App (src/app.h): The equalizer overlay.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "app_state_eq.h"
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


// Equaliser overlay (SHIFT+e).
//
// Presets are addressed by one "unified" index: 0 .. kEqPresets.size()-1 are
// the built-in presets, everything after that is settings_.eq_custom_presets
// in the order saved.
int App::eq_preset_count() const {
    return static_cast<int>(kEqPresets.size() + settings_.eq_custom_presets.size());
}

const EqGains& App::eq_preset_gains(int index) const {
    const int builtin = static_cast<int>(kEqPresets.size());
    if (index < builtin) return kEqPresets[std::clamp(index, 0, builtin - 1)].gains;
    const int c = std::clamp(index - builtin, 0, static_cast<int>(settings_.eq_custom_presets.size()) - 1);
    return settings_.eq_custom_presets[c].gains;
}

std::string App::eq_preset_name(int index) const {
    const int builtin = static_cast<int>(kEqPresets.size());
    if (index < builtin) return kEqPresets[std::clamp(index, 0, builtin - 1)].name;
    const int c = index - builtin;
    if (c < static_cast<int>(settings_.eq_custom_presets.size())) return settings_.eq_custom_presets[c].name;
    return "Custom";
}

// The preset the sliders currently equal, or -1. The preset the cycle last
// stopped on wins a tie, so a custom preset saved with the same curve as a
// built-in one keeps showing under its own name.
int App::eq_current_preset() const {
    const int n = eq_preset_count();
    const EqGains& g = settings_.eq_gains;
    if (eq_->last_preset >= 0 && eq_->last_preset < n && eq_gains_equal(eq_preset_gains(eq_->last_preset), g))
        return eq_->last_preset;
    for (int i = 0; i < n; ++i) {
        if (eq_gains_equal(eq_preset_gains(i), g)) return i;
    }
    return -1;
}

void App::eq_open() {
    const int m = eq_current_preset();
    if (m >= 0) eq_->last_preset = m;
    eq_->naming = false;
    eq_->name_buf.clear();
    eq_->status.clear();
    eq_->delete_armed = false;
    mode_ = Mode::Equalizer;
}

void App::eq_apply() {
    player_.set_equalizer(settings_.eq_enabled, settings_.eq_gains);
}

// Changing a band while the EQ is off switches it on: what you adjust is what
// you hear. Gains snap to whole dB (the grid the sliders are drawn on).
void App::eq_set_gain(int band, float db) {
    band = std::clamp(band, 0, kEqBands - 1);
    settings_.eq_gains[band] = std::clamp(std::round(db), kEqMinDb, kEqMaxDb);
    settings_.eq_enabled = true;
    eq_apply();
}

void App::eq_select_preset(int dir) {
    const int n = eq_preset_count();
    const int cur = eq_current_preset();
    const int base = cur >= 0 ? cur : std::clamp(eq_->last_preset, 0, n - 1);
    const int next = ((base + dir) % n + n) % n;
    settings_.eq_gains = eq_preset_gains(next);
    settings_.eq_enabled = true;
    eq_->last_preset = next;
    eq_apply();
}

// S: opens the "Save as:" prompt. When the sliders were moved away from a
// custom preset the prompt starts with that preset's name, so ENTER alone
// updates it; otherwise it starts empty.
void App::eq_begin_naming() {
    eq_->name_buf.clear();
    const int builtin = static_cast<int>(kEqPresets.size());
    if (eq_current_preset() < 0 && eq_->last_preset >= builtin && eq_->last_preset < eq_preset_count())
        eq_->name_buf = eq_preset_name(eq_->last_preset);
    eq_->naming = true;
    edit_owner_ = "eq-name"; // claim the shared caret -- see edit_focus()
    edit_caret_ = edit_anchor_ = eq_->name_buf.size();
}

// ENTER in the prompt: stores the current curve under the typed name. A name
// that a custom preset already has (any capitalisation) replaces that preset;
// built-in names and "Custom" are refused. The list is written to config.txt
// at once, so a preset survives even if the program is closed abruptly.
void App::eq_commit_name() {
    std::string name;
    for (char c : eq_->name_buf) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || u == 127) continue;                   // control characters
        name.push_back((c == '=' || c == '{' || c == '}') ? '-' : c); // would confuse the config parser
    }
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();

    if (name.empty()) {
        eq_->status = "Nothing saved: the name is empty";
        eq_->naming = false;
        eq_->name_buf.clear();
        return;
    }
    if (eq_name_reserved(name)) {
        eq_->status = "\"" + name + "\" is a built-in name, pick another";
        eq_->naming = false;
        eq_->name_buf.clear();
        return;
    }

    const int builtin = static_cast<int>(kEqPresets.size());
    int existing = -1;
    for (size_t i = 0; i < settings_.eq_custom_presets.size(); ++i) {
        if (eq_name_equal(settings_.eq_custom_presets[i].name, name)) { existing = static_cast<int>(i); break; }
    }
    if (existing >= 0) {
        settings_.eq_custom_presets[existing].name = name;
        settings_.eq_custom_presets[existing].gains = settings_.eq_gains;
        eq_->last_preset = builtin + existing;
        eq_->status = "Updated \"" + name + "\"";
    } else if (settings_.eq_custom_presets.size() >= kEqMaxCustomPresets) {
        eq_->status = "Limit of " + std::to_string(kEqMaxCustomPresets) + " custom presets reached";
        eq_->naming = false;
        eq_->name_buf.clear();
        return;
    } else {
        EqCustomPreset cp;
        cp.name = name;
        cp.gains = settings_.eq_gains;
        settings_.eq_custom_presets.push_back(std::move(cp));
        eq_->last_preset = builtin + static_cast<int>(settings_.eq_custom_presets.size()) - 1;
        eq_->status = "Saved \"" + name + "\"";
    }
    eq_->naming = false;
    eq_->name_buf.clear();
    save_settings(settings_);
}

// DEL / X: removes the custom preset the sliders currently equal. Built-in
// presets cannot be deleted. The first press only asks; the second one, with
// no other key in between, deletes. The sliders keep their gains (they simply
// read "Custom" afterwards) and the cycle continues from the neighbour.
void App::eq_delete_custom(bool confirmed) {
    const int builtin = static_cast<int>(kEqPresets.size());
    const int cur = eq_current_preset();
    if (cur < builtin) {
        eq_->status = cur < 0 ? "Select a custom preset to delete it" : "Built-in presets cannot be deleted";
        return;
    }
    const std::string name = eq_preset_name(cur);
    if (!confirmed) {
        eq_->delete_armed = true;
        eq_->status = "Press DEL / X again to delete \"" + name + "\"";
        return;
    }
    settings_.eq_custom_presets.erase(settings_.eq_custom_presets.begin() + (cur - builtin));
    eq_->last_preset = cur - 1;
    eq_->status = "Deleted \"" + name + "\"";
    save_settings(settings_);
}

std::vector<std::string> App::build_eq_panel() const {
    const int W = kEqPanelWidth;
    const int inner = W - 4;               // 54: 4 axis columns + 10 bands x 5 columns
    constexpr int kCell = 5;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string head = ansi_for(settings_.header_color, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto framed = [&](const std::string& content) { return bar + " " + content + " " + bar; };
    auto plain_row = [&](const std::string& plain) {
        return framed(pad_right(truncate_str(plain, inner), inner));
    };
    auto centre = [&](const std::string& t) { // exactly kCell columns
        const int w = display_width(t);
        const int left = std::max(0, (kCell - w) / 2);
        return pad_right(std::string(left, ' ') + t, kCell);
    };

    // Every legend line of this overlay -- the two rows under the sliders and
    // the footer in the bottom border -- is drawn in the border colour, so the
    // whole key legend reads as one block. (Other panels colour their legends
    // differently; here this was chosen on purpose.)
    const std::string legend = border_bottom;
    // The legend rows are centred in the panel, and so is the footer in the
    // bottom border (the dashes are split evenly on both sides of it).
    auto legend_row = [&](const std::string& plain) {
        const std::string t = truncate_str(plain, inner);
        const int tw = display_width(t);
        const int left = std::max(0, (inner - tw) / 2);
        const std::string body = std::string(left, ' ') + t + std::string(std::max(0, inner - tw - left), ' ');
        return bar + " " + (legend.empty() ? body : legend + body + R) + " " + bar;
    };
    auto legend_bottom = [&](const std::string& footer) {
        const std::string t = " " + truncate_str(footer, W - 6) + " ";
        const int dashes = std::max(0, W - 2 - display_width(t));
        const int left = dashes / 2, right = dashes - left;
        std::string out = settings_.box_lower_left;
        for (int i = 0; i < left; ++i) out += settings_.box_horizontal;
        out += t;
        for (int i = 0; i < right; ++i) out += settings_.box_horizontal;
        out += settings_.box_lower_right;
        return legend.empty() ? out : legend + out + R;
    };

    const EqGains& g = settings_.eq_gains;
    const bool on = settings_.eq_enabled;
    const int preset = eq_current_preset();
    char pre[24];
    { const float pa = eq_auto_preamp_db(g); std::snprintf(pre, sizeof pre, pa > -0.05f ? "0.0 dB" : "%+.1f dB", pa); }

    std::vector<std::string> lines;
    lines.push_back(box_top("Equalizer", W, border));
    lines.push_back(plain_row(std::string("Preset: ") + (preset >= 0 ? eq_preset_name(preset) : std::string("Custom")) +
                              "   EQ: " + (on ? "ON" : "OFF") + "   Preamp: " + pre));

    // Plot: 13 rows of 2 dB each, or 7 rows of 4 dB when the terminal is short.
    const int step = term_rows_ >= 24 ? 2 : 4;
    const int half_rows = 12 / step;
    const std::string dim = border;                                   // EQ off: bars in the border grey
    const std::string full = "\u2588\u2588\u2588", low = "\u2584\u2584\u2584", up = "\u2580\u2580\u2580";
    for (int i = -half_rows; i <= half_rows; ++i) {
        const int L = -i * step; // +12 at the top, -12 at the bottom
        std::string axis;
        if (L == 0 || L == 12 || L == -12 || L == 6 || L == -6) {
            char a[8];
            std::snprintf(a, sizeof a, "%+d", L);
            axis = pad_left(a, 3) + " ";
        } else {
            axis = "    ";
        }
        std::string content = axis;
        for (int b = 0; b < kEqBands; ++b) {
            const float v = g[b];
            const bool sel = b == eq_->band;
            const std::string colour = sel ? head : (on ? std::string() : dim);
            std::string glyph;
            if (L == 0) {
                content += (sel ? head : border) + "\u2500\u2500\u2500\u2500\u2500" + R;
                continue;
            } else if (L > 0) {
                if (v >= L) glyph = full;
                else if (v > L - step) glyph = low;
            } else {
                if (v <= L) glyph = full;
                else if (v < L + step && v < 0) glyph = up;
            }
            if (glyph.empty()) content += "     ";
            else content += " " + colour + glyph + (colour.empty() ? "" : R) + " ";
        }
        lines.push_back(framed(content));
    }

    // Values and band names underneath; the selected band is reversed.
    std::string vals = "    ", names = "    ";
    for (int b = 0; b < kEqBands; ++b) {
        char num[8];
        std::snprintf(num, sizeof num, "%+d", static_cast<int>(std::lround(g[b])));
        const std::string v = centre(g[b] == 0.0f ? "0" : num);
        const std::string n = centre(kEqLabels[b]);
        vals += b == eq_->band ? HI + v + R : v;
        names += b == eq_->band ? HI + n + R : n;
    }
    lines.push_back(framed(vals));
    lines.push_back(framed(names));

    // One row for the "Save as:" prompt or the last action's feedback. It is
    // always present (blank when idle) so the panel keeps the same height.
    if (eq_->naming) {
        const std::string prefix = "Save as: ";
        const EditPaint p = paint_edit_field(eq_->name_buf, edit_caret_, edit_anchor_,
                                             std::max(1, inner - display_width(prefix) - 1), "", true);
        const int fill = std::max(0, inner - display_width(prefix) - p.cols);
        lines.push_back(framed(prefix + p.s + std::string(fill, ' ')));
    } else {
        lines.push_back(plain_row(eq_->status));
    }

    if (eq_->naming) {
        push_legend_block(lines, W, border_bottom, {"Type a name (max " + std::to_string(kEqNameMaxBytes) + " characters)", "An existing custom name is replaced", "[ENTER] save  [ESC] cancel"}, legend_row, legend_bottom);
    } else {
        push_legend_block(lines, W, border_bottom, {"[</>] band  [UP/DOWN] gain  [,/.] preset  [0] zero", "[S] save preset  [DEL/X] delete preset", "[SPACE] on/off  [R] flat  [" + hotkey_text("HKeyEqualizer", "E") + " / ESC] close"}, legend_row, legend_bottom);
    }
    return lines;
}

} // namespace muisc
