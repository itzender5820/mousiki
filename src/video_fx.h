#pragma once
// The retro looks of the video picture (Settings -> Video Effect, VIDEO overlay, Style Option): their numbers and names in one place.
#include <cctype>
#include <string>

namespace muisc {

inline constexpr int kVideoFxCount = 29;
// 0 off, 1 scanlines, 2 crt, 3 vhs, 4 glitch, 5 pixel, 6 gradient, 7 quiet glitch, 8 phosphor green, 9 phosphor amber, 10 halftone, 11 dither,
// 12 game boy (high-res), 13 ega 16, 14 bloom, 15 film, 16 neon edge, 17 interlace, 18 game boy low-res, 19 neon edge gradient, 20 game boy high-res gradient, 21 dither b/w, 22 dither gradient, 23 game boy high-res b/w, 24 game boy low-res b/w, 25 game boy low-res gradient,
// 26 phosphor green glow, 27 cga, 28 c64
inline const char* video_fx_name(int fx) {   // as shown in the menus (every word capitalized)
    static const char* const n[kVideoFxCount] = {"Off", "Scanlines", "CRT", "VHS", "Glitch", "Pixel", "Gradient", "Quiet Glitch", "Phosphor Green", "Phosphor Amber",
                                                 "Halftone", "Dither", "Game Boy High-Res", "EGA 16", "Bloom", "Film", "Neon Edge", "Interlace", "Game Boy Low-Res", "Neon Edge Gradient", "Game Boy High-Res Gradient", "Dither B/W", "Dither Gradient", "Game Boy High-Res B/W", "Game Boy Low-Res B/W", "Game Boy Low-Res Gradient", "Phosphor Green Glow", "CGA", "C64"};
    return fx >= 0 && fx < kVideoFxCount ? n[fx] : "Off";
}
inline std::string video_fx_key(int fx) {   // as written in config.txt (lower case)
    std::string v = video_fx_name(fx);
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}
// The order of the effects in the lists (the numbers stay as they are: they are stored): "Quiet Glitch" sits right below "Glitch".
// "Dither" (11) is no longer offered: it hardly differed from "Dither B/W" (21), which takes its place (an old setting of 11 reads as 21).
inline constexpr int kVideoFxShown = kVideoFxCount - 2;   // (Dither 11 and Bloom 14 are not offered)
inline constexpr int kVideoFxOrder[kVideoFxShown] = {0, 1, 2, 3, 4, 7, 5, 6, 8, 26, 9, 10, 21, 22, 12, 23, 20, 18, 25, 24, 13, 27, 28, 15, 16, 19, 17};
// "Bloom" (14) is no longer offered either (too plain): an old setting of it reads as Off.
inline int video_fx_canon(int fx) { return fx == 11 ? 21 : fx == 14 || fx >= kVideoFxCount ? 0 : fx; }   // (an effect that is gone, e.g. Night Vision of an early 5.1.0 build, reads as Off)
// The effects that run the gradient wave need the visualizer gradient.
// The effects that are built on the tube's scanlines / mask: the Video Base Layer replaces the structure they draw (Scanlines itself keeps its lines).
inline bool video_fx_uses_base(int fx) { return fx == 2 || fx == 3 || fx == 4 || fx == 7 || fx == 8 || fx == 9 || fx == 26; }
inline bool video_fx_uses_gradient(int fx) { return fx == 6 || fx == 19 || fx == 20 || fx == 22 || fx == 25; }
inline int video_fx_pos(int fx) { fx = video_fx_canon(fx); for (int i = 0; i < kVideoFxShown; ++i) if (kVideoFxOrder[i] == fx) return i; return 0; }
// The effect `dir` steps (+1 / -1) on from `fx` in the list's order, round and round. With `with_default` -1 ("as in the VIDEO menu") is one more
// stop in front of Off.
inline int video_fx_step(int fx, int dir, bool with_default = false) {
    const int n = kVideoFxShown + (with_default ? 1 : 0);
    const int pos = fx < 0 ? 0 : video_fx_pos(fx) + (with_default ? 1 : 0);
    const int to = ((pos + (dir < 0 ? -1 : 1)) % n + n) % n;
    if (with_default) return to == 0 ? -1 : kVideoFxOrder[to - 1];
    return kVideoFxOrder[to];
}
// A name (any case) or a number as written in config.txt; anything else = off.
inline int video_fx_from(const std::string& s) {
    std::string v;
    for (char c : s) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "game boy") return 12;   // (the name before there were two resolutions)
    for (int i = 0; i < kVideoFxCount; ++i)
        if (v == video_fx_key(i) || v == std::to_string(i)) return video_fx_canon(i);
    return 0;
}

