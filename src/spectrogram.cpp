// Spectrogram (see spectrogram.h): analysis like Audacity's spectrogram view, braille and picture output.
#include "spectrogram.h"
#include "utf8_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ostream>
#include <sstream>

#include "kiss_fftr.h"
#include "miniz.h"

namespace muisc {

namespace {

// ---- colour maps ------------------------------------------------------------------------------------------------------
// "Color (Roseus)", Audacity's default spectrogram colours (https://github.com/dofuuz/roseus, CC0), 256 steps.
const uint8_t kRoseus[256][3] = {
    {1,1,1}, {1,2,2}, {2,2,2}, {2,3,3}, {2,3,4}, {2,4,5}, {2,5,6}, {3,6,7},
    {3,7,8}, {3,8,10}, {3,9,12}, {3,10,14}, {3,12,16}, {3,13,17}, {3,14,19}, {2,15,21},
    {2,16,23}, {2,17,25}, {2,18,27}, {2,19,30}, {1,20,32}, {1,21,34}, {1,22,36}, {1,23,38},
    {1,24,40}, {0,25,43}, {0,26,45}, {0,27,47}, {0,27,50}, {0,28,52}, {0,29,54}, {0,30,57},
    {0,30,59}, {1,31,62}, {1,32,64}, {1,32,67}, {2,33,69}, {3,33,72}, {4,34,74}, {5,35,77},
    {6,35,79}, {8,35,82}, {9,36,84}, {11,36,86}, {13,37,89}, {15,37,91}, {17,37,94}, {19,37,96},
    {21,38,99}, {23,38,101}, {25,38,104}, {27,38,106}, {29,38,108}, {31,38,111}, {33,38,113}, {35,38,115},
    {38,38,118}, {40,38,120}, {42,38,122}, {44,38,124}, {46,38,126}, {49,38,128}, {51,38,130}, {53,37,132},
    {55,37,134}, {58,37,136}, {60,36,138}, {62,36,139}, {65,36,141}, {67,35,143}, {69,35,144}, {72,35,146},
    {74,34,147}, {76,34,149}, {79,33,150}, {81,33,151}, {84,32,152}, {86,32,153}, {88,31,154}, {91,31,155},
    {93,30,156}, {95,29,157}, {98,29,158}, {100,28,159}, {103,28,159}, {105,27,160}, {107,27,160}, {110,26,161},
    {112,26,161}, {114,25,161}, {117,25,162}, {119,24,162}, {121,24,162}, {124,23,162}, {126,23,162}, {128,23,162},
    {131,22,161}, {133,22,161}, {135,22,161}, {137,22,161}, {140,22,160}, {142,22,160}, {144,22,159}, {146,22,159},
    {148,22,158}, {150,22,157}, {153,22,157}, {155,23,156}, {157,23,155}, {159,23,154}, {161,24,153}, {163,24,152},
    {165,25,151}, {167,26,150}, {169,26,149}, {171,27,148}, {173,28,147}, {175,29,146}, {177,29,145}, {179,30,144},
    {181,31,142}, {183,32,141}, {184,33,140}, {186,34,139}, {188,35,137}, {190,36,136}, {192,37,135}, {193,39,133},
    {195,40,132}, {197,41,130}, {198,42,129}, {200,43,128}, {202,45,126}, {203,46,125}, {205,47,123}, {206,48,122},
    {208,50,120}, {209,51,119}, {211,52,117}, {212,54,116}, {214,55,114}, {215,57,113}, {217,58,111}, {218,60,110},
    {219,61,109}, {221,63,107}, {222,64,106}, {223,66,104}, {225,67,103}, {226,69,101}, {227,70,100}, {228,72,99},
    {229,73,97}, {230,75,96}, {231,77,94}, {233,78,93}, {234,80,92}, {235,82,91}, {236,83,89}, {237,85,88},
    {237,87,87}, {238,89,86}, {239,90,84}, {240,92,83}, {241,94,82}, {242,96,81}, {242,97,80}, {243,99,79},
    {244,101,78}, {245,103,77}, {245,105,76}, {246,107,75}, {246,108,74}, {247,110,74}, {248,112,73}, {248,114,72},
    {248,116,72}, {249,118,71}, {249,120,71}, {250,122,70}, {250,124,70}, {250,126,70}, {251,128,70}, {251,130,69},
    {251,132,70}, {251,134,70}, {251,136,70}, {252,138,70}, {252,140,70}, {252,142,71}, {252,144,72}, {252,146,72},
    {252,148,73}, {252,150,74}, {251,152,75}, {251,154,76}, {251,156,77}, {251,158,78}, {251,160,80}, {251,162,81},
    {250,164,83}, {250,166,85}, {250,168,87}, {249,170,88}, {249,172,90}, {248,174,93}, {248,176,95}, {248,178,97},
    {247,180,99}, {247,182,102}, {246,184,104}, {246,186,107}, {245,188,110}, {244,190,112}, {244,192,115}, {243,194,118},
    {243,195,121}, {242,197,124}, {242,199,127}, {241,201,131}, {240,203,134}, {240,205,137}, {239,207,140}, {239,208,144},
    {238,210,147}, {238,212,151}, {237,213,154}, {237,215,158}, {236,217,161}, {236,218,165}, {236,220,169}, {236,222,172},
    {235,223,176}, {235,225,180}, {235,226,183}, {235,228,187}, {235,229,191}, {235,230,194}, {236,232,198}, {236,233,201},
    {236,234,205}, {237,236,208}, {237,237,212}, {238,238,215}, {239,239,219}, {240,240,222}, {241,242,225}, {242,243,228},
    {243,244,231}, {244,245,234}, {246,246,237}, {247,247,240}, {249,248,242}, {251,249,245}, {253,250,247}, {254,251,249},
};

std::array<std::array<uint8_t, 3>, 256> make_palette(int scheme) {
    std::array<std::array<uint8_t, 3>, 256> p{};
    for (int i = 0; i < 256; ++i) {
        const float v = static_cast<float>(i) / 255.0f;
        float r, g, b;
        if (scheme == 1) {   // Audacity's "Color (classic)" steps -- light blue, violet, red, white -- but from black, not light grey
            static const float st[5][3] = {{0.00f, 0.00f, 0.00f}, {0.30f, 0.60f, 1.00f}, {0.90f, 0.10f, 0.90f}, {1.00f, 0.00f, 0.00f}, {1.00f, 1.00f, 1.00f}};
            const float f = v * 4.0f;
            const int k = std::min(3, static_cast<int>(f));
            const float u = f - static_cast<float>(k);
            r = st[k][0] + (st[k + 1][0] - st[k][0]) * u;
            g = st[k][1] + (st[k + 1][1] - st[k][1]) * u;
            b = st[k][2] + (st[k + 1][2] - st[k][2]) * u;
        } else if (scheme == 2) {   // grayscale: black (quiet) to white (loud), Audacity's "Inverse grayscale"
            r = g = b = v;
        } else if (scheme == 3) {   // gradient: black, then the two colours set with spectro_set_gradient() (filled in there)
            r = g = b = v;
        } else {
            p[static_cast<size_t>(i)] = {kRoseus[i][0], kRoseus[i][1], kRoseus[i][2]};
            continue;
        }
        p[static_cast<size_t>(i)] = {static_cast<uint8_t>(std::lround(std::clamp(r, 0.0f, 1.0f) * 255.0f)),
                                     static_cast<uint8_t>(std::lround(std::clamp(g, 0.0f, 1.0f) * 255.0f)),
                                     static_cast<uint8_t>(std::lround(std::clamp(b, 0.0f, 1.0f) * 255.0f))};
    }
    return p;
}

// ---- frequency scales (Audacity's NumberScale) -----------------------------------------------------------------------
double scale_value(int scale, double hz) {
    switch (scale) {
        case 0: return hz;
        case 1: return std::log(std::max(1.0, hz));
        case 3: {   // Bark (Traunmueller)
            double z = 26.81 * hz / (1960.0 + hz) - 0.53;
            if (z < 2.0) z += 0.15 * (2.0 - z);
            else if (z > 20.1) z += 0.22 * (z - 20.1);
            return z;
        }
        case 4: return 11.17268 * std::log(1.0 + (46.06538 * hz) / (hz + 14678.49));   // ERB
        case 5: return -1.0 / std::max(1.0, hz);                                          // period
        default: return 1127.0 * std::log(1.0 + hz / 700.0);                              // Mel
    }
}

void freq_bounds(const SpectroSettings& s, int rate, double& lo, double& hi) {
    hi = std::min<double>(std::max(100, s.max_freq), rate / 2.0);
    lo = std::clamp<double>(s.min_freq, 0.0, hi - 10.0);
    if ((s.scale == 1 || s.scale == 5) && lo < 1.0) lo = 1.0;
}

const int kPads[4] = {1, 2, 4, 8};
const int kMinFreqs[9] = {0, 20, 50, 100, 200, 300, 500, 1000, 2000};
const int kMaxFreqs[10] = {1000, 2000, 4000, 5000, 8000, 10000, 12000, 16000, 20000, 24000};
const int kWfSpacings[14] = {20, 30, 40, 50, 60, 80, 100, 120, 150, 200, 250, 300, 400, 500};
const char* const kScaleNames[6] = {"linear", "logarithmic", "mel", "bark", "erb", "period"};
const char* const kSchemeNames[4] = {"roseus", "classic", "grayscale", "gradient"};

template <size_t N> int step_list(const int (&list)[N], int cur, int dir) {
    int i = 0;
    for (size_t k = 0; k < N; ++k) if (list[k] <= cur) i = static_cast<int>(k);
    i = std::clamp(i + (dir > 0 ? 1 : -1), 0, static_cast<int>(N) - 1);
    return list[i];
}
int to_int(const std::string& v, int def) { char* e = nullptr; const long x = std::strtol(v.c_str(), &e, 10); return e && e != v.c_str() ? static_cast<int>(x) : def; }

// dB values are kept as 8 bit: -140 .. +10 dB
inline uint8_t q_db(float db) { return static_cast<uint8_t>(std::clamp(std::lround((db + 140.0f) * (255.0f / 150.0f)), 0L, 255L)); }
inline float dq_db(uint8_t q) { return static_cast<float>(q) * (150.0f / 255.0f) - 140.0f; }

} // namespace

// ---- settings ------------------------------------------------------------------------------------------------------
const char* spectro_row_label(int row) {
    static const char* const L[kSpectroRowCount] = {"Display", "View", "Style", "Motion", "Scale", "Min frequency", "Max frequency",
        "Gain (dB)", "Range (dB)", "Frequency gain", "Window size", "Zero padding", "Colors", "Channels",
        "Time span", "Frequency labels", "Graphs", "Graph spacing", "Slant", "Peak height", "Resolution"};
    return row >= 0 && row < kSpectroRowCount ? L[row] : "";
}

std::string spectro_row_value(const SpectroSettings& s, int row) {
    switch (row) {
        case kSrView: return s.view == 1 ? "waterfall" : "spectrogram";
        case kSrStyle: return s.style == 1 ? "image" : "braille";
        case kSrMotion: return s.motion == 1 ? "scroll" : "sweep";
        case kSrScale: return kScaleNames[std::clamp(s.scale, 0, 5)];
        case kSrMinFreq: return std::to_string(s.min_freq) + " Hz";
        case kSrMaxFreq: return std::to_string(s.max_freq) + " Hz";
        case kSrGain: return std::to_string(s.gain);
        case kSrRange: return std::to_string(s.range);
        case kSrFreqGain: return std::to_string(s.freq_gain) + " dB/dec";
        case kSrWindow: return std::to_string(1 << std::clamp(s.window_log2, 8, 13));
        case kSrZeroPad: return std::to_string(s.zero_pad);
        case kSrScheme: return kSchemeNames[std::clamp(s.scheme, 0, 3)];
        case kSrChannels: return s.channels == 1 ? "mix" : "left + right";
        case kSrSpan: return std::to_string(s.span) + " s";
        case kSrLabels: return s.labels ? "on" : "off";
        case kSrWfGraphs: return std::to_string(s.wf_graphs);
        case kSrWfSpacing: return std::to_string(s.wf_spacing) + " ms";
        case kSrWfSlant: return std::to_string(s.wf_slant) + " %";
        case kSrWfHeight: return std::to_string(s.wf_height) + " %";
        case kSrWfDetail: return s.wf_detail == 1 ? "high" : "normal";
    }
    return "";
}

void spectro_adjust(SpectroSettings& s, int row, int dir) {
    const int d = dir > 0 ? 1 : -1;
    switch (row) {
        case kSrView: s.view = dir > 0 ? 1 : 0; break;
        case kSrStyle: s.style = dir > 0 ? 1 : 0; break;
        case kSrMotion: s.motion = dir > 0 ? 1 : 0; break;
        case kSrScale: s.scale = std::clamp(s.scale + d, 0, 5); break;
        case kSrMinFreq: s.min_freq = step_list(kMinFreqs, s.min_freq, d); break;
        case kSrMaxFreq: s.max_freq = step_list(kMaxFreqs, s.max_freq, d); break;
        case kSrGain: s.gain = std::clamp(s.gain + d, -20, 100); break;
        case kSrRange: s.range = std::clamp(s.range + 5 * d, 10, 200); break;
        case kSrFreqGain: s.freq_gain = std::clamp(s.freq_gain + d, 0, 60); break;
        case kSrWindow: s.window_log2 = std::clamp(s.window_log2 + d, 8, 13); break;
        case kSrZeroPad: s.zero_pad = step_list(kPads, s.zero_pad, d); break;
        case kSrScheme: s.scheme = std::clamp(s.scheme + d, 0, 3); break;
        case kSrChannels: s.channels = dir > 0 ? 1 : 0; break;
        case kSrSpan: s.span = std::clamp(d > 0 ? (s.span / 5 + 1) * 5 : ((s.span + 4) / 5 - 1) * 5, 5, 60); break;   // steps of 5 s
        case kSrLabels: s.labels = dir > 0; break;
        case kSrWfGraphs: s.wf_graphs = std::clamp(s.wf_graphs + 4 * d, 8, 100); break;
        case kSrWfSpacing: s.wf_spacing = step_list(kWfSpacings, s.wf_spacing, d); break;
        case kSrWfSlant: s.wf_slant = std::clamp(s.wf_slant + 5 * d, 0, 40); break;
        case kSrWfHeight: s.wf_height = std::clamp(s.wf_height + 5 * d, 10, 60); break;
        case kSrWfDetail: s.wf_detail = dir > 0 ? 1 : 0; break;
    }
}

bool spectro_row_visible(const SpectroSettings& s, int row) {
    if (row == kSrDisplay) return false;   // the caller's row, not listed here
    const bool wf = s.view == 1;
    if (row == kSrMotion || row == kSrChannels || row == kSrSpan) return !wf;
    if (row >= kSrWfGraphs) return wf;
    return true;
}

std::vector<int> spectro_visible_rows(const SpectroSettings& s) {
    std::vector<int> r;
    for (int i = kSrView; i < kSpectroRowCount; ++i) if (spectro_row_visible(s, i)) r.push_back(i);
    return r;
}

void spectro_reset(SpectroSettings& s) {
    SpectroSettings d;
    d.style = s.style;
    d.motion = s.motion;
    d.view = s.view;
    s = d;
}

bool spectro_config_key(SpectroSettings& s, const std::string& key, const std::string& value) {
    const std::string lk = ascii_lower_str(key);
    if (lk.compare(0, 7, "spectro") != 0) return false;
    const std::string n = lk.substr(7);
    const std::string v = ascii_lower_str(value);
    auto pick = [&](const char* const* names, int count, int def) {
        for (int i = 0; i < count; ++i) if (v == names[i]) return i;
        return def;
    };
    if (n == "style") s.style = v == "braille" ? 0 : 1;
    else if (n == "motion") s.motion = v == "scroll" ? 1 : 0;
    else if (n == "scale") s.scale = pick(kScaleNames, 6, s.scale);
    else if (n == "minfreq") s.min_freq = std::clamp(to_int(v, s.min_freq), 0, 20000);
    else if (n == "maxfreq") s.max_freq = std::clamp(to_int(v, s.max_freq), 100, 96000);
    else if (n == "gain") s.gain = std::clamp(to_int(v, s.gain), -20, 100);
    else if (n == "range") s.range = std::clamp(to_int(v, s.range), 10, 200);
    else if (n == "frequencygain") s.freq_gain = std::clamp(to_int(v, s.freq_gain), 0, 60);
    else if (n == "windowsize") { const int w = to_int(v, 2048); int l = 8; while (l < 13 && (1 << l) < w) ++l; s.window_log2 = l; }
    else if (n == "windowtype") {}   // no longer an option: always hann
    else if (n == "zeropadding") { const int z = to_int(v, 2); s.zero_pad = z >= 8 ? 8 : z >= 4 ? 4 : z >= 2 ? 2 : 1; }
    else if (n == "colors") s.scheme = v == "inverse grayscale" ? 2 : pick(kSchemeNames, 4, s.scheme);
    else if (n == "channels") s.channels = v == "mix" ? 1 : 0;
    else if (n == "timespan") s.span = std::clamp((to_int(v, s.span) + 2) / 5 * 5, 5, 60);   // 5 .. 60 in steps of 5
    else if (n == "view") s.view = v == "waterfall" ? 1 : 0;
    else if (n == "waterfallgraphs") s.wf_graphs = std::clamp(to_int(v, s.wf_graphs), 8, 100);
    else if (n == "waterfallspacing") s.wf_spacing = std::clamp(to_int(v, s.wf_spacing), 20, 500);
    else if (n == "waterfallslant") s.wf_slant = std::clamp(to_int(v, s.wf_slant), 0, 40);
    else if (n == "waterfallheight") s.wf_height = std::clamp(to_int(v, s.wf_height), 10, 60);
    else if (n == "waterfallresolution") s.wf_detail = v == "high" ? 1 : 0;
    else if (n == "labels") s.labels = !(v == "false" || v == "off" || v == "0");
    else return false;
    return true;
}

void spectro_config_write(std::ostream& out, const SpectroSettings& s, const char* c, const char* p) {
    std::ostringstream o;
    o << c << " Spectrogram (SHIFT+i overlay, SHIFT+u full screen). The defaults are Audacity's: mel scale 0-20000 Hz, gain 20,\n"
        << c << " range 80, window 2048 (hann), zero padding 2, colors roseus. Style braille|image, Motion sweep|scroll,\n"
        << c << " Scale linear|logarithmic|mel|bark|erb|period, Colors roseus|classic|grayscale|gradient, Channels both|mix, TimeSpan 5-60 s (steps of 5)\n";
    o << p << "Style=" << (s.style == 1 ? "image" : "braille") << "\n"
        << p << "Motion=" << (s.motion == 1 ? "scroll" : "sweep") << "\n"
        << p << "Scale=" << kScaleNames[std::clamp(s.scale, 0, 5)] << "\n"
        << p << "MinFreq=" << s.min_freq << "\n"
        << p << "MaxFreq=" << s.max_freq << "\n"
        << p << "Gain=" << s.gain << "\n"
        << p << "Range=" << s.range << "\n"
        << p << "FrequencyGain=" << s.freq_gain << "\n"
        << p << "WindowSize=" << (1 << std::clamp(s.window_log2, 8, 13)) << "\n"
        << p << "ZeroPadding=" << s.zero_pad << "\n"
        << p << "Colors=" << kSchemeNames[std::clamp(s.scheme, 0, 3)] << "\n"
        << p << "Channels=" << (s.channels == 1 ? "mix" : "both") << "\n"
        << p << "TimeSpan=" << s.span << "\n"
        << p << "Labels=" << (s.labels ? "true" : "false") << "\n"
        << c << " View spectrogram|waterfall: the waterfall is a stack of spectra (newest in front, each graph outlined in black). Graphs 8-100,\n"
        << c << " Spacing 20-500 ms between two graphs, Slant 0-40 % of the width, Height 10-60 % of the picture\n"
        << p << "View=" << (s.view == 1 ? "waterfall" : "spectrogram") << "\n"
        << p << "WaterfallGraphs=" << s.wf_graphs << "\n"
        << p << "WaterfallSpacing=" << s.wf_spacing << "\n"
        << p << "WaterfallSlant=" << s.wf_slant << "\n"
        << p << "WaterfallHeight=" << s.wf_height << "\n"
        << c << " WaterfallResolution normal|high: high draws the graphs at twice the resolution and scales them down (finer, shaded outlines; more work)\n"
        << p << "WaterfallResolution=" << (s.wf_detail == 1 ? "high" : "normal") << "\n";
    // a profile's block (SpectroProfile<n>...) carries no comments
    if (std::strcmp(p, "Spectro") == 0) { out << o.str(); return; }
    std::istringstream in(o.str());
    for (std::string line; std::getline(in, line);)
        if (line.compare(0, std::strlen(c), c) != 0) out << line << "\n";
}

namespace {
std::array<std::array<uint8_t, 3>, 256> g_pal[4] = {make_palette(0), make_palette(1), make_palette(2), make_palette(3)};
std::array<uint8_t, 3> g_grad_a{255, 255, 255}, g_grad_b{255, 255, 255};
bool g_grad_set = false;
unsigned g_grad_gen = 0;   // the gradient colours changed: pictures in that scheme are drawn again
}

const std::array<std::array<uint8_t, 3>, 256>& spectro_palette(int scheme) { return g_pal[std::clamp(scheme, 0, 3)]; }

const std::array<std::array<uint8_t, 3>, 256>& spectro_palette_view(const SpectroSettings& s) {
    if (s.view != 1) return spectro_palette(s.scheme);
    // The waterfall: background and outline are black whatever the scheme (entries 0 .. 3 too: Sixel takes the colour of a group of 4
    // entries from its third). The gradient scheme changes, so the copy is made again when it did.
    // "high" resolution: index = register * 4 (64 registers, so it survives Sixel's 64 colours): 0 black, 1 .. 40 the fill by height,
    // 41 .. 63 the fill with a half dark outline over it (a line that only half covers a pixel).
    static std::array<std::array<uint8_t, 3>, 256> wf[2][4];
    static unsigned gen[2][4] = {{~0u, ~0u, ~0u, ~0u}, {~0u, ~0u, ~0u, ~0u}};
    const int k = std::clamp(s.scheme, 0, 3), hi = s.wf_detail == 1 ? 1 : 0;
    if (gen[hi][k] != g_grad_gen) {
        if (!hi) {
            wf[0][k] = g_pal[k];
            for (int i = 0; i < 4; ++i) wf[0][k][static_cast<size_t>(i)] = {0, 0, 0};
        } else {
            for (int i = 0; i < 256; ++i) {
                const int r = i >> 2;
                std::array<uint8_t, 3> c{0, 0, 0};
                if (r >= 1 && r <= 40) c = g_pal[k][static_cast<size_t>(48 + (r - 1) * 207 / 39)];
                else if (r >= 41) {
                    c = g_pal[k][static_cast<size_t>(48 + (r - 41) * 207 / 22)];
                    for (auto& v : c) v = static_cast<uint8_t>(v / 2);
                }
                wf[1][k][static_cast<size_t>(i)] = c;
            }
        }
        gen[hi][k] = g_grad_gen;
    }
    return wf[hi][k];
}

void spectro_set_gradient(const std::array<uint8_t, 3>& from, const std::array<uint8_t, 3>& to) {
    if (g_grad_set && from == g_grad_a && to == g_grad_b) return;
    g_grad_set = true;
    ++g_grad_gen;
    g_grad_a = from;
    g_grad_b = to;
    // black -> the first colour (half way) -> the second colour (loudest)
    for (int i = 0; i < 256; ++i) {
        const float v = static_cast<float>(i) / 255.0f;
        for (int c = 0; c < 3; ++c) {
            const float a = from[static_cast<size_t>(c)], b = to[static_cast<size_t>(c)];
            const float x = v < 0.5f ? a * (v / 0.5f) : a + (b - a) * ((v - 0.5f) / 0.5f);
            g_pal[3][static_cast<size_t>(i)][static_cast<size_t>(c)] = static_cast<uint8_t>(std::lround(std::clamp(x, 0.0f, 255.0f)));
        }
    }
}

double spectro_freq_at(const SpectroSettings& s, int rate, double y01) {
    double lo, hi;
    freq_bounds(s, rate, lo, hi);
    const double a = scale_value(s.scale, lo), b = scale_value(s.scale, hi);
    const double want = a + (b - a) * std::clamp(y01, 0.0, 1.0);
    double f0 = lo, f1 = hi;   // every scale rises with the frequency: bisection
    for (int i = 0; i < 48; ++i) {
        const double m = 0.5 * (f0 + f1);
        if (scale_value(s.scale, m) < want) f0 = m; else f1 = m;
    }
    return 0.5 * (f0 + f1);
}

double spectro_pos_of(const SpectroSettings& s, int rate, double hz) {
    double lo, hi;
    freq_bounds(s, rate, lo, hi);
    const double a = scale_value(s.scale, lo), b = scale_value(s.scale, hi);
    return (scale_value(s.scale, hz) - a) / (b - a);
}

std::vector<std::pair<int, std::string>> spectro_axis(const SpectroSettings& s, int rate, int h_px, int unit_px) {
    std::vector<std::pair<int, std::string>> out;
    const int rows = h_px / std::max(1, unit_px);
    if (rows < 2) return out;
    double lo, hi;
    freq_bounds(s, rate, lo, hi);
    // the same layout as SpectroAnalyzer::render()
    const bool two = s.channels == 0;
    const int ch_h = two ? std::max(1, (h_px - 1) / 2) : h_px;
    const int ch2_y = ch_h + 1, ch2_h = two ? std::max(1, h_px - ch2_y) : 0;
    // round numbers, most important first (decades, then 5 / 2 / 3 ... times a decade), like Audacity's ruler
    std::vector<double> cand;
    const double first = hi >= 2000 ? 100 : 10;   // no "10" squeezed in at the bottom of a wide range
    for (double dec = first; dec <= 100000; dec *= 10) cand.push_back(dec);
    for (int m : {5, 2, 3, 4, 6, 8, 7, 9})
        for (double dec = first; dec <= 100000; dec *= 10) cand.push_back(m * dec);
    std::vector<char> used(static_cast<size_t>(rows), 0);
    for (int c = 0; c < (two ? 2 : 1); ++c) {
        const int top = c == 0 ? 0 : ch2_y, h = c == 0 ? ch_h : ch2_h;
        // the bottom (lo) of each channel first, so 0 / the minimum is there like in Audacity
        for (double f : cand) {
            if (f < lo || f > hi) continue;
            const double p = spectro_pos_of(s, rate, f);
            const int y = top + static_cast<int>(std::lround((1.0 - p) * (h - 1)));
            const int r = y / std::max(1, unit_px);
            if (r < 0 || r >= rows) continue;
            bool free = true;
            for (int k = std::max(0, r - 1); k <= std::min(rows - 1, r + 1); ++k) if (used[static_cast<size_t>(k)]) free = false;
            if (!free) continue;
            used[static_cast<size_t>(r)] = 1;
            out.push_back({r, std::to_string(static_cast<long long>(f))});
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- analyzer --------------------------------------------------------------------------------------------------------
void SpectroAnalyzer::push(const float* lr, size_t frames, int rate) {
    if (!on_.load(std::memory_order_relaxed) || !lr || frames == 0) return;
    if (rate > 0) feed_rate_.store(rate, std::memory_order_relaxed);
    const size_t w = fw_.load(std::memory_order_relaxed);
    const size_t r = fr_.load(std::memory_order_acquire);
    const size_t n = std::min(frames, kFeed - (w - r));
    for (size_t i = 0; i < n; ++i) {
        const size_t at = (w + i) & (kFeed - 1);
        feed_[2 * at] = lr[2 * i];
        feed_[2 * at + 1] = lr[2 * i + 1];
    }
    fw_.store(w + n, std::memory_order_release);
}

void SpectroAnalyzer::set_visible(bool on) {
    if (on && !on_.load()) fr_.store(fw_.load());   // nothing old in the feed
    on_.store(on);
}

void SpectroAnalyzer::reset() {
    ncols_ = 0;
    raw_total_ = 0;
    std::fill(hist_.begin(), hist_.end(), 0);
    std::fill(cache_tag_.begin(), cache_tag_.end(), ~0ull);
    rebuilt_ = true;
    disp_cols_ = 0;
    ++gen_;
}

void SpectroAnalyzer::configure(const SpectroSettings& s, int rate) {
    const int win = 1 << std::clamp(s.window_log2, 8, 13);
    const int fft = std::min(16384, win * std::clamp(s.zero_pad, 1, 8));   // memory: the history keeps fft / 2 bins per column
    const int nch = s.channels == 1 ? 1 : 2;
    Shape sh{rate, fft, win, s.span, nch};
    if (sh.rate == shape_.rate && sh.fft == shape_.fft && sh.win == shape_.win && sh.span == shape_.span && sh.ch == shape_.ch) return;
    const bool new_rate = sh.rate != shape_.rate;
    shape_ = sh;
    rate_ = rate;
    win_ = win;
    fft_ = fft;
    bins_ = fft / 2;
    // Hann (Audacity's default), scaled so a full-scale sine reads 0 dB (2 / sum of the window)
    window_.assign(static_cast<size_t>(win), 1.0f);
    const double pi = 3.14159265358979323846;
    double sum = 0.0;
    for (int i = 0; i < win; ++i) { window_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * i / win)); sum += window_[static_cast<size_t>(i)]; }
    for (float& v : window_) v = static_cast<float>(v * 2.0 / sum);
    if (fft_cfg_) kiss_fftr_free(static_cast<kiss_fftr_cfg>(fft_cfg_));
    fft_cfg_ = kiss_fftr_alloc(fft, 0, nullptr, nullptr);
    fft_in_.assign(static_cast<size_t>(fft), 0.0f);
    fft_out_.assign(static_cast<size_t>(fft + 2), 0.0f);
    // columns: one page (the width of the view) holds cols_per_page_ columns, whatever the time span
    hop_ = std::max(1, static_cast<int>(std::lround(static_cast<double>(rate) * s.span / cols_per_page_)));
    cap_ = static_cast<size_t>(cols_per_page_) * 2 + 64;
    hist_.assign(cap_ * static_cast<size_t>(nch) * static_cast<size_t>(bins_), 0);
    cache_tag_.assign(cap_, ~0ull);
    cache_.clear();
    cache_h_ = 0;
    cache_key_.clear();
    ++gen_;
    rebuilt_ = true;
    if (new_rate || raw_.empty()) {   // another track / station rate: start afresh
        raw_cap_ = static_cast<size_t>(rate) * 61 + 16384;
        raw_.assign(raw_cap_ * 2, 0);
        raw_total_ = 0;
        ncols_ = 0;
        return;
    }
    // same signal, other analysis: compute what is on screen again from the kept samples
    ncols_ = raw_total_ / static_cast<uint64_t>(hop_);
    const uint64_t redo = std::min<uint64_t>(ncols_, static_cast<uint64_t>(cols_per_page_) + 32);
    for (uint64_t c = ncols_ - redo; c < ncols_; ++c) column(c);
}

void SpectroAnalyzer::column(uint64_t c) {
    const int nch = shape_.ch;
    const size_t slot = static_cast<size_t>(c % cap_);
    const int pad = (fft_ - win_) / 2;   // the window sits in the middle of the zero padding, as in Audacity
    const long long end = static_cast<long long>((c + 1) * static_cast<uint64_t>(hop_));
    const long long start = end - win_;
    const long long oldest = static_cast<long long>(raw_total_) - static_cast<long long>(raw_cap_);
    for (int ch = 0; ch < nch; ++ch) {
        std::fill(fft_in_.begin(), fft_in_.end(), 0.0f);
        for (int i = 0; i < win_; ++i) {
            const long long k = start + i;
            if (k < 0 || k < oldest || k >= static_cast<long long>(raw_total_)) continue;
            const size_t at = static_cast<size_t>(static_cast<uint64_t>(k) % raw_cap_);
            const float l = raw_[2 * at] / 32768.0f, r = raw_[2 * at + 1] / 32768.0f;
            const float v = nch == 1 ? 0.5f * (l + r) : (ch == 0 ? l : r);
            fft_in_[static_cast<size_t>(pad + i)] = v * window_[static_cast<size_t>(i)];
        }
        kiss_fftr(static_cast<kiss_fftr_cfg>(fft_cfg_), fft_in_.data(), reinterpret_cast<kiss_fft_cpx*>(fft_out_.data()));
        uint8_t* dst = hist_.data() + (slot * static_cast<size_t>(nch) + static_cast<size_t>(ch)) * static_cast<size_t>(bins_);
        for (int b = 0; b < bins_; ++b) {
            const float re = fft_out_[static_cast<size_t>(2 * b)], im = fft_out_[static_cast<size_t>(2 * b + 1)];
            const float p = re * re + im * im;
            dst[b] = q_db(p > 0.0f ? 10.0f * std::log10(p) : -160.0f);
        }
    }
    cache_tag_[slot] = ~0ull;
}

bool SpectroAnalyzer::update(const SpectroSettings& s_in) {
    set_visible(true);
    SpectroSettings s = s_in;
    if (s.view == 1) {
        // the waterfall looks back graphs * spacing: the history is kept for twice the time span, and one mix of both channels is enough
        const double need = std::clamp(s.wf_graphs, 8, 100) * std::clamp(s.wf_spacing, 20, 500) / 1000.0;
        s.span = std::clamp(static_cast<int>(std::ceil(need / 1.9 / 5.0)) * 5, 5, 60);
        s.channels = 1;
    }
    configure(s, std::max(8000, feed_rate_.load(std::memory_order_relaxed)));
    const size_t w = fw_.load(std::memory_order_acquire);
    size_t r = fr_.load(std::memory_order_relaxed);
    if (w == r) { advance_display(); return false; }
    const uint64_t before = ncols_;
    for (; r != w; ++r) {
        const size_t at = r & (kFeed - 1);
        const size_t pos = static_cast<size_t>(raw_total_ % raw_cap_);
        raw_[2 * pos] = static_cast<int16_t>(std::lround(std::clamp(feed_[2 * at], -1.0f, 1.0f) * 32767.0f));
        raw_[2 * pos + 1] = static_cast<int16_t>(std::lround(std::clamp(feed_[2 * at + 1], -1.0f, 1.0f) * 32767.0f));
        ++raw_total_;
        if (raw_total_ % static_cast<uint64_t>(hop_) == 0) {   // a column is due: its window ends here
            column(ncols_);
            ++ncols_;
        }
    }
    fr_.store(w, std::memory_order_release);
    advance_display();
    return ncols_ != before;
}

// The audio arrives in bursts (one audio buffer at a time), so the column count jumps. The scrolling picture follows a
// clock instead: it moves on at the column rate and is only nudged toward the real count, so its steps come evenly.
void SpectroAnalyzer::advance_display() {
    const auto now = std::chrono::steady_clock::now();
    const double cps = columns_per_second();
    const double dt = disp_t_.time_since_epoch().count() == 0 ? 0.0 : std::chrono::duration<double>(now - disp_t_).count();
    disp_t_ = now;
    const double target = static_cast<double>(ncols_) - cps * 0.08;   // a little behind the newest column
    if (cps <= 0 || dt > 0.5 || std::abs(target - disp_cols_) > cps * 0.5) disp_cols_ = std::max(0.0, target);   // start, seek, pause
    else {
        disp_cols_ += cps * std::min(dt, 0.1);
        disp_cols_ += (target - disp_cols_) * std::min(1.0, dt * 2.0);   // drift toward the real position
    }
    disp_cols_ = std::clamp(disp_cols_, 0.0, static_cast<double>(ncols_));
}

void SpectroAnalyzer::prepare(const SpectroSettings& s, int h) {
    // the bin range of every row and the per-column cache depend on the height and on these settings
    std::ostringstream k;
    k << h << "," << s.scale << "," << s.min_freq << "," << s.max_freq << "," << s.gain << "," << s.range << "," << s.freq_gain << "," << gen_;
    const std::string key = k.str();
    const int nch = shape_.ch;
    if (key != cache_key_) {
        cache_key_ = key;
        cache_h_ = h;
        cache_.assign(cap_ * static_cast<size_t>(nch) * static_cast<size_t>(h), 0);
        std::fill(cache_tag_.begin(), cache_tag_.end(), ~0ull);
        row_lo_.assign(static_cast<size_t>(h), 0);
        row_hi_.assign(static_cast<size_t>(h), 1);
        const double binw = static_cast<double>(rate_) / fft_;
        for (int yy = 0; yy < h; ++yy) {
            // row yy (0 = top) spans heights (h-1-yy)/h .. (h-yy)/h; Audacity's findValue: nearest bins, strongest wins
            const double f0 = spectro_freq_at(s, rate_, static_cast<double>(h - 1 - yy) / h);
            const double f1 = spectro_freq_at(s, rate_, static_cast<double>(h - yy) / h);
            const int i0 = std::min(bins_ - 1, static_cast<int>(std::floor(0.5 + f0 / binw)));
            const int i1 = std::min(bins_, static_cast<int>(std::floor(0.5 + f1 / binw)));
            row_lo_[static_cast<size_t>(yy)] = i0;
            row_hi_[static_cast<size_t>(yy)] = std::max(i0 + 1, i1);
        }
    }
    prep_ = s;
}

int SpectroAnalyzer::value_at(uint64_t col, int ch, int y) {
    const SpectroSettings& s = prep_;
    const int nch = shape_.ch;
    const int h = cache_h_;
    const size_t slot = static_cast<size_t>(col % cap_);
    uint8_t* cached = cache_.data() + slot * static_cast<size_t>(nch) * static_cast<size_t>(h);
    if (cache_tag_[slot] != col) {
        cache_tag_[slot] = col;
        const double fg_step = 0.001 * rate_ / fft_;
        const float gain = static_cast<float>(s.gain), range = static_cast<float>(std::max(1, s.range));
        for (int c = 0; c < nch; ++c) {
            const uint8_t* src = hist_.data() + (slot * static_cast<size_t>(nch) + static_cast<size_t>(c)) * static_cast<size_t>(bins_);
            for (int yy = 0; yy < h; ++yy) {
                float best = -1e9f;
                for (int b = row_lo_[static_cast<size_t>(yy)]; b < row_hi_[static_cast<size_t>(yy)]; ++b) {
                    float db = dq_db(src[b]);
                    if (s.freq_gain != 0 && b > 0) db += static_cast<float>(s.freq_gain * std::log10(fg_step * b));
                    best = std::max(best, db);
                }
                const float v = std::clamp((best + range + gain) / range, 0.0f, 1.0f);
                cached[static_cast<size_t>(c) * static_cast<size_t>(h) + static_cast<size_t>(yy)] = static_cast<uint8_t>(std::lround(v * 255.0f));
            }
        }
    }
    return cached[static_cast<size_t>(std::min(ch, nch - 1)) * static_cast<size_t>(h) + static_cast<size_t>(y)];
}

void SpectroAnalyzer::render(const SpectroSettings& s, int w, int h, int x0, int cols, std::vector<uint8_t>& idx, bool playhead) {
    idx.assign(static_cast<size_t>(std::max(0, cols)) * static_cast<size_t>(std::max(0, h)), 0);
    if (w <= 0 || h <= 0 || cols <= 0 || cap_ == 0) return;
    if (s.view == 1) {   // waterfall: the whole picture is made once, the tiles are slices of it
        std::vector<uint8_t> whole;
        waterfall(s, w, h, false, whole);
        for (int y = 0; y < h; ++y)
            for (int xi = 0; xi < cols; ++xi) {
                const int x = x0 + xi;
                if (x >= 0 && x < w) idx[static_cast<size_t>(y) * static_cast<size_t>(cols) + static_cast<size_t>(xi)] = whole[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
            }
        return;
    }
    const int nch = shape_.ch;
    // stereo: two channels, one dark pixel row between them
    const int ch_h = nch == 2 ? std::max(1, (h - 1) / 2) : h;
    const int ch2_y = ch_h + 1;
    const int ch2_h = nch == 2 ? std::max(1, h - ch2_y) : 0;
    prepare(s, ch_h);
    const uint64_t cpp = static_cast<uint64_t>(cols_per_page_);
    const uint64_t head = ncols_ % cpp, page_start = ncols_ - head;
    const int head_x = static_cast<int>(head * static_cast<uint64_t>(w) / cpp);
    const uint64_t off = scroll_offset(w);
    for (int xi = 0; xi < cols; ++xi) {
        const int x = x0 + xi;
        if (x < 0 || x >= w) continue;
        long long col;
        if (s.motion == 1) {
            // scroll: every pixel belongs to a fixed place in time (absolute pixel P = column * w / cpp), and the picture
            // moves on in whole pixels (scroll_step_ of them at a time). Mapping "x pixels left of the newest column"
            // instead picked a different subset of columns every time one was added: fine lines shimmered (aliasing).
            const long long p = static_cast<long long>(off) - static_cast<long long>(w - x);
            col = p < 0 ? -1 : static_cast<long long>(static_cast<uint64_t>(p) * cpp / static_cast<uint64_t>(w));
        } else {
            const uint64_t k = static_cast<uint64_t>(x) * cpp / static_cast<uint64_t>(w);
            col = k < head ? static_cast<long long>(page_start + k) : static_cast<long long>(page_start) - static_cast<long long>(cpp) + static_cast<long long>(k);
        }
        const bool line = s.motion == 0 && playhead && x == head_x && ncols_ > 0;
        // sweep: a short dark gap ahead of the playhead separates the new page from the old one
        const int gap = std::max(2, w / 80);
        if (s.motion == 0 && ncols_ > 0 && x > head_x && x <= head_x + gap) continue;
        if (line) {
            for (int y = 0; y < h; ++y) idx[static_cast<size_t>(y) * static_cast<size_t>(cols) + static_cast<size_t>(xi)] = 200;
            continue;
        }
        if (col < 0 || ncols_ - static_cast<uint64_t>(col) > cap_) continue;
        for (int y = 0; y < h; ++y) {
            int v = 0;
            if (nch == 1) v = value_at(static_cast<uint64_t>(col), 0, y);
            else if (y < ch_h) v = value_at(static_cast<uint64_t>(col), 0, y);
            else if (y >= ch2_y) v = value_at(static_cast<uint64_t>(col), 1, std::min(ch_h - 1, (y - ch2_y) * ch_h / ch2_h));
            else continue;   // the line between the channels
            idx[static_cast<size_t>(y) * static_cast<size_t>(cols) + static_cast<size_t>(xi)] = static_cast<uint8_t>(v);
        }
    }
}

void SpectroAnalyzer::column_images(const SpectroSettings& s, uint64_t c0, uint64_t c1, int h, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(c1 > c0 ? c1 - c0 : 0) * static_cast<size_t>(std::max(0, h)), 0);
    if (h <= 0 || c1 <= c0 || cap_ == 0) return;
    const int nch = shape_.ch;
    const int ch_h = nch == 2 ? std::max(1, (h - 1) / 2) : h;
    const int ch2_y = ch_h + 1;
    const int ch2_h = nch == 2 ? std::max(1, h - ch2_y) : 0;
    prepare(s, ch_h);
    for (uint64_t c = c0; c < c1; ++c) {
        if (c >= ncols_ || ncols_ - c > cap_) continue;   // not made yet / no longer kept: black
        uint8_t* o = out.data() + static_cast<size_t>(c - c0) * static_cast<size_t>(h);
        for (int y = 0; y < h; ++y) {
            if (nch == 1 || y < ch_h) o[y] = static_cast<uint8_t>(value_at(c, 0, y));
            else if (y >= ch2_y) o[y] = static_cast<uint8_t>(value_at(c, 1, std::min(ch_h - 1, (y - ch2_y) * ch_h / ch2_h)));
        }
    }
}

std::vector<std::string> SpectroAnalyzer::braille(const SpectroSettings& s, int cols, int rows) {
    std::vector<std::string> out(static_cast<size_t>(std::max(0, rows)));
    if (cols <= 0 || rows <= 0) return out;
    const int W = cols * 2, H = rows * 4;
    std::vector<uint8_t> v;
    const int step = scroll_step_;
    scroll_step_ = 1;   // braille is small and cheap: every pixel step
    if (s.view == 1) waterfall(s, W, H, true, v);   // outlines only: a lit dot is a line, the black outline of the picture has no braille
    else render(s, W, H, 0, W, v, true);
    scroll_step_ = step;
    const auto& pal = spectro_palette(s.scheme);
    const bool wf = s.view == 1;
    // dot order inside a braille cell: bit of (row, column)
    static const int kBit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    // ordered dither: louder = more dots
    static const int kThr[4][2] = {{70, 178}, {142, 106}, {88, 196}, {160, 124}};
    for (int r = 0; r < rows; ++r) {
        std::string line, last;
        for (int c = 0; c < cols; ++c) {
            int bits = 0, mx = 0;
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int val = v[static_cast<size_t>(r * 4 + dy) * static_cast<size_t>(W) + static_cast<size_t>(c * 2 + dx)];
                    mx = std::max(mx, val);
                    if (wf ? val > 0 : val > kThr[dy][dx]) bits |= kBit[dy][dx];
                }
            if (bits == 0) { line += ' '; continue; }
            int sum = 0;
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 2; ++dx) sum += v[static_cast<size_t>(r * 4 + dy) * static_cast<size_t>(W) + static_cast<size_t>(c * 2 + dx)];
            const auto& p = pal[static_cast<size_t>(wf ? mx : (mx + sum / 8) / 2)];   // between the loudest and the average dot (waterfall: the line's own colour)
            const std::string a = "\x1b[38;2;" + std::to_string(p[0]) + ";" + std::to_string(p[1]) + ";" + std::to_string(p[2]) + "m";
            if (a != last) { line += a; last = a; }
            const int cp = 0x2800 + bits;
            line += static_cast<char>(0xE0 | (cp >> 12));
            line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            line += static_cast<char>(0x80 | (cp & 0x3F));
        }
        out[static_cast<size_t>(r)] = line + "\x1b[0m";
    }
    return out;
}

