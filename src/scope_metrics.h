#pragma once
// The grid metrics of the big oscilloscope overlay (SHIFT+p inside it): what the axis values are measured in, how many
// divisions the grid has and which extra measurement the picture's name shows -- separately for the YT and the XY picture.
// Header only and shared by the player and the radio (their settings, config files and overlays all use this one description).

#include "string_util.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace muisc {

struct ScopeMetrics {
    // YT picture
    int yt_time_unit = 0;     // x axis: 0 = milliseconds, 1 = samples, 2 = divisions (the name then says "ms/div")
    int yt_time_div = 8;      // vertical grid lines: 4, 8, 10 or 16 divisions across the width
    int yt_value_div = 4;     // horizontal grid lines: 2, 4, 8 or 10 divisions across the height
    int yt_info = 3;          // name shows: 0 = nothing, 1 = RMS level, 2 = peak hold, 3 = both
    // XY picture
    int xy_div = 4;           // grid lines per axis: 2, 4, 8 or 10 divisions
    int xy_info = 3;          // name shows: 0 = nothing, 1 = L/R correlation, 2 = L/R balance, 3 = both
};

constexpr int kScopeMetricRows = 6;

inline const std::vector<int>& scope_div_choices(int row) {
    static const std::vector<int> t = {4, 8, 10, 16}, v = {2, 4, 8, 10};
    return row == 1 ? t : v;   // row 1 = YT time divisions, the others are 2 / 4 / 8 / 10
}

// Row order of the overlay: 0..3 YT (time unit, time divisions, value divisions, info), 4..5 XY (divisions, info).
inline const char* scope_metric_label(int row) {
    static const char* const n[kScopeMetricRows] = {"Time unit", "Time divisions", "Value divisions", "Info", "Divisions", "Info"};
    return n[std::clamp(row, 0, kScopeMetricRows - 1)];
}

inline std::string scope_metric_value(const ScopeMetrics& m, int row) {
    static const char* const tu[3] = {"milliseconds", "samples", "divisions"};
    static const char* const yi[4] = {"off", "RMS level", "peak hold", "RMS + peak hold"};
    static const char* const xi[4] = {"off", "correlation", "balance", "correlation + balance"};
    switch (row) {
        case 0: return tu[std::clamp(m.yt_time_unit, 0, 2)];
        case 1: return std::to_string(m.yt_time_div);
        case 2: return std::to_string(m.yt_value_div);
        case 3: return yi[std::clamp(m.yt_info, 0, 3)];
        case 4: return std::to_string(m.xy_div);
        case 5: return xi[std::clamp(m.xy_info, 0, 3)];
    }
    return "";
}

// LEFT / RIGHT on a row (dir -1 / +1). Choices do not wrap, like the other overlays' rows.
inline void scope_metric_step(ScopeMetrics& m, int row, int dir) {
    auto step = [&](int& v, int n) { v = std::clamp(v + (dir > 0 ? 1 : -1), 0, n - 1); };
    auto step_div = [&](int& v, int r) {
        const std::vector<int>& c = scope_div_choices(r);
        int i = 0;
        for (size_t k = 0; k < c.size(); ++k) if (c[k] == v) i = static_cast<int>(k);
        i = std::clamp(i + (dir > 0 ? 1 : -1), 0, static_cast<int>(c.size()) - 1);
        v = c[static_cast<size_t>(i)];
    };
    switch (row) {
        case 0: step(m.yt_time_unit, 3); break;
        case 1: step_div(m.yt_time_div, 1); break;
        case 2: step_div(m.yt_value_div, 2); break;
        case 3: step(m.yt_info, 4); break;
        case 4: step_div(m.xy_div, 4); break;
        case 5: step(m.xy_info, 4); break;
    }
}

// ---- config files (player: "Key=value" lines in config.txt, radio: the same in radio_config.txt) ------------------------
inline const std::vector<std::string>& scope_metric_keys() {
    static const std::vector<std::string> k = {"ScopeYtTimeUnit", "ScopeYtTimeDiv", "ScopeYtValueDiv", "ScopeYtInfo", "ScopeXyDiv", "ScopeXyInfo"};
    return k;
}

// Takes one config line; true when the key belongs to the metrics (ScopeYtValueUnit / ScopeXyValueUnit from earlier versions are
// swallowed and ignored: the axis values are always percent now).
inline bool scope_metric_parse(ScopeMetrics& m, const std::string& key, const std::string& value) {
    std::string l = value;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto pick = [&](int& v, std::initializer_list<const char*> names) {   // by name or by index
        int i = 0;
        for (const char* n : names) { if (l == n) { v = i; return; } ++i; }
        const int idx = muisc::atoi_safe(l.c_str());
        if (!l.empty() && l[0] >= '0' && l[0] <= '9' && idx >= 0 && idx < i) v = idx;
    };
    auto div = [&](int& v, int row) {
        const int d = muisc::atoi_safe(l.c_str());
        for (int c : scope_div_choices(row)) if (c == d) v = d;
    };
    if (key == "ScopeYtTimeUnit") pick(m.yt_time_unit, {"ms", "samples", "div"});
    else if (key == "ScopeYtTimeDiv") div(m.yt_time_div, 1);
    else if (key == "ScopeYtValueDiv") div(m.yt_value_div, 2);
    else if (key == "ScopeYtInfo") pick(m.yt_info, {"off", "rms", "peak", "both"});
    else if (key == "ScopeXyDiv") div(m.xy_div, 4);
    else if (key == "ScopeXyInfo") pick(m.xy_info, {"off", "corr", "balance", "both"});
    else if (key == "ScopeYtValueUnit" || key == "ScopeXyValueUnit") { /* retired */ }
    else return false;
    return true;
}

