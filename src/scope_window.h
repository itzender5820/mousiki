#pragma once
// The oscilloscope (SHIFT+9) and the spectrogram (SHIFT+8) in their own windows, in the player and in the radio.
//
// Each window is a second copy of this very program started as `mousiki --scope-window` / `mousiki --spectro-window`. It draws with the graphics card
// through SDL2 (zlib licence), which is loaded at run time (SDL2.dll / libSDL2-2.0.so.0 / libSDL2-2.0.0.dylib): the
// build needs nothing extra, and without SDL2 everything else works and the key only reports that SDL2 is missing.
//
// Why a separate process: on macOS a window has to live on the main thread, which here belongs to the terminal UI; a
// window that is dragged or minimised on Windows stalls its own message loop, which must never stall the UI or the
// audio; and a graphics driver problem can at worst close the scope window, not the player.
//
// Data path: the audio callback (player or radio) copies what went to the device into a lock-free ring
// (scope_feed_push: one atomic load while the window is closed); a sender thread moves it every few milliseconds through
// a pipe to the window process, together with the scope settings (ScopeWinConfig) whenever they change. The window
// draws EVERY sample once, as a beam that leaves phosphor afterglow, at the monitor's refresh rate.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include "spectrogram.h"

namespace muisc {

// Everything the window needs to look like the scope in the terminal (image style, or braille style: braille = 1). Sent as raw bytes: both ends are the same binary.
struct ScopeWinConfig {
    uint32_t magic = 0x4F43534D;   // "MSCO"
    int32_t rate = 48000;          // sample rate of the samples that follow
    float decay = 0.80f;           // afterglow per 1/30 s (the SHIFT+o "Decay")
    float glow = 0.60f;            // bloom 0..1
    float z_depth = 0.70f;
    uint8_t z = 0;                 // beam intensity follows Z
    uint8_t z_source = 0;          // 0 = beam speed, 1 = level
    uint8_t rotate = 0;            // 45 degrees (mid / side)
    uint8_t mono_phase = 1;        // near-mono signals become a phase portrait
    uint8_t interp = 1;            // 1 = lines between the samples, 0 = dots
    uint8_t color_by_x = 1;        // 1 = palette across the width (the "gradient" colour), 0 = by beam speed
    uint8_t music = 0;             // oscilloscope music mode: fixed scale, finer beam, more smoothing, no phase portrait
    uint8_t braille = 0;           // 1 = the braille style: the terminal scope's dot grid, scaled up (else the smooth image style)
    uint8_t mode = 0;              // braille style: 0 = XY, 1 = YT, 2 = both overlapping, 3 = both in two rows (SHIFT+o "Scope mode"); modes other than XY are drawn as a fine dot picture in either style
    float dot_threshold = 0.28f;   // braille: brightness a dot needs to light (SHIFT+o "Dot threshold")
    float tail = 0.45f;            // braille: oldest sample of the trace relative to the newest (SHIFT+o "Tail")
    int32_t trace = 1024;          // braille: samples drawn per frame (SHIFT+o "Trace Length")
    uint8_t crt = 1;               // CRT beam (light by dwell time), with or without music mode; 0 = the classic look
    uint8_t low_rate = 1;          // music mode, CRT beam: below 96 kHz the jumps between shapes are faded out
    uint8_t dwell = 1;             // CRT beam: light by dwell time; 0 = every stroke equally bright
    uint8_t rough = 0;             // not music mode: size from the previous frame's peak (loud beats clip along the edges)
    uint8_t pal[256][3] = {};      // palette, index 0..255
    char title[96] = {};           // window title (track / station)
};

// The spectrogram window's settings: the spectrogram options (SHIFT+i) as plain numbers, the colours of the scheme
// in use (the "gradient" one included) and the title. Sent as raw bytes like ScopeWinConfig.
struct SpectroWinConfig {
    uint32_t magic = 0x5053534D;   // "MSSP"
    int32_t rate = 48000;          // sample rate of the samples that follow
    int32_t v[24] = {};            // SpectroSettings, see spectro_win_pack()
    uint8_t pal[256][3] = {};
    char title[96] = {};
};
inline void spectro_win_pack(const SpectroSettings& s, SpectroWinConfig& c) {
    const int32_t x[24] = {s.style, s.motion, s.scale, s.min_freq, s.max_freq, s.gain, s.range, s.freq_gain, s.window_log2,
                           s.window_type, s.zero_pad, s.scheme, s.channels, s.span, s.labels ? 1 : 0, s.view, s.wf_graphs, s.wf_spacing,
                           s.wf_slant, s.wf_height, s.wf_detail, 0, 0, 0};
    for (int i = 0; i < 24; ++i) c.v[i] = x[i];
}
inline SpectroSettings spectro_win_unpack(const SpectroWinConfig& c) {
    SpectroSettings s;
    s.style = c.v[0]; s.motion = c.v[1]; s.scale = c.v[2]; s.min_freq = c.v[3]; s.max_freq = c.v[4]; s.gain = c.v[5];
    s.range = c.v[6]; s.freq_gain = c.v[7]; s.window_log2 = c.v[8]; s.window_type = c.v[9]; s.zero_pad = c.v[10];
    s.scheme = c.v[11]; s.channels = c.v[12]; s.span = c.v[13]; s.labels = c.v[14] != 0;
    s.view = c.v[15] == 1 ? 1 : 0;
    if (s.view == 1) {
        s.wf_graphs = std::clamp<int>(c.v[16], 8, 100); s.wf_spacing = std::clamp<int>(c.v[17], 20, 500);
        s.wf_slant = std::clamp<int>(c.v[18], 0, 40); s.wf_height = std::clamp<int>(c.v[19], 10, 60);
        s.wf_detail = c.v[20] == 1 ? 1 : 0;
        s.wf_smooth = true;   // the window moves the graphs with the monitor's refresh
    }
    return s;
}

// Can the window be opened at all (SDL2 found)? `why` gets a short reason when not.
bool scope_window_available(std::string* why);
// Opens / closes the window. open() returns false with `err` set when it cannot start.
bool scope_window_open(std::string* err);
void scope_window_close();
// True while the window process runs. Notices a window closed by the user (its X, ESC or q).
bool scope_window_running();
// Non-empty once after the window ended with an error (e.g. no usable graphics driver).
std::string scope_window_take_error();
// The scope settings / colours / title; cheap to call every frame (only a change is sent).
void scope_window_set_config(const ScopeWinConfig& c);
// Audio thread: what went to the device, interleaved L/R. Does nothing while the window is closed.
void scope_feed_push(const float* interleaved_lr, size_t frames, int rate);

// Entry point of the window process (`mousiki --scope-window`). Returns the process exit code.
int scope_window_main();

// The spectrogram window (SHIFT+8): the same, for the spectrogram. It analyses the decoded signal itself (like the
// spectrogram in the terminal: before EQ, normalization and volume) and scrolls smoothly at the monitor's refresh rate.
bool spectro_window_open(std::string* err);
void spectro_window_close();
bool spectro_window_running();
std::string spectro_window_take_error();
void spectro_window_set_config(const SpectroWinConfig& c);
// Audio thread: the decoded signal, interleaved L/R. Does nothing while the window is closed.
void spectro_window_feed_push(const float* interleaved_lr, size_t frames, int rate);
bool spectro_window_feed_active();
// Entry point of the window process (`mousiki --spectro-window`).
int spectro_window_main();

} // namespace muisc