uint64_t SpectroAnalyzer::scroll_offset(int w) const {
    if (w <= 0) return 0;
    const uint64_t px = static_cast<uint64_t>(disp_cols_ * w / cols_per_page_);
    const uint64_t q = static_cast<uint64_t>(std::max(1, scroll_step_));
    return px / q * q;
}

void SpectroAnalyzer::changed_columns(const SpectroSettings& s, int w, int& first, int& last) {
    first = last = 0;
    if (w <= 0) return;
    if (s.view == 1) {   // waterfall: every new column moves the front graph, the others when a step is full: all of it
        const bool changed = w != cw_last_w_ || ncols_ != cw_last_cols_ || rebuilt_;
        cw_last_w_ = w; cw_last_cols_ = ncols_; rebuilt_ = false;
        if (changed) { first = 0; last = w; }
        return;
    }
    if (w != cw_last_w_ || ncols_ < cw_last_cols_ || rebuilt_) {
        cw_last_w_ = w; cw_last_cols_ = ncols_; cw_last_off_ = scroll_offset(w); rebuilt_ = false; first = 0; last = w; return;
    }
    const uint64_t cpp = static_cast<uint64_t>(cols_per_page_);
    if (s.motion != 1 && ncols_ == cw_last_cols_) return;
    if (s.motion == 1) {
        // scroll: only when the picture has moved on by (a step of) whole pixels -- then all of it
        if (scroll_offset(w) == cw_last_off_) return;
        cw_last_off_ = scroll_offset(w);
        first = 0; last = w;
    } else if (ncols_ / cpp != cw_last_cols_ / cpp || ncols_ - cw_last_cols_ >= cpp) { first = 0; last = w; }
    else {
        const int x_old = static_cast<int>((cw_last_cols_ % cpp) * static_cast<uint64_t>(w) / cpp);
        const int x_new = static_cast<int>((ncols_ % cpp) * static_cast<uint64_t>(w) / cpp);
        first = std::max(0, x_old - 1);
        last = std::min(w, x_new + 2 + std::max(2, w / 80));   // the gap ahead of the playhead moves along
    }
    cw_last_cols_ = ncols_;
}

