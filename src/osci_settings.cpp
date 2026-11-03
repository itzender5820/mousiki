// Oscilloscope settings: the palette names, trace length and resolution steps, and the mapping between
// the settings' text values and their indices.
#include "settings.h"
#include "utf8_util.h"
#include "viz_prefs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ostream>
#include <sstream>

// The oscilloscope's settings: the rows of the SHIFT+o overlay (labels, values, stepping), the per-style parameter sets and
// their config keys. The code is shared by the music player and the radio: both settings structs inherit VizPrefs (viz_prefs.h).

namespace muisc {

const char* const kOsciPaletteNames[kOsciPaletteCount] = {"gradient", "settings", "temperature", "aurora", "magma", "ice", "neon", "spectrum", "classic green"};

namespace {
int osci_trace_step(int t, int dir) {   // Trace Length: 128 .. 1024 in steps of 128, then 2048, 4096, 8192
    if (dir > 0) return t < 1024 ? std::min(1024, t + 128) : std::min(8192, t * 2);
    return t <= 1024 ? std::max(128, t - 128) : t / 2;
}
const int kFpsSteps[6] = {30, 45, 60, 90, 120, 165};
const char* const kCellSteps[7] = {"", "8x16", "9x18", "10x20", "12x24", "14x28", "16x32"};
const char* const kProtoSteps[4] = {"auto", "kitty", "sixel", "off"};
template <class T, size_t N> int index_of(const T (&arr)[N], const std::string& v, int def) {
    for (size_t i = 0; i < N; ++i) if (v == arr[i]) return static_cast<int>(i);
    return def;
}
int step_in(int idx, int n, int dir) { return std::clamp(idx + (dir > 0 ? 1 : -1), 0, n - 1); }
}

const char* osci_res_name(int scale) { return scale >= 3 ? "third" : scale == 2 ? "half" : "full"; }
int osci_res_parse(const std::string& v, int def) {
    std::string l = v;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "full" || l == "1") return 1;
    if (l == "half" || l == "2") return 2;
    if (l == "third" || l == "3") return 3;
    return def;
}

std::vector<int> osci_visible_rows(const VizPrefs& s) {
    std::vector<int> r = {kOrMode, kOrStyle, kOrFps, kOrMusic};   // the picture's own settings (style, frame rate) come right below the scope mode
    {   // the beam: CRT or classic in both modes; low-rate blanking only matters in music mode, rough mode only without it
        const OsciSet& o = s.osci();
        r.push_back(kOrCrt);
        if (o.crt) r.push_back(kOrDwell);
        if (o.crt && o.music) r.push_back(kOrLowRate);
        if (!o.music) r.push_back(kOrRough);
    }
    if (s.osci_style == 1) { r.push_back(kOrProtocol); r.push_back(kOrRes); r.push_back(kOrDetail); }
    r.push_back(kOrDecay);
    if (s.osci_style != 1) r.push_back(kOrDot);
    r.push_back(kOrTail);
    for (int id : {kOrInterp, kOrZ, kOrZDepth, kOrZSource, kOrTrace, kOrRotate, kOrMono, kOrPalette}) r.push_back(id);
    if (s.osci_style == 1) r.push_back(kOrGlow);
    return r;
}

std::string osci_row_label(int row) {
    static const char* const L[kOsciRowCount] = {"Decay", "Dot threshold", "Tail", "Line/Vec. Interpol.", "Z-Axis (XYZ Mode)",
        "Z Depth", "Z Source", "Trace Length", "Rotate 45 deg (M/S)", "Mono Phase Portrait", "Color", "Glow",
        "Osci style", "Osci cell pixels", "Image protocol", "Frame rate", "Display", "Image resolution", "Osci music mode", "Scope mode", "Image detail", "Beam", "Low-rate blanking", "Dwell brightness", "Rough mode"};
    return row >= 0 && row < kOsciRowCount ? L[row] : "";
}