inline std::string scope_metric_config_value(const ScopeMetrics& m, const std::string& key) {
    static const char* const tu[3] = {"ms", "samples", "div"};
    static const char* const yi[4] = {"off", "rms", "peak", "both"};
    static const char* const xi[4] = {"off", "corr", "balance", "both"};
    if (key == "ScopeYtTimeUnit") return tu[std::clamp(m.yt_time_unit, 0, 2)];
    if (key == "ScopeYtTimeDiv") return std::to_string(m.yt_time_div);
    if (key == "ScopeYtValueDiv") return std::to_string(m.yt_value_div);
    if (key == "ScopeYtInfo") return yi[std::clamp(m.yt_info, 0, 3)];
    if (key == "ScopeXyDiv") return std::to_string(m.xy_div);
    if (key == "ScopeXyInfo") return xi[std::clamp(m.xy_info, 0, 3)];
    return "";
}

inline void scope_metric_config_write(std::ostream& out, const ScopeMetrics& m, const char* comment_prefix) {
    out << comment_prefix << " Grid metrics of the big oscilloscope overlay (SHIFT+p in it), per picture. The axis values are always percent of the picture\n"
        << comment_prefix << " (100% = the loudest part with the automatic size, full scale in Osci music mode). YT: ScopeYtTimeUnit ms|samples|div,\n"
        << comment_prefix << " ScopeYtTimeDiv 4|8|10|16, ScopeYtValueDiv 2|4|8|10, ScopeYtInfo off|rms|peak|both (RMS level / peak hold in dB, a steady readout).\n"
        << comment_prefix << " XY: ScopeXyDiv 2|4|8|10, ScopeXyInfo off|corr|balance|both (L/R correlation -1..+1, L/R level balance in dB)\n";
    for (const std::string& k : scope_metric_keys()) out << k << "=" << scope_metric_config_value(m, k) << "\n";
}

inline std::string scope_metric_config_text(const ScopeMetrics& m, const char* comment_prefix) {
    std::ostringstream os;
    scope_metric_config_write(os, m, comment_prefix);
    return os.str();
}

// The lines of the SHIFT+p window (plain text, `inner` columns wide): a heading and the rows of each picture, the selected row
// marked ("> ") and flagged true so the caller can highlight it.
inline std::vector<std::pair<std::string, bool>> scope_metric_panel_rows(const ScopeMetrics& m, int sel, int inner) {
    std::vector<std::pair<std::string, bool>> out;
    auto pad = [](std::string t, int w) { if (static_cast<int>(t.size()) < w) t.append(static_cast<size_t>(w - static_cast<int>(t.size())), ' '); return t; };
    for (int r = 0; r < kScopeMetricRows; ++r) {
        if (r == 0) out.emplace_back("YT picture (amplitude over time)", false);
        if (r == 4) out.emplace_back("XY picture (left horizontal, right vertical)", false);
        const std::string label = pad(std::string(r == sel ? "> " : "  ") + scope_metric_label(r), 20);
        std::string v = scope_metric_value(m, r);
        const int room = std::max(0, inner - static_cast<int>(label.size()));
        if (static_cast<int>(v.size()) > room) v.resize(static_cast<size_t>(room));
        out.emplace_back(label + std::string(static_cast<size_t>(room - static_cast<int>(v.size())), ' ') + v, r == sel);
    }
    return out;
}

// ---- axis values -------------------------------------------------------------------------------------------------------
// The text of a value at `pct` percent of the picture (-100 .. +100): "0", "+50%", "-100%". The percentages are relative to the
// picture's scale: 100 % is the loudest part of the signal with the automatic size, full scale in Osci music mode.
inline std::string scope_value_text(double pct) {
    if (std::fabs(pct) < 1e-6) return "0";
    char b[32];
    std::snprintf(b, sizeof b, "%+.1f", pct);
    std::string s = b;
    if (s.size() > 2 && s.substr(s.size() - 2) == ".0") s.erase(s.size() - 2);
    return s + "%";
}

// ---- steady readouts ---------------------------------------------------------------------------------------------------
// The text of a level in dB for the picture names: whole dB, "-- dB" below -90 (silence).
inline std::string scope_db_text(double db) {
    if (db < -90.0) return "-- dB";
    char b[24];
    std::snprintf(b, sizeof b, "%.0f dB", db);
    return b;
}

} // namespace muisc