// ---- the waterfall ---------------------------------------------------------------------------------------------------------
int spectro_waterfall_plot_width(const SpectroSettings& s, int w_px) {
    const int shear = static_cast<int>(w_px * std::clamp(s.wf_slant, 0, 40) / 100.0);
    return std::max(8, w_px - shear - 1);
}

std::vector<std::pair<int, std::string>> spectro_axis_x(const SpectroSettings& s, int rate, int w_px, int unit_px) {
    std::vector<std::pair<int, std::string>> out;
    const int cells = w_px / std::max(1, unit_px);
    if (cells < 8) return out;
    double lo, hi;
    freq_bounds(s, rate, lo, hi);
    const int fw = spectro_waterfall_plot_width(s, w_px);
    std::vector<double> cand;
    const double first = hi >= 2000 ? 100 : 10;
    for (double dec = first; dec <= 100000; dec *= 10) cand.push_back(dec);
    for (int m : {5, 2, 3, 4, 6, 8, 7, 9})
        for (double dec = first; dec <= 100000; dec *= 10) cand.push_back(m * dec);
    std::vector<char> used(static_cast<size_t>(cells) + 8, 0);
    for (double f : cand) {
        if (f < lo || f > hi) continue;
        const int x = static_cast<int>(std::lround(spectro_pos_of(s, rate, f) * (fw - 1)));
        const int c = x / std::max(1, unit_px);
        const std::string t = f >= 1000 && std::fmod(f, 1000.0) == 0.0 ? std::to_string(static_cast<long long>(f / 1000)) + "k" : std::to_string(static_cast<long long>(f));
        const int tw = static_cast<int>(t.size());
        if (c < 0 || c + tw > cells) continue;
        bool free = true;
        for (int k = std::max(0, c - 1); k <= std::min(cells - 1, c + tw); ++k) if (used[static_cast<size_t>(k)]) free = false;
        if (!free) continue;
        for (int k = c; k < c + tw; ++k) used[static_cast<size_t>(k)] = 1;
        out.push_back({c, t});
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The graphs drawn into a W x H picture (palette indices of the normal resolution: 0 background, 1 outline, 48 .. 255 fill by
// height). `R` is the supersampling factor the caller scales the picture down by afterwards: sizes that are meant in output pixels
// (the minimum row pitch) are multiplied by it.
void SpectroAnalyzer::waterfall_raw(const SpectroSettings& s, int w, int h, int R, bool lines_only, std::vector<uint8_t>& img) {
    img.assign(static_cast<size_t>(std::max(0, w)) * static_cast<size_t>(std::max(0, h)), 0);
    if (w < 16 || h < 8 || cap_ == 0 || ncols_ == 0) return;
    const int N0 = std::clamp(s.wf_graphs, 8, 100);
    const int A = std::max(2, h * std::clamp(s.wf_height, 10, 60) / 100);   // the loudest peak
    const int shear = static_cast<int>(w * std::clamp(s.wf_slant, 0, 40) / 100.0);
    const int fw = spectro_waterfall_plot_width(s, w);
    const int S = std::max(2, h - A - 3 * R);                               // the whole stack from the front baseline to the back one
    // In a small picture the graphs would lie closer together than their own outline is thick (just black lines). Then fewer, evenly
    // spaced graphs are drawn -- a row pitch of 4 pixels at least (braille: 2 dots) -- and the time between them grows to keep the span.
    const int N = std::clamp(std::min(N0, S / ((lines_only ? 2 : 4) * R) + 1), 2, N0);
    prepare(s, fw);
    const double cps = columns_per_second();
    const int step = std::max(1, static_cast<int>(std::lround(std::clamp(s.wf_spacing, 20, 500) / 1000.0 * cps * N0 / N)));
    const long long newest = static_cast<long long>(ncols_) - 1;
    // smooth: the older graphs sit at (slot + phase) and glide back as the clock runs; the phase is the part of a step that has passed
    double phase = 0.0;
    long long base = newest / step * step;
    if (s.wf_smooth) {
        const double nf = std::clamp(disp_cols_ - 1.0, 0.0, static_cast<double>(newest));
        base = static_cast<long long>(std::floor(nf / step)) * step;
        phase = std::clamp((nf - static_cast<double>(base)) / step, 0.0, 0.999);
    }
    auto put = [&](int x, int y, uint8_t v) { if (x >= 0 && x < w && y >= 0 && y < h) img[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = v; };
    auto line = [&](int x0, int y0, int x1, int y1, uint8_t v) {   // Bresenham
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            put(x0, y0, v);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    };
    std::vector<int> top(static_cast<size_t>(fw)), vv(static_cast<size_t>(fw));
    for (int g = N - 1; g >= 0; --g) {
        const long long col = g == 0 ? newest : base - static_cast<long long>(g - 1) * step;
        if (col < 0 || static_cast<uint64_t>(newest - col) + 2 >= cap_) continue;   // no data that old
        auto pos_of = [&](int gg) { return gg == 0 ? 0.0 : static_cast<double>(gg) + phase; };
        if (pos_of(g) > N - 1 + 1e-9) continue;                                      // glided off the back
        const double f = N > 1 ? pos_of(g) / (N - 1) : 0.0;
        const int baseline = h - 2 * R - static_cast<int>(std::lround(f * S));
        const int ox = static_cast<int>(std::lround(f * shear));
        for (int x = 0; x < fw; ++x) {
            const int v = value_at(static_cast<uint64_t>(col), 0, fw - 1 - x);   // rows run from the top (high frequency) down
            vv[static_cast<size_t>(x)] = v;
            top[static_cast<size_t>(x)] = baseline - static_cast<int>(std::lround(static_cast<double>(v) * A / 255.0));
        }
        // the body: down to the baseline of the graph in front of it (the front graph: to the bottom), so nothing of the graphs
        // behind shows through below the curve and the stack ends in a slanted edge, not in a block
        const int next_base = g == 0 ? h - 1 : h - 2 * R - static_cast<int>(std::lround(pos_of(g - 1) / (N - 1) * S));
        const int bottom = std::min(h - 1, std::max(baseline, next_base) + 1);
        for (int x = 0; x < fw; ++x) {
            const int px = ox + x;
            if (px < 0 || px >= w) continue;
            for (int y = std::max(0, top[static_cast<size_t>(x)]); y <= bottom; ++y) {
                uint8_t c = 0;
                if (!lines_only) {
                    const double frac = y <= baseline ? static_cast<double>(baseline - y) / A : 0.0;
                    c = static_cast<uint8_t>(48 + std::clamp(static_cast<int>(std::lround(frac * 207.0)), 0, 207));
                }
                img[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(px)] = c;
            }
        }
        // the outline: a fine black line along the curve and down both ends (braille: lit dots in the colour of their height)
        auto ink = [&](int x) { return lines_only ? static_cast<uint8_t>(std::max(48, 48 + vv[static_cast<size_t>(x)] * 207 / 255)) : static_cast<uint8_t>(1); };
        for (int x = 0; x + 1 < fw; ++x) line(ox + x, top[static_cast<size_t>(x)], ox + x + 1, top[static_cast<size_t>(x + 1)], ink(x));
        line(ox, top[0], ox, bottom, ink(0));
        line(ox + fw - 1, top[static_cast<size_t>(fw - 1)], ox + fw - 1, bottom, ink(fw - 1));
        line(ox, bottom, ox + fw - 1, bottom, ink(0));   // the lower edge: shows as the steps of the slanted side (the graph in front covers the rest)
    }
}

void SpectroAnalyzer::waterfall(const SpectroSettings& s, int w, int h, bool lines_only, std::vector<uint8_t>& img) {
    const bool hi = s.wf_detail == 1 && !lines_only;   // braille already has 2 x 4 dots to a cell
    std::ostringstream k;
    k << ncols_ << "," << w << "," << h << "," << lines_only << "," << s.scale << "," << s.min_freq << "," << s.max_freq << "," << s.gain << "," << s.range << ","
      << s.freq_gain << "," << s.wf_graphs << "," << s.wf_spacing << "," << s.wf_slant << "," << s.wf_height << "," << hi << "," << gen_ << "," << rate_ << "," << (s.wf_smooth ? static_cast<long long>(disp_cols_ * 64.0) : -1LL);
    if (k.str() == wf_key_ && wf_img_.size() == static_cast<size_t>(w) * static_cast<size_t>(h)) { img = wf_img_; return; }
    if (!hi) waterfall_raw(s, w, h, 1, lines_only, img);
    else {
        // twice the resolution, scaled down: every output pixel is 2 x 2 samples. The outline lies in one sample row / column, so it
        // covers half a pixel or all of it (its place is known to half a pixel); that becomes black, half dark or nothing -- and a fine
        // line in a quiet part keeps its colour instead of flickering with the rounding. Index = register * 4 (see spectro_palette_view()).
        std::vector<uint8_t> big;
        waterfall_raw(s, w * 2, h * 2, 2, false, big);
        img.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
        const size_t bw = static_cast<size_t>(w) * 2;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int nb = 0, no = 0, nf = 0, lv = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const uint8_t v = big[(static_cast<size_t>(y) * 2 + dy) * bw + static_cast<size_t>(x) * 2 + dx];
                        if (v == 0) ++nb; else if (v == 1) ++no; else { ++nf; lv += v - 48; }
                    }
                int r = 0;
                if (nb >= 3) r = 0;                                  // the background
                else {
                    const int lev = nf > 0 ? lv / nf : 0;            // 0 .. 207: the fill's height colour
                    if (no >= 2 || (nf == 0 && no > 0)) r = 0;       // the line covers it: black
                    else if (no == 1) r = 41 + std::min(22, lev * 23 / 208);   // a line half over it: half dark
                    else r = 1 + std::min(39, lev * 40 / 208);
                }
                img[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = static_cast<uint8_t>(r << 2);
            }
    }
    wf_key_ = k.str();
    wf_img_ = img;
}

SpectroAnalyzer& spectro() { static SpectroAnalyzer a; return a; }
void spectro_feed_push(const float* lr, size_t frames, int rate) { spectro().push(lr, frames, rate); }
bool spectro_feed_active() { return spectro().visible(); }

// ---- the picture in the terminal --------------------------------------------------------------------------------------
namespace {

std::string b64(const uint8_t* d, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(d[i]) << 16) | (static_cast<uint32_t>(d[i + 1]) << 8) | d[i + 2];
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
    }
    if (i + 1 == n) { const uint32_t v = static_cast<uint32_t>(d[i]) << 16; o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += "=="; }
    else if (i + 2 == n) { const uint32_t v = (static_cast<uint32_t>(d[i]) << 16) | (static_cast<uint32_t>(d[i + 1]) << 8); o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '='; }
    return o;
}

std::string cup(int row, int col) { return "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) + "H"; }

constexpr int kKittyBase = 7300;
constexpr int kParts = 5;   // pieces of a tile around an overlay: whole / top, bottom, left, right

std::string kitty_tile(int id, int row, int col, int cells_w, int cells_h, const std::vector<uint8_t>& idx, int w, int h,
                       const std::array<std::array<uint8_t, 3>, 256>& pal) {
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const auto& p = pal[idx[i]];
        rgb[3 * i] = p[0]; rgb[3 * i + 1] = p[1]; rgb[3 * i + 2] = p[2];
    }
    std::string payload, comp;
    mz_ulong n = mz_compressBound(static_cast<mz_ulong>(rgb.size()));
    std::vector<uint8_t> z(n);
    if (mz_compress2(z.data(), &n, rgb.data(), static_cast<mz_ulong>(rgb.size()), 1) == MZ_OK) { payload = b64(z.data(), n); comp = ",o=z"; }
    else payload = b64(rgb.data(), rgb.size());
    const std::string keys = "a=T,f=24,s=" + std::to_string(w) + ",v=" + std::to_string(h) + ",i=" + std::to_string(id) +
                             ",p=1,z=-1,C=1,q=2" + comp + ",c=" + std::to_string(cells_w) + ",r=" + std::to_string(cells_h);
    std::string o = cup(row, col);
    constexpr size_t kChunk = 4096;
    for (size_t off = 0; off < payload.size() || off == 0; off += kChunk) {
        const bool more = off + kChunk < payload.size();
        o += "\x1b_G";
        if (off == 0) o += keys + ",";
        o += std::string("m=") + (more ? "1" : "0") + ";" + payload.substr(off, kChunk) + "\x1b\\";
        if (!more) break;
    }
    return o;
}

// Sixel: 64 colour registers (the 256 steps / 4), opaque. In Windows Terminal two screen rows per sixel row (pixel
// aspect ratio), so half the data.
// `thin`: fine lines (the waterfall's outlines). With two screen rows per sixel row only every second picture row would be sent, and
// a line that lies in the other one would be missing: of the two rows the darker pixel is sent then.
std::string sixel_tile(int row, int col, const std::vector<uint8_t>& idx, int w, int h, const std::array<std::array<uint8_t, 3>, 256>& pal, bool thin = false) {
    static const bool aspect = std::getenv("WT_SESSION") != nullptr || std::getenv("MOUSIKI_SIXEL_ASPECT") != nullptr;
    const int pan = aspect ? 2 : 1;
    const int H = (h + pan - 1) / pan;
    std::string o = cup(row, col);
    o += "\x1bP0;1;0q\"" + std::to_string(pan) + ";1;" + std::to_string(w) + ";" + std::to_string(H);
    for (int c = 0; c < 64; ++c) {
        const auto& p = pal[static_cast<size_t>(c * 4 + 2)];
        o += "#" + std::to_string(c) + ";2;" + std::to_string(p[0] * 100 / 255) + ";" + std::to_string(p[1] * 100 / 255) + ";" + std::to_string(p[2] * 100 / 255);
    }
    std::vector<uint8_t> bits(static_cast<size_t>(64) * w, 0);
    std::vector<int> mx(64, -1);
    for (int by = 0; by < H; by += 6) {
        const int bh = std::min(6, H - by);
        std::vector<int> used;
        for (int y = 0; y < bh; ++y) {
            const int sy = std::min(h - 1, (by + y) * pan);
            const int sy2 = (thin && pan == 2) ? std::min(h - 1, sy + 1) : sy;
            for (int x = 0; x < w; ++x) {
                int ix = idx[static_cast<size_t>(sy) * w + x];
                if (sy2 != sy) {
                    const int i2 = idx[static_cast<size_t>(sy2) * w + x];
                    const auto &pa = pal[static_cast<size_t>(ix)], &pb = pal[static_cast<size_t>(i2)];
                    if (pb[0] + pb[1] + pb[2] < pa[0] + pa[1] + pa[2]) ix = i2;
                }
                const int c = ix >> 2;
                if (mx[static_cast<size_t>(c)] < 0) used.push_back(c);
                mx[static_cast<size_t>(c)] = std::max(mx[static_cast<size_t>(c)], x);
                bits[static_cast<size_t>(c) * w + x] |= static_cast<uint8_t>(1u << y);
            }
        }
        for (size_t u = 0; u < used.size(); ++u) {
            const int c = used[u];
            uint8_t* b = bits.data() + static_cast<size_t>(c) * w;
            o += "#" + std::to_string(c);
            int run = 0;
            char prev = 0;
            auto flush = [&]() { if (run > 3) o += "!" + std::to_string(run) + prev; else o.append(static_cast<size_t>(run), prev); run = 0; };
            for (int x = 0; x <= mx[static_cast<size_t>(c)]; ++x) {
                const char ch = static_cast<char>(63 + b[x]);
                b[x] = 0;
                if (run > 0 && ch == prev) ++run; else { if (run) flush(); prev = ch; run = 1; }
            }
            if (run) flush();
            mx[static_cast<size_t>(c)] = -1;
            if (u + 1 < used.size()) o += '$';
        }
        o += '-';
    }
    o += "\x1b\\";
    return o;
}

} // namespace

bool spectro_rect_copy() {
    if (const char* e = std::getenv("MOUSIKI_SIXEL_SHIFT")) if (*e) return *e == '1';
    // Windows Terminal (also from WSL, which passes WT_SESSION on): its DECCRA copies the Sixel picture along. Other
    // terminals that have DECCRA (xterm ...) keep Sixel graphics apart from the text cells, so it would not move them.
    static const bool wt = std::getenv("WT_SESSION") != nullptr;
    return wt;
}

std::string SpectroGfx::clear(int proto) {
    std::string o;
    have_off_ = false;
    // Sixel: the picture is part of the screen; spaces over its cells take it away (put this BEFORE the new text, so
    // the next screen -- settings, a menu -- is never left with pieces of it in cells it does not write itself)
    if (proto == 2 && shown_ && row_ >= 0 && cols_ > 0) {
        o += "\x1b[0m";
        for (int r = 0; r < rows_; ++r) o += cup(row_ + r, col_) + std::string(static_cast<size_t>(cols_), ' ');
    }
    if (proto == 1)
        for (size_t t = 0; t < on_screen_.size(); ++t)
            if (on_screen_[t])
                for (int part = 0; part < kParts; ++part)
                    o += "\x1b_Ga=d,d=I,i=" + std::to_string(kKittyBase + static_cast<int>(t) * kParts + part) + ",q=2\x1b\\";
    std::fill(on_screen_.begin(), on_screen_.end(), 0);
    std::fill(layout_.begin(), layout_.end(), 0);
    all_dirty_ = true;
    shown_ = false;
    return o;
}

std::string SpectroGfx::emit(int proto, const SpectroSettings& s_in, SpectroAnalyzer& a, int row, int col, int cols, int rows,
                             int cell_w, int cell_h, bool due, int skip_row, int skip_col, int skip_rows, int skip_cols) {
    SpectroSettings s = s_in;
    if (s.view == 1) s.motion = 0;   // the waterfall has no sweep / scroll: it is sent in tiles like a sweep
    std::string o;
    if (proto == 0 || cols <= 0 || rows <= 0) return o;
    std::ostringstream k;
    k << proto << "," << row << "," << col << "," << cols << "," << rows << "," << cell_w << "," << cell_h << "," << s.scheme << ","
      << s.scale << "," << s.min_freq << "," << s.max_freq << "," << s.gain << "," << s.range << "," << s.freq_gain << ","
      << s.motion << "," << s.channels << "," << s.span << "," << s.window_log2 << "," << s.zero_pad << "," << (s.scheme == 3 ? g_grad_gen : 0)
      << "," << s.view << "," << s.wf_graphs << "," << s.wf_spacing << "," << s.wf_slant << "," << s.wf_height << "," << s.wf_detail;
    const int n = (cols + kTileCols - 1) / kTileCols;
    if (k.str() != key_) {
        if (proto_ == 1) o += clear(1);
        key_ = k.str();
        dirty_.assign(static_cast<size_t>(n), 1);
        on_screen_.assign(static_cast<size_t>(n), 0);
        layout_.assign(static_cast<size_t>(n), 0);
        all_dirty_ = true;
    }
    proto_ = proto;
    row_ = row; col_ = col; cols_ = cols; rows_ = rows; cw_ = cell_w; ch_ = cell_h;
    const int W = cols * cell_w, H = rows * cell_h;
    // Which part of a tile an overlay covers: the rest of the tile is still drawn (pieces above, below, left and right
    // of it), so the spectrogram keeps running all around the overlay -- also in the columns of a tile that the overlay
    // only partly covers (those keep moving too).
    struct Piece { int r0, r1, c0, c1; };   // cells, relative to the tile
    auto pieces_of = [&](int t, std::vector<Piece>& ps) {
        const int c0 = t * kTileCols, cw = std::min(kTileCols, cols - c0);
        ps.clear();
        int r0 = 0, r1 = 0, a = 0, b = 0;
        if (skip_rows > 0 && skip_cols > 0 && col + c0 < skip_col + skip_cols && skip_col < col + c0 + cw) {
            r0 = std::clamp(skip_row - row, 0, rows);
            r1 = std::clamp(skip_row + skip_rows - row, 0, rows);
            a = std::clamp(skip_col - (col + c0), 0, cw);
            b = std::clamp(skip_col + skip_cols - (col + c0), 0, cw);
        }
        if (r1 <= r0 || b <= a) { ps.push_back({0, rows, 0, cw}); return 0; }
        if (r0 > 0) ps.push_back({0, r0, 0, cw});
        if (r1 < rows) ps.push_back({r1, rows, 0, cw});
        if (a > 0) ps.push_back({r0, r1, 0, a});
        if (b < cw) ps.push_back({r0, r1, b, cw});
        return 1 + r0 + 100 * r1 + 10000 * a + 100000 * b;
    };
    // pictures per second: a sweep sends a narrow strip, scroll the whole picture (much more data)
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last;
    static double bytes_avg = 0;   // bytes of one whole picture (scroll) / of one update (sweep), smoothed
    constexpr double kBudget = 3.0e6;   // bytes per second a terminal is given for the picture
    const auto now = Clock::now();
    double min_gap = std::max(1.0 / 30, bytes_avg / kBudget);
    // Sixel scroll in a terminal that can copy a rectangle of the screen (DECCRA; Windows Terminal moves the picture
    // with it): the terminal shifts what is already on screen by whole cells and only the newest cell column is sent --
    // a few KB per step instead of the whole picture, so even a full-screen picture moves on at every step.
    const bool shift = proto == 2 && s.motion == 1 && spectro_rect_copy();
    if (shift) a.set_scroll_step(cell_w);
    else if (s.motion == 1) {
        // Scroll: the picture is sent whole whenever it moves. It moves on in EVEN steps of whole pixels, as many at a
        // time as the budget needs (a small view: every pixel; a full-screen Sixel picture: 2 or 3) -- irregular jumps
        // looked far more jerky than regular ones of the same average size.
        const double px_per_s = a.columns_per_second() * W / std::max(1, a.columns_per_page());
        const double pics_per_s = std::min(60.0, bytes_avg > 0 ? kBudget / bytes_avg : 60.0);
        static int step = 1;
        const int want = std::max(1, static_cast<int>(std::ceil(px_per_s * 1.1 / std::max(1.0, pics_per_s))));
        if (want > step || want < step - 1 || (want < step && px_per_s * 1.3 / pics_per_s < step - 1)) step = want;
        a.set_scroll_step(step);
        min_gap = std::min(min_gap, 1.0 / 60);   // the steps pace it
    } else a.set_scroll_step(1);
    int f = 0, l = 0;
    if (!shift) a.changed_columns(s, W, f, l);
    if (all_dirty_) { std::fill(dirty_.begin(), dirty_.end(), 1); all_dirty_ = false; have_off_ = false; }
    if (l > f)
        for (int t = f / (kTileCols * cell_w); t <= std::min(n - 1, (l - 1) / (kTileCols * cell_w)); ++t) dirty_[static_cast<size_t>(t)] = 1;
    std::vector<Piece> ps;
    bool any_new = false;   // a tile whose layout changed or that is not on screen yet: no waiting
    for (int t = 0; t < n; ++t) {
        const int lay = pieces_of(t, ps);
        if (lay != layout_[static_cast<size_t>(t)]) {
            if (on_screen_[static_cast<size_t>(t)] && proto == 1)
                for (int part = 0; part < kParts; ++part) o += "\x1b_Ga=d,d=I,i=" + std::to_string(kKittyBase + t * kParts + part) + ",q=2\x1b\\";
            layout_[static_cast<size_t>(t)] = lay;
            on_screen_[static_cast<size_t>(t)] = 0;
            dirty_[static_cast<size_t>(t)] = 1;
        }
        if (!on_screen_[static_cast<size_t>(t)]) any_new = true;
    }
    if (!due || (!any_new && !urgent_ && !shift && std::chrono::duration<double>(now - last).count() < min_gap)) return o;
    last = now;
    urgent_ = false;
    const size_t before = o.size();
    bool whole = true;
    const auto& pal = spectro_palette_view(s);
    std::vector<uint8_t> idx, part;
    if (shift) {
        // a rectangle of the picture (cells relative to it), drawn from the current view
        auto draw_rect = [&](int r0, int r1, int c0, int c1) {
            const int tw = (c1 - c0) * cell_w, ph = (r1 - r0) * cell_h;
            if (tw <= 0 || ph <= 0) return;
            a.render(s, W, H, c0 * cell_w, tw, idx, false);
            part.assign(idx.begin() + static_cast<long>(r0) * cell_h * tw, idx.begin() + static_cast<long>(r1) * cell_h * tw);
            o += sixel_tile(row + r0, col + c0, part, tw, ph, pal, s.view == 1);
        };
        const uint64_t off = a.scroll_offset(W);
        if (!have_off_ || off < last_off_ || off - last_off_ >= static_cast<uint64_t>(W)) {
            std::fill(dirty_.begin(), dirty_.end(), 1);   // start, a seek back, a new track: the whole picture
        } else if (off > last_off_) {
            const int k = static_cast<int>((off - last_off_) / static_cast<uint64_t>(cell_w));
            // the parts of the picture outside an overlay: each moves left on its own (an overlay must not move along)
            std::vector<Piece> area;
            int r0 = 0, r1 = 0, ca = 0, cb = 0;
            if (skip_rows > 0 && skip_cols > 0) {
                r0 = std::clamp(skip_row - row, 0, rows); r1 = std::clamp(skip_row + skip_rows - row, 0, rows);
                ca = std::clamp(skip_col - col, 0, cols); cb = std::clamp(skip_col + skip_cols - col, 0, cols);
            }
            if (r1 <= r0 || cb <= ca) area.push_back({0, rows, 0, cols});
            else {
                if (r0 > 0) area.push_back({0, r0, 0, cols});
                if (r1 < rows) area.push_back({r1, rows, 0, cols});
                if (ca > 0) area.push_back({r0, r1, 0, ca});
                if (cb < cols) area.push_back({r0, r1, cb, cols});
            }
            for (const Piece& p : area) {
                const int wc = p.c1 - p.c0;
                if (k < wc) {
                    // DECCRA: copy rows r0..r1, columns c0 + k .. c1 to column c0 (1-based, page 1)
                    o += "\x1b[" + std::to_string(row + p.r0 + 1) + ";" + std::to_string(col + p.c0 + k + 1) + ";" +
                         std::to_string(row + p.r1) + ";" + std::to_string(col + p.c1) + ";1;" + std::to_string(row + p.r0 + 1) + ";" +
                         std::to_string(col + p.c0 + 1) + ";1$v";
                    draw_rect(p.r0, p.r1, p.c1 - k, p.c1);
                } else draw_rect(p.r0, p.r1, p.c0, p.c1);
            }
            last_off_ += static_cast<uint64_t>(k) * static_cast<uint64_t>(cell_w);
        }
        last_off_ = off;
        have_off_ = true;
    }
    for (int t = 0; t < n; ++t) {
        if (!dirty_[static_cast<size_t>(t)] && on_screen_[static_cast<size_t>(t)]) { whole = false; continue; }
        const int c0 = t * kTileCols, cw = std::min(kTileCols, cols - c0);
        const int tw = cw * cell_w;
        a.render(s, W, H, c0 * cell_w, tw, idx, true);
        const int lay = pieces_of(t, ps);
        for (size_t pi = 0; pi < ps.size(); ++pi) {
            const Piece& p = ps[pi];
            const int ph = (p.r1 - p.r0) * cell_h, pw = (p.c1 - p.c0) * cell_w;
            if (ph <= 0 || pw <= 0) continue;
            part.resize(static_cast<size_t>(ph) * pw);
            for (int y = 0; y < ph; ++y)
                std::copy_n(idx.begin() + (static_cast<std::ptrdiff_t>(p.r0 * cell_h + y) * tw + static_cast<std::ptrdiff_t>(p.c0) * cell_w), pw,
                            part.begin() + static_cast<std::ptrdiff_t>(y) * pw);
            const int id = kKittyBase + t * kParts + (lay ? 1 + static_cast<int>(pi) : 0);
            if (proto == 1) o += kitty_tile(id, row + p.r0, col + c0 + p.c0, p.c1 - p.c0, p.r1 - p.r0, part, pw, ph, pal);
            else o += sixel_tile(row + p.r0, col + c0 + p.c0, part, pw, ph, pal, s.view == 1);
        }
        dirty_[static_cast<size_t>(t)] = 0;
        on_screen_[static_cast<size_t>(t)] = 1;
    }
    // the size of a whole picture (scroll) / of an update (sweep), for the pacing above
    const size_t sent = o.size() - before;
    if (sent > 64) {
        const double b = static_cast<double>(sent);
        bytes_avg = (s.motion == 1 && !whole) || bytes_avg <= 0 ? std::max(bytes_avg, b) : bytes_avg * 0.7 + b * 0.3;
    }
    shown_ = true;
    return o;
}

} // namespace muisc