std::string osci_row_value(const VizPrefs& s, int row) {
    char b[24];
    const OsciSet& o = s.osci();
    auto on = [](bool v) { return std::string(v ? "on" : "off"); };
    auto f2 = [&](double v) { std::snprintf(b, sizeof b, "%.2f", v); return std::string(b); };
    switch (row) {
        case kOrDecay: return f2(o.decay);
        case kOrDot: return f2(o.dot_threshold);
        case kOrTail: return f2(o.tail);
        case kOrInterp: return on(o.interp);
        case kOrZ: return on(o.z);
        case kOrZDepth: return f2(o.z_depth);
        case kOrZSource: return o.z_source == 1 ? "level" : "speed";
        case kOrTrace: return std::to_string(o.trace);
        case kOrRotate: return on(o.rotate);
        case kOrMono: return o.music ? std::string("off (music mode)") : on(o.mono_phase);
        case kOrPalette: return kOsciPaletteNames[std::clamp(o.palette, 0, kOsciPaletteCount - 1)];
        case kOrGlow: return f2(o.glow);
        case kOrStyle: return s.osci_style == 1 ? "image" : "braille";
        case kOrCells: return s.cell_pixels.empty() ? "auto" : s.cell_pixels;
        case kOrProtocol: return s.gfx_protocol;
        case kOrFps: return std::to_string(o.frame_rate) + " fps";
        case kOrRes: return osci_res_name(s.image_scale);
        case kOrDetail: return s.image_detail == 1 ? "high (1024 px)" : "normal (560 px)";
        case kOrCrt: return s.osci().crt ? "CRT" : "classic";
        case kOrLowRate: return s.osci().low_rate ? "on (below 96 kHz)" : "off";
        case kOrDwell: return s.osci().dwell ? "on" : "off (strokes equal)";
        case kOrRough: return s.osci().rough ? "on (edge clipping)" : "off";
        case kOrMusic: return on(o.music);
        case kOrMode: return s.osci_mode == 1 ? "YT" : s.osci_mode == 2 ? "XY + YT (overlap)" : s.osci_mode == 3 ? "XY + YT (row)" : s.osci_mode == 4 ? "XY + YT (col)" : "XY";
    }
    return "";
}

void osci_adjust(VizPrefs& s, int row, int dir) {
    OsciSet& o = s.osci();
    auto snap = [](double v, double step, double lo, double hi) { return static_cast<float>(std::clamp(std::round(v / step) * step, lo, hi)); };
    switch (row) {
        case kOrDecay: o.decay = snap(o.decay + dir * 0.01, 0.01, 0.0, 0.99); break;
        case kOrDot: o.dot_threshold = snap(o.dot_threshold + dir * 0.01, 0.01, 0.01, 1.0); break;
        case kOrTail: o.tail = snap(o.tail + dir * 0.01, 0.01, 0.0, 1.0); break;
        case kOrInterp: o.interp = dir > 0; break;
        case kOrZ: o.z = dir > 0; break;
        case kOrZDepth: o.z_depth = snap(o.z_depth + dir * 0.05, 0.05, 0.0, 1.0); break;
        case kOrZSource: o.z_source = dir > 0 ? 1 : 0; break;
        case kOrTrace: o.trace = osci_trace_step(o.trace, dir); break;
        case kOrRotate: o.rotate = dir > 0; break;
        case kOrMono: o.mono_phase = dir > 0; break;
        case kOrPalette: o.palette = step_in(o.palette, kOsciPaletteCount, dir); break;
        case kOrGlow: o.glow = snap(o.glow + dir * 0.05, 0.05, 0.0, 1.0); break;
        case kOrStyle: s.osci_style = dir > 0 ? 1 : 0; break;
        case kOrCells: s.cell_pixels = kCellSteps[step_in(index_of(kCellSteps, s.cell_pixels, 0), 7, dir)]; break;
        case kOrProtocol: s.gfx_protocol = kProtoSteps[step_in(index_of(kProtoSteps, s.gfx_protocol, 0), 4, dir)]; break;
        case kOrMusic: o.music = dir > 0; break;
        case kOrMode: s.osci_mode = step_in(s.osci_mode, 5, dir); break;   // XY <-> YT <-> overlap <-> row <-> col
        case kOrRes: s.image_scale = std::clamp(s.image_scale + (dir > 0 ? -1 : 1), 1, 3); break;   // right = sharper
        case kOrDetail: s.image_detail = dir > 0 ? 1 : 0; break;
        case kOrCrt: s.osci().crt = dir > 0; break;
        case kOrLowRate: s.osci().low_rate = dir > 0; break;
        case kOrDwell: s.osci().dwell = dir > 0; break;
        case kOrRough: s.osci().rough = dir > 0; break;
        case kOrFps: {
            int i = 0;
            for (int k = 0; k < 6; ++k) if (kFpsSteps[k] == o.frame_rate) i = k;
            o.frame_rate = kFpsSteps[step_in(i, 6, dir)];
            break;
        }
    }
}

void osci_reset(VizPrefs& s) {   // the parameters of the style in use; the shared rows (style, protocol, cells) and its fps stay
    const int fps = s.osci().frame_rate;
    s.osci() = s.osci_style == 1 ? OsciSet{} : OsciSet::braille_defaults();
    s.osci().frame_rate = fps;
    if (s.osci_style == 1) s.osci().glow = 0.60f;
}

