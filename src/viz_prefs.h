#pragma once
// The oscilloscope and spectrogram settings -- everything the SHIFT+o / SHIFT+i overlays edit -- in ONE place. The music player's
// `Settings` (config.txt) and the radio's `RadioSettings` (radio_config.txt) both INHERIT from VizPrefs, so the overlays, the
// rows, the stepping, the config keys and the defaults are the same code for both, while each keeps its own values.
//
// "Share settings profiles" (Settings -> ANIMA/VIZ -> OSCI/SPECTRO SETTINGS): the radio then uses the player's VizPrefs (config.txt); its own
// values stay untouched in radio_config.txt and come back when the option is turned off. Colours / themes are never shared.
#include <iosfwd>
#include <string>
#include <vector>
#include "scope_metrics.h"
#include "spectrogram.h"

namespace muisc {

// Palettes of the oscilloscope (Color row of the SHIFT+o overlay). 0 = the VIZ gradient of the COLORS tab laid over the
// picture from left to right; the others colour by the beam speed (about the pitch: slow = first colour, fast = last colour).
constexpr int kOsciPaletteCount = 9;
extern const char* const kOsciPaletteNames[kOsciPaletteCount];   // gradient, settings, temperature, aurora, magma, ice, neon, spectrum, classic green

// The oscilloscope's parameters of ONE style (braille or image).
struct OsciSet {
    float decay = 0.80f;                // afterglow per frame, 0.00 .. 0.99
    float dot_threshold = 0.28f;        // braille: brightness a subpixel needs to light a dot, 0.01 .. 1.00
    float tail = 0.45f;                 // brightness of the oldest trace sample, 0.00 .. 1.00
    bool interp = true;                 // Line/Vec. Interpol.: connect the samples with lines
    bool z = false;                     // Z-Axis (XYZ mode): the beam intensity follows a Z signal
    float z_depth = 0.70f;              // 0.00 .. 1.00, how strongly Z modulates the beam
    int z_source = 0;                   // 0 = beam speed, 1 = signal level
    int trace = 1024;                   // samples per frame, 128 .. 8192
    bool rotate = false;                // 45 degree rotation (M/S view)
    bool mono_phase = true;             // near-mono signals draw a phase portrait instead of a diagonal line
    int palette = 0;                    // see kOsciPaletteNames
    float glow = 0.60f;                 // image: size / strength of the bloom around the beam, 0.00 .. 1.00
    int frame_rate = 30;                // screen refresh while this style is in use: 30 | 45 | 60 | 90 | 120 | 165 fps
    bool music = false;                 // oscilloscope music mode (SHIFT+o): unprocessed signal, fixed scale, no phase portrait
    bool crt = true;                    // music mode: CRT beam (every sample once, light by dwell time); false = the classic look
    bool low_rate = true;               // music mode, CRT beam: below 96 kHz the jumps between shapes are faded out
    bool dwell = true;                  // CRT beam: light by dwell time (slow strokes bright, jumps dark); false = every stroke equally bright
    bool rough = false;                 // automatic size from the previous frame's peak only (sudden louder beats clamped to the edges); not in music mode
    // braille starts with the classic beam and rough mode (loud beats clip along the edges); the image style with the CRT beam
    static OsciSet braille_defaults() { OsciSet o; o.crt = false; o.rough = true; return o; }
};


// Everything the SHIFT+o / SHIFT+i overlays edit, plus the big scope overlay's grid (SHIFT+0).
struct VizPrefs {
    // Oscilloscope look, tuned live from the SHIFT+o overlay in the main UI (Osci* / OsciImage*).
    // Each style (braille / image) keeps its own parameter set; osci() is the one in use.
    OsciSet osci_set[2] = {OsciSet::braille_defaults(), OsciSet{}};   // [0] = braille, [1] = image
    int osci_style = 0;                 // 0 = braille characters, 1 = image (a real pixel picture: Kitty graphics / Sixel)
    bool scope_grid = true;             // the big scope overlay (SHIFT+0): axes / grid over both pictures (SHIFT+a there; ScopeGrid)
    bool scope_values = true;           // ... and the axis values at the grid lines (SHIFT+v there; ScopeValues)
    std::string scope_grid_color;       // its colour, 0..255; empty = the colour of inactive list elements (SHIFT+c there; ScopeGridColor)
    ScopeMetrics scope_metrics;         // ... and its grid metrics, per picture (SHIFT+p there; ScopeYt* / ScopeXy*)
    int osci_mode = 0;                  // 0 = XY (L/R), 1 = YT (amplitude over time), 2 = both on top of each other (overlap), 3 = both in two rows, XY above YT, 4 = both in two columns, XY left of YT (OsciMode=xy|yt|overlap|row|col)
    std::string gfx_protocol = "auto";  // image style: auto | kitty | sixel | off
    std::string cell_pixels;            // image style: pixels of one terminal cell "WxH"; empty = ask the terminal
    SpectroSettings spectro;            // the spectrogram (SHIFT+i overlay, SHIFT+u full screen), Audacity's defaults
    // Each look has its own values (colours, scale, frequencies, gain ...): spectrogram and waterfall, each as image and as braille.
    // `spectro` holds the ones in use; spectro_sync() (call it after every edit) stores them in their slot and, when the view or the
    // style was switched, loads the slot of the new look. A look that was never used starts with a copy of the values in use.
    SpectroSettings spectro_slot[4];
    bool spectro_have[4] = {false, false, false, false};
    int spectro_active = -1;
    static int spectro_slot_of(const SpectroSettings& s) { return (s.view == 1 ? 2 : 0) + (s.style == 1 ? 1 : 0); }
    // The style (0 braille / 1 image) each view was last shown in (-1: never): switching the view brings back ITS style and ITS
    // values, so braille chosen for the spectrogram does not show in the waterfall, and the other way round.
    int spectro_view_style[2] = {-1, -1};
    void spectro_sync() {
        const int now = spectro_slot_of(spectro);
        if (spectro_active < 0 || now == spectro_active) {
            spectro_active = now;
            spectro_slot[now] = spectro;
            spectro_have[now] = true;
            spectro_view_style[now / 2] = now % 2;
            return;
        }
        SpectroSettings leaving = spectro;   // (the view and style row of the old look are the ones it had)
        leaving.view = spectro_active / 2;
        leaving.style = spectro_active % 2;
        spectro_slot[spectro_active] = leaving;
        spectro_have[spectro_active] = true;
        spectro_view_style[spectro_active / 2] = spectro_active % 2;
        const bool view_changed = (now / 2) != (spectro_active / 2);
        int target = now;
        if (view_changed) {   // the other view: in the style it had last (a view never shown starts as an image)
            const int ls = spectro_view_style[now / 2];
            target = (now / 2) * 2 + (ls >= 0 ? ls : 1);
        }
        if (spectro_have[target]) {
            const bool smooth = spectro.wf_smooth;
            spectro = spectro_slot[target];
            spectro.wf_smooth = smooth;
        } else if (view_changed) {   // a look never used: its own defaults, not a copy of the other view's values
            SpectroSettings fresh;
            fresh.wf_smooth = spectro.wf_smooth;
            spectro = fresh;
        } else {
            spectro_slot[target] = spectro;   // another style of the same view: starts with a copy of the values in use
        }
        spectro.view = target / 2;
        spectro.style = target % 2;
        spectro_slot[target] = spectro;
        spectro_have[target] = true;
        spectro_active = target;
        spectro_view_style[target / 2] = target % 2;
    }
    int image_scale = 2;                // image style: 1 = full, 2 = half, 3 = a third of the screen resolution (enlarged on output)
    int image_detail = 0;               // image style: 0 = normal (picture up to 560 px wide), 1 = high (up to 1024 px)
    OsciSet& osci() { return osci_set[osci_style == 1 ? 1 : 0]; }
    const OsciSet& osci() const { return osci_set[osci_style == 1 ? 1 : 0]; }

