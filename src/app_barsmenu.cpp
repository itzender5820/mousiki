// The spectrum bars overlay (SHIFT+b, Mode::BarsMenu): the style of the bars, the Hi-Fi rows and the motion. It is drawn UNDER the upper pane
// (render_frame), so the bars at the bottom of the meta column stay visible while the values are changed. Member functions of App.
#include "app.h"
#include "app_internal.h"
#include "bars_rows.h"
#include "overlay_common.h"
#include <algorithm>

namespace muisc {

std::vector<int> App::bars_menu_rows() const { return bars_rows(settings_); }

void App::bars_menu_adjust(int dir) {
    bars_change(settings_, bars_menu_row_, dir);
    if (!settings_.element_visualizer && bars_menu_row_ != kBmStyle) status_line_ = "the spectrum bars are switched off (Settings, GENERAL)";
}

std::vector<std::string> App::build_bars_menu_panel(int main_w) const {
    const int W = kBarsMenuPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi, const std::string& right = std::string()) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + (right.empty() ? bar : right);
    };
    std::vector<std::string> lines;
    lines.push_back(box_top("Spectrum Bars", W, border));
    // The room: the panel sits under the upper pane, so the bars stay visible. With less room than rows it scrolls (scroll bar in the right border).
    const std::vector<int> all = bars_menu_rows();
    const int n = static_cast<int>(all.size());
    const int pane = static_cast<int>(build_metadata_panel(main_w).size());
    const int overhead = 1 + 1 + 4;   // the top border, the blank line, the legend (three lines and the bottom border)
    int at = 0;
    for (int i = 0; i < n; ++i) if (all[static_cast<size_t>(i)] == bars_menu_row_) at = i;
    const BarsWindow bw = bars_window(n, at, bars_menu_scroll_, term_rows_ - 1 - pane - overhead);
    const int cap = bw.cap, first = bw.first;
    for (int i = 0; i < cap; ++i) {
        const int r = all[static_cast<size_t>(first + i)];
        const bool sel = r == bars_menu_row_;
        std::string right;
        if (bw.scrolls) right = border + ((i >= bw.thumb_at && i < bw.thumb_at + bw.thumb_len) ? std::string("\xE2\x94\x83") : settings_.box_vertical) + R;   // ┃ the thumb, the border line the track
        lines.push_back(row(std::string(sel ? "> " : "  ") + pad_right(bars_row_label(r), 22) + pad_left(bars_value(settings_, r), inner - 24), sel, right));
    }
    lines.push_back(row("", false));
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return bar + " " + border_bottom + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, inner - left - display_width(tx))), ' ') + "\x1b[0m" + " " + bar;
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[UP/DOWN] select  [LEFT/RIGHT] change", "[R] defaults (not the style)", "[" + hotkey_text("HKeyBarsMenu", "B") + " / ESC] close"}, leg_in, leg_bot);
    return lines;
}

}  // namespace muisc