// The player's "Display" row: what the lyrics area shows (the radio has its own, in radio_settings.cpp).
std::string osci_row_value(const Settings& s, int row) {
    if (row == kOrDisplay) return s.lyric_viz == 1 ? "osci" : s.lyric_viz == 2 ? "spectro" : "sphere";
    return osci_row_value(static_cast<const VizPrefs&>(s), row);
}

void osci_adjust(Settings& s, int row, int dir) {
    if (row == kOrDisplay) { s.lyric_viz = s.viz_step(s.lyric_viz, dir > 0 ? 1 : -1, true); return; }   // sphere -> osci -> spectro
    osci_adjust(static_cast<VizPrefs&>(s), row, dir);
}

namespace {
bool as_bool(const std::string& v, bool def) {
    const std::string l = ascii_lower_str(v);
    if (l == "true" || l == "1" || l == "yes" || l == "on") return true;
    if (l == "false" || l == "0" || l == "no" || l == "off") return false;
    return def;
}
float as_float(const std::string& v, float def, float lo, float hi) {
    try { return std::clamp(std::stof(v), lo, hi); } catch (...) { return def; }
}
int as_int(const std::string& v, int def, int lo, int hi) {
    try { return std::clamp(std::stoi(v), lo, hi); } catch (...) { return def; }
}
int fps_snap(int fr) { return fr >= 143 ? 165 : fr >= 105 ? 120 : fr >= 75 ? 90 : fr >= 52 ? 60 : fr >= 38 ? 45 : 30; }
}

bool osci_config_key(VizPrefs& s, const std::string& key_in, const std::string& value) {
    const std::string key = ascii_lower_str(key_in);   // the radio's config reader hands over lower-case keys
    // Per style: braille = Osci<name> (the keys the player always had), image = OsciImage<name>.
    for (int st = 0; st < 2; ++st) {
        const std::string P = st == 0 ? "osci" : "osciimage";
        if (key.compare(0, P.size(), P) != 0) continue;
        const std::string n = key.substr(P.size());
        OsciSet& o = s.osci_set[st];
        if (n == "decay") o.decay = as_float(value, o.decay, 0.0f, 0.99f);
        else if (n == "dotthreshold") o.dot_threshold = as_float(value, o.dot_threshold, 0.01f, 1.0f);
        else if (n == "tailbrightness") o.tail = as_float(value, o.tail, 0.0f, 1.0f);
        else if (n == "interpolation") o.interp = as_bool(value, o.interp);
        else if (n == "zaxis") o.z = as_bool(value, o.z);
        else if (n == "zdepth") o.z_depth = as_float(value, o.z_depth, 0.0f, 1.0f);
        else if (n == "zsource") o.z_source = ascii_lower_str(value) == "level" ? 1 : 0;
        else if (n == "tracelength") o.trace = as_int(value, o.trace, 128, 8192);
        else if (n == "rotate") o.rotate = as_bool(value, o.rotate);
        else if (n == "monophase") o.mono_phase = as_bool(value, o.mono_phase);
        else if (n == "glow") o.glow = as_float(value, o.glow, 0.0f, 1.0f);
        else if (n == "palette") { const std::string l = ascii_lower_str(value); for (int i = 0; i < kOsciPaletteCount; ++i) if (l == kOsciPaletteNames[i] || (i == 8 && (l == "green" || l == "classic"))) o.palette = i; }
        // frame rate and music mode per style; the braille line (written first) also sets the image style, so a config
        // from before (one value for both) gives both styles that value -- the image line that follows overrides it
        else if (n == "framerate") { const int fr = fps_snap(as_int(value, o.frame_rate, 30, 165)); o.frame_rate = fr; if (st == 0) s.osci_set[1].frame_rate = fr; }
        else if (n == "crtbeam") o.crt = as_bool(value, o.crt);
        else if (n == "lowrateblanking") o.low_rate = as_bool(value, o.low_rate);
        else if (n == "dwellbrightness") o.dwell = as_bool(value, o.dwell);
        else if (n == "roughmode") o.rough = as_bool(value, o.rough);
        else if (n == "musicmode") { o.music = as_bool(value, o.music); if (st == 0) s.osci_set[1].music = o.music; }
        else continue;
        return true;
    }
    if (key == "oscimode") { const std::string l = ascii_lower_str(value); s.osci_mode = l == "yt" ? 1 : (l == "overlap" || l == "both" || l == "xy+yt") ? 2 : (l == "row" || l == "rows") ? 3 : (l == "col" || l == "cols" || l == "column" || l == "columns") ? 4 : 0; return true; }
    if (key == "scopegrid") { s.scope_grid = as_bool(value, s.scope_grid); return true; }
    if (key == "scopevalues") { s.scope_values = as_bool(value, s.scope_values); return true; }
    if (key == "scopegridcolor") { s.scope_grid_color = value; return true; }
    if (scope_metric_parse(s.scope_metrics, key_in, value)) return true;
    if (key == "oscistyle") { s.osci_style = ascii_lower_str(value) == "image" ? 1 : 0; return true; }
    if (key == "osciimageprotocol") { s.gfx_protocol = ascii_lower_str(value); return true; }
    if (key == "osciimageresolution") { s.image_scale = osci_res_parse(value, s.image_scale); return true; }
    if (key == "osciimagedetail") { s.image_detail = ascii_lower_str(value) == "high" ? 1 : 0; return true; }
    if (key == "framerate") {   // up to v3.0.0 one value for both styles
        const int fr = fps_snap(as_int(value, 30, 30, 165));
        s.osci_set[0].frame_rate = s.osci_set[1].frame_rate = fr;
        return true;
    }
    return false;
}