// Video Resolution (VideoResolution=): the widest picture that is decoded and sent, 0 standard (as it always was), 1 high, 2 full.
// Kitty gets true colour with high / full, Sixel (at most 256 colours) a wider picture only.
inline const char* video_res_name(int r) { return r == 1 ? "high" : r == 2 ? "full" : "standard"; }
inline int video_res_from(const std::string& s) { return s.rfind("high", 0) == 0 || s == "1" ? 1 : s.rfind("full", 0) == 0 || s == "2" ? 2 : 0; }
inline int video_res_width(int r, bool sixel) { return sixel ? (r == 1 ? 720 : r == 2 ? 960 : 480) : (r == 1 ? 1280 : r == 2 ? 1920 : 640); }
inline constexpr int kVideoBufferChoices[6] = {8, 16, 32, 64, 128, 256};   // MB of decoded frames kept ahead (VideoBuffer=)
inline std::string video_res_label(int r, bool sixel = false) { return std::string(video_res_name(r)) + " (" + std::to_string(video_res_width(r, sixel)) + " px)"; }
// How many seconds of a 16:9 picture (25 frames per second, 3 bytes a pixel) fit in `mb` megabytes at the resolution.
inline int video_buffer_seconds(int mb, int res, bool sixel) {
    const double w = video_res_width(res, sixel), h = w * 9.0 / 16.0;
    return static_cast<int>(mb * 1048576.0 / (w * h * 3.0) / 25.0 + 0.5);
}

// Video Base Layer (VideoBase=): the structure of the screen under the tube looks (CRT, VHS, Glitch, Quiet Glitch, the Phosphor looks):
// 0 scanlines (as it always was), 1 aperture grille (Trinitron: vertical RGB stripes and two thin damper wires), 2 shadow mask (round dots
// in a delta pattern), 3 slot mask (stripes with slots), 4 LCD grid (square pixels with dark gaps), 5 clean (no structure at all).
inline constexpr int kBaseLayers = 6;
inline const char* base_name(int b) {
    switch (b) { case 1: return "aperture grille"; case 2: return "shadow mask"; case 3: return "slot mask"; case 4: return "lcd grid"; case 5: return "clean"; default: return "scanlines"; }
}
inline int base_from(const std::string& s) { for (int i = 0; i < kBaseLayers; ++i) if (s == base_name(i) || s == std::to_string(i)) return i; return 0; }

// The colours of the "gradient" look: left / centre / right stop of the visualizer gradient (RGB).
// Dither pattern (a toggle that works with every look that dithers: Game Boy, EGA 16, Dither B/W and Gradient, and the reduction to the 256
// colours of a terminal's picture protocol, e.g. sixel). 0 "auto" = what each look used before (the default pattern is 5, interleaved noise) (blue noise for the Game Boy looks and for the
// 256 colours, Bayer 8x8 for EGA and the Dither looks). The patterns do not change from frame to frame: no shimmer, and a calm picture to encode.
inline constexpr int kDitherPatterns = 7;
inline const char* dither_name(int d) {
    switch (d) { case 1: return "blue noise"; case 2: return "bayer 2x2"; case 3: return "bayer 4x4"; case 4: return "bayer 8x8"; case 5: return "interleaved noise"; case 6: return "golden ratio"; default: return "auto"; }
}
inline int dither_from(const std::string& s) {
    if (s == "clustered dot" || s == "line screen") return 5;   // (patterns of batch 242 that are gone: the default one)
    for (int i = 0; i < kDitherPatterns; ++i) if (s == dither_name(i) || s == std::to_string(i)) return i;
    return 0;
}

struct VideoFxGradient {
    unsigned char c[3][3] = {{0, 200, 255}, {160, 90, 255}, {255, 60, 160}};
    int sat = 100;   // Video Gradient Saturation 0 .. 100 (%): the gradient's colours as they are at 100, paler towards 0 (0 = the same brightness in grey)
};

// Chromatic aberration (a toggle that works with every effect, for the video and for covers): 0 off, 1 subtle, 2 medium, 3 strong.
inline constexpr int kChromaLevels = 4;
inline const char* chroma_name(int l) { return l == 1 ? "subtle" : l == 2 ? "medium" : l == 3 ? "strong" : "off"; }
inline int chroma_from(const std::string& s) { return s == "subtle" || s == "1" ? 1 : s == "medium" || s == "2" ? 2 : s == "strong" || s == "3" ? 3 : 0; }

// Everything that is done to the picture of a video: the grade (contrast 0..100 %, colours 0 = greyscale .. 100 = as they are), chromatic
// aberration, then the effect.
struct VideoLook {
    int fx = 0, contrast = 0, colors = 100, chroma = 0, brightness = 50;
    int base = 0;     // the base layer under the tube looks (0 scanlines .. kBaseLayers - 1)
    int dither = 0;   // the dither pattern (0 auto .. kDitherPatterns - 1); only changes how the dithering looks, so it is not part of active()
    VideoFxGradient grad;
    bool active() const { return fx > 0 || contrast > 0 || colors < 100 || chroma > 0 || brightness != 50; }
    std::string sig() const {
        return std::to_string(fx) + "," + std::to_string(contrast) + "," + std::to_string(colors) + "," + std::to_string(chroma) + "," + std::to_string(brightness) + "," + std::to_string(dither) + "," + (video_fx_uses_base(fx) ? std::to_string(base) : std::string()) + "," +
               (video_fx_uses_gradient(fx) ? std::to_string(grad.c[0][0]) + "." + std::to_string(grad.c[0][1]) + "." + std::to_string(grad.c[0][2]) + "." + std::to_string(grad.c[2][0]) + "." + std::to_string(grad.c[2][1]) + "." + std::to_string(grad.c[2][2]) + "." + std::to_string(grad.sat) : std::string());
    }
};

}  // namespace muisc
