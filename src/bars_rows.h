// The rows of the spectrum bars overlay (the player's and the radio's): their labels, values and changes in one place. Templates over the
// settings struct of each program (both have the same bar fields).
#pragma once
#include "bars_presets.h"
#include <algorithm>
#include <string>
#include <vector>

namespace muisc {

enum BarsRow { kBmStyle, kBmRes, kBmChan, kBmGap, kBmHeight, kBmAdapt, kBmFScale, kBmGain, kBmRange, kBmColor, kBmBehavior, kBmFlu, kBmDeg, kBmVis };

inline const char* bars_row_label(int r) {
    switch (r) {
        case kBmStyle: return "Style";
        case kBmBehavior: return "Behavior";
        case kBmRes: return "Resolution";
        case kBmChan: return "Channels";
        case kBmGap: return "Channel gap";
        case kBmHeight: return "Height";
        case kBmAdapt: return "Adapt height";
        case kBmFScale: return "Frequency scale";
        case kBmGain: return "Gain (dB)";
        case kBmRange: return "Range (dB)";
        case kBmColor: return "Color";
        case kBmFlu: return "Fluidity";
        case kBmDeg: return "Degradation";
        default: return "Viscosity";
    }
}

// the rows in the order they are listed: Style, the Hi-Fi rows (only with that style), then the motion
template <class S> std::vector<int> bars_rows(const S& s) {
    std::vector<int> r{kBmStyle};
    if (s.bars_style == 1) for (int i : {kBmRes, kBmChan, kBmGap, kBmHeight, kBmAdapt, kBmFScale, kBmGain, kBmRange, kBmColor}) r.push_back(i);
    for (int i : {kBmBehavior, kBmFlu, kBmDeg, kBmVis}) r.push_back(i);
    return r;
}

template <class S> std::string bars_value(const S& s, int r) {
    static const char* heights[] = {"full", "medium", "low", "very low"};
    static const char* scales[] = {"linear", "logarithmic", "mel", "bark", "erb"};
    switch (r) {
        case kBmStyle: return s.bars_style == 1 ? "Hi-Fi" : "Classic";
        case kBmRes: return s.hifi_resolution == 1 ? "rough" : "fine";
        case kBmChan: return s.hifi_channels == 1 ? "mono" : "stereo";
        case kBmGap: return s.hifi_split == 0 ? "remove" : "add";
        case kBmHeight: return heights[std::clamp(s.hifi_scale, 0, 3)];
        case kBmAdapt: return s.hifi_adapt == 1 ? "on" : "off";
        case kBmFScale: return scales[std::clamp(s.hifi_fscale, 0, 4)];
        case kBmGain: return std::to_string(s.hifi_gain);
        case kBmRange: return std::to_string(s.hifi_range);
        case kBmColor: return s.hifi_color == 1 ? "corresponding" : "gradient";
        case kBmBehavior: return bars_preset_name(bars_preset_of(s.bar_fluidity(), s.bar_degradation(), s.bar_viscosity()));
        case kBmFlu: return std::to_string(s.bar_fluidity());
        case kBmDeg: return std::to_string(s.bar_degradation());
        default: return std::to_string(s.bar_viscosity());
    }
}

template <class S> void bars_change(S& s, int r, int dir) {
    auto cyc = [&](int& v, int n) { v = ((v + dir) % n + n) % n; };
    switch (r) {
        case kBmStyle: cyc(s.bars_style, 2); break;
        case kBmRes: cyc(s.hifi_resolution, 2); break;
        case kBmChan: cyc(s.hifi_channels, 2); break;
        case kBmGap: cyc(s.hifi_split, 2); break;
        case kBmHeight: cyc(s.hifi_scale, 4); break;
        case kBmAdapt: cyc(s.hifi_adapt, 2); break;
        case kBmFScale: cyc(s.hifi_fscale, 5); break;
        case kBmGain: s.hifi_gain = std::clamp(s.hifi_gain + 2 * dir, -20, 20); break;
        case kBmRange: s.hifi_range = std::clamp(s.hifi_range + 4 * dir, 40, 100); break;
        case kBmColor: cyc(s.hifi_color, 2); break;
        case kBmBehavior: {   // the next / previous preset (from "custom": the first / last)
            const int now = bars_preset_of(s.bar_fluidity(), s.bar_degradation(), s.bar_viscosity());
            const int to = now == kBarsCustom ? (dir > 0 ? 0 : 3) : ((now + dir) % 4 + 4) % 4;
            s.bar_fluidity() = kBarsPresets[to].fluidity; s.bar_degradation() = kBarsPresets[to].degradation; s.bar_viscosity() = kBarsPresets[to].viscosity;
            break;
        }
        case kBmFlu: s.bar_fluidity() = std::clamp(s.bar_fluidity() + dir, 1, 10); break;
        case kBmDeg: s.bar_degradation() = std::clamp(s.bar_degradation() + dir, 1, 10); break;
        default: s.bar_viscosity() = std::clamp(s.bar_viscosity() + dir, 1, 10); break;
    }
}

// R: the Hi-Fi values and the motion of the style in use back to the defaults (the style is left alone)
template <class S> void bars_reset(S& s) {
    const S d{};
    s.hifi_resolution = d.hifi_resolution; s.hifi_channels = d.hifi_channels; s.hifi_color = d.hifi_color;
    s.hifi_scale = d.hifi_scale; s.hifi_adapt = d.hifi_adapt; s.hifi_split = d.hifi_split;
    s.hifi_fscale = d.hifi_fscale; s.hifi_gain = d.hifi_gain; s.hifi_range = d.hifi_range;
    s.bar_fluidity() = d.hifi_fluidity; s.bar_degradation() = d.hifi_degradation; s.bar_viscosity() = d.hifi_viscosity;
}

// The window of a list that is longer than the room: `cap` rows from `first`, and a scroll bar thumb (`thumb_at`, `thumb_len` rows).
struct BarsWindow { int first = 0, cap = 0, thumb_at = 0, thumb_len = 0; bool scrolls = false; };
inline BarsWindow bars_window(int n, int selected, int& scroll, int room) {
    BarsWindow w;
    w.cap = std::clamp(room, 4, std::max(4, n));
    w.cap = std::min(w.cap, n);
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + w.cap) scroll = selected - w.cap + 1;
    scroll = std::clamp(scroll, 0, std::max(0, n - w.cap));
    w.first = scroll;
    w.scrolls = n > w.cap;
    w.thumb_len = std::max(1, w.cap * w.cap / std::max(1, n));
    w.thumb_at = w.scrolls ? w.first * (w.cap - w.thumb_len) / std::max(1, n - w.cap) : 0;
    return w;
}

}  // namespace muisc