void osci_config_write(std::ostream& out, const VizPrefs& s, const char* c) {
    auto num = [](float v) { std::ostringstream os; os.precision(2); os << std::fixed << v; return os.str(); };
    auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    auto block = [&](const std::string& P, const OsciSet& o) {
        out << P << "Decay=" << num(o.decay) << "\n" << P << "DotThreshold=" << num(o.dot_threshold) << "\n"
            << P << "TailBrightness=" << num(o.tail) << "\n" << P << "Interpolation=" << b(o.interp) << "\n"
            << P << "ZAxis=" << b(o.z) << "\n" << P << "ZDepth=" << num(o.z_depth) << "\n"
            << P << "ZSource=" << (o.z_source == 1 ? "level" : "speed") << "\n" << P << "TraceLength=" << o.trace << "\n"
            << P << "Rotate=" << b(o.rotate) << "\n" << P << "MonoPhase=" << b(o.mono_phase) << "\n"
            << P << "Palette=" << kOsciPaletteNames[std::clamp(o.palette, 0, kOsciPaletteCount - 1)] << "\n"
            << P << "Glow=" << num(o.glow) << "\n" << P << "FrameRate=" << o.frame_rate << "\n"
            << P << "MusicMode=" << b(o.music) << "\n" << P << "CrtBeam=" << b(o.crt) << "\n"
            << P << "LowRateBlanking=" << b(o.low_rate) << "\n" << P << "DwellBrightness=" << b(o.dwell) << "\n" << P << "RoughMode=" << b(o.rough) << "\n";
    };
    out << c << " Oscilloscope (SHIFT+o overlay), one block per style: braille = Osci<name>, image = OsciImage<name>. Each style keeps its own values.\n";
    out << c << " Decay = afterglow 0.00-0.99 | DotThreshold 0.01-1.00 (braille: lower = thicker line) | TailBrightness 0.00-1.00 | Interpolation = connect the\n";
    out << c << " samples with lines | ZAxis = beam intensity (ZDepth 0-1, ZSource speed|level) | TraceLength 128-8192 samples | Rotate = 45 degrees (mid vertical) |\n";
    out << c << " MonoPhase = phase portrait for near-mono signals | Palette gradient|settings|temperature|aurora|magma|ice|neon|spectrum | Glow 0-1 (image bloom) |\n";
    out << c << " FrameRate = screen refresh while that style is in use: 30 | 45 | 60 | 90 | 120 | 165 | MusicMode = oscilloscope music mode (the scopes\n";
    out << c << " get the decoded signal itself, no EQ / normalization / volume, at a fixed scale and without the phase portrait; the XY beam\n";
    out << c << " adds light like a CRT -- Decay about 0.10 gives the look of osci-render)\n";
    out << c << " CrtBeam = true: the CRT beam | false: the classic look (both with and without music mode) | LowRateBlanking (music mode, CRT beam) = fade out the\n";
    out << c << " jumps between shapes in files below 96 kHz | DwellBrightness (CRT beam) = true: slow strokes bright, jumps dark |\n";
    out << c << " false: every stroke equally bright (the jump lines show) | RoughMode (not music mode) = size from the previous frame's peak, loud beats clip along the edges\n";
    block("Osci", s.osci_set[0]);
    block("OsciImage", s.osci_set[1]);
    out << c << " The big oscilloscope overlay (SHIFT+0): ScopeGrid = axes / grid over both pictures (SHIFT+a in the overlay) | ScopeGridColor = its colour,\n";
    out << c << " 0-255 (SHIFT+c in the overlay); empty = the colour of inactive list elements.\n";
    out << c << " The pictures follow OsciStyle: braille dots, or (image) two pixel pictures on a Kitty / Sixel terminal.\n";
    out << "ScopeGrid=" << b(s.scope_grid) << "\n";
    out << "ScopeValues=" << b(s.scope_values) << "\n";
    out << "ScopeGridColor=" << s.scope_grid_color << "\n";
    scope_metric_config_write(out, s.scope_metrics, c);
    out << c << " braille | image (a real pixel picture drawn by the terminal: Kitty graphics or Sixel; braille where there is neither)\n";
    out << c << " xy (left = horizontal, right = vertical) | yt (amplitude over time, triggered like a normal scope) | overlap (YT drawn over the XY figure) | row (XY in the top half, YT in the bottom half)\n";
    out << "OsciMode=" << (s.osci_mode == 1 ? "yt" : s.osci_mode == 2 ? "overlap" : s.osci_mode == 3 ? "row" : s.osci_mode == 4 ? "col" : "xy") << "\n";
    out << "OsciStyle=" << (s.osci_style == 1 ? "image" : "braille") << "\n";
    out << c << " image style: auto | kitty | sixel | off, and the pixels of one terminal cell (WxH, empty = ask the terminal)\n";
    out << "OsciImageProtocol=" << s.gfx_protocol << "\n";
    out << c << " image style: full | half | third of the screen resolution (lower = less work and data per frame, a softer and wider beam)\n";
    out << "OsciImageResolution=" << osci_res_name(s.image_scale) << "\n";
    out << c << " image style: normal | high -- the widest picture rendered (560 / 1024 px): high = sharper thin lines, more data per frame\n";
    out << "OsciImageDetail=" << (s.image_detail == 1 ? "high" : "normal") << "\n";

}

} // namespace muisc