    // The look of `src` (every value of the overlays) -- but this one's own grid COLOUR, which belongs to the colour theme.
    void take_shared_from(const VizPrefs& src) { std::string gc = scope_grid_color; *this = src; scope_grid_color = gc; }
};

// ---- the SHIFT+o overlay (osci_settings.cpp): the same rows in both modes -------------------------------------------------
// Overlay rows (ids). Which of them are shown depends on the style: osci_visible_rows().
enum OsciRow { kOrDecay, kOrDot, kOrTail, kOrInterp, kOrZ, kOrZDepth, kOrZSource, kOrTrace, kOrRotate, kOrMono, kOrPalette, kOrGlow,
               kOrStyle, kOrCells, kOrProtocol, kOrFps, kOrDisplay, kOrRes, kOrMusic, kOrMode, kOrDetail, kOrCrt, kOrLowRate, kOrDwell, kOrRough, kOsciRowCount };
// image style: Image detail (SHIFT+o) -- the widest picture rendered: normal = 560 px (big overlay 640 per picture), high = 1024
// (1280), for sharper thin lines; more to render and send per frame (Sixel especially)
inline int osci_detail_cap(int detail, bool big) { return detail == 1 ? (big ? 1280 : 1024) : (big ? 640 : 560); }
// image style: the picture is rendered at 1/1, 1/2 or 1/3 of the screen resolution and enlarged on the way to the terminal
const char* osci_res_name(int scale);   // "full" | "half" | "third"
int osci_res_parse(const std::string& v, int def);
std::vector<int> osci_visible_rows(const VizPrefs& s);          // the rows the overlay shows for the style in use
std::string osci_row_label(int row);
std::string osci_row_value(const VizPrefs& s, int row);         // kOrDisplay is the caller's (the player and the radio show different things)
void osci_adjust(VizPrefs& s, int row, int dir);                // dir = +1 / -1; kOrDisplay is the caller's
void osci_reset(VizPrefs& s);                                   // the parameters of the style in use
// Config keys Osci* / OsciImage* / OsciStyle / OsciMode / OsciImageProtocol / FrameRate / ScopeGrid ... (case does not matter).
// True = the key was one of them. The grid metrics (ScopeYt* / ScopeXy*) are parsed by the caller with scope_metric_parse().
bool osci_config_key(VizPrefs& s, const std::string& key, const std::string& value);
// Writes the oscilloscope block of the config file; `c` is the comment prefix ("##" in config.txt, "#" in radio_config.txt).
void osci_config_write(std::ostream& out, const VizPrefs& s, const char* c);
// The spectrogram looks that are not in use (SpectroProfile<n>... keys: n = 2 * view + style, view 0 spectrogram / 1 waterfall, style 0 braille / 1 image)
bool spectro_profile_key(VizPrefs& s, const std::string& key, const std::string& value);
void spectro_profiles_write(std::ostream& out, const VizPrefs& s, const char* comment_prefix);
void spectro_profiles_finish(VizPrefs& s);   // after reading a file: the values in use go into their slot

} // namespace muisc