namespace muisc {

bool spectro_profile_key(VizPrefs& s, const std::string& key, const std::string& value) {
    std::string lk = key;
    for (char& ch : lk) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lk == "spectrolaststyle0" || lk == "spectrolaststyle1") {   // the style each view was last shown in
        const int v = lk.back() - '0';
        s.spectro_view_style[v] = value == "0" ? 0 : value == "1" ? 1 : -1;
        return true;
    }
    static const std::string kHead = "spectroprofile";
    if (lk.compare(0, kHead.size(), kHead) != 0 || lk.size() <= kHead.size()) return false;
    const int n = lk[kHead.size()] - '0';
    if (n < 0 || n > 3) return true;
    spectro_config_key(s.spectro_slot[n], "spectro" + lk.substr(kHead.size() + 1), value);
    s.spectro_have[n] = true;
    return true;
}

void spectro_profiles_write(std::ostream& out, const VizPrefs& s, const char* c) {
    const int act = VizPrefs::spectro_slot_of(s.spectro);
    bool head = false;
    for (int n = 0; n < 4; ++n) {
        if (n == act || !s.spectro_have[n]) continue;
        if (!head) {
            out << c << " Spectrogram and waterfall, image and braille each keep their own values (the block above is the look in use). SpectroProfile<n>:\n"
                << c << " n = 2 * view + style, view 0 = spectrogram / 1 = waterfall, style 0 = braille / 1 = image\n";
            head = true;
        }
        SpectroSettings sp = s.spectro_slot[n];
        sp.view = n / 2;
        sp.style = n % 2;
        spectro_config_write(out, sp, c, ("SpectroProfile" + std::to_string(n)).c_str());
    }
    out << c << " The style (0 braille / 1 image) the spectrogram (view 0) and the waterfall (view 1) were last shown in:\n";
    for (int v = 0; v < 2; ++v) {
        const int ls = v == act / 2 ? act % 2 : s.spectro_view_style[v];
        if (ls >= 0) out << "SpectroLastStyle" << v << "=" << ls << "\n";
    }
}

void spectro_profiles_finish(VizPrefs& s) {
    for (int n = 0; n < 4; ++n)
        if (s.spectro_have[n]) { s.spectro_slot[n].view = n / 2; s.spectro_slot[n].style = n % 2; }
    s.spectro_active = -1;
    {   // (SpectroLastStyle<v> was read before the values in use were known: the view in use shows its own style)
        const int keep[2] = {s.spectro_view_style[0], s.spectro_view_style[1]};
        s.spectro_sync();
        for (int v = 0; v < 2; ++v) if (v != s.spectro.view) s.spectro_view_style[v] = keep[v];
    }
}

} // namespace muisc
