#pragma once
#include <array>
#include <mutex>
#include <vector>

namespace muisc {

// Short-time FFT spectrum analyzer for the small visualizer strip under
// the metadata panel.
//
// This is fed LIVE by whatever's actually coming out of the speaker:
// Player::data_callback() calls push_samples() with each chunk it plays,
// into a small ring buffer, and compute_bars() reads straight off that
// ring. It never touches the full decoded track and never needs to know
// the playback position — which means it works identically whether the
// track finished decoding in 200ms or is still streaming in on slow
// hardware, and it can't block on anything the decode thread is doing.
// (Ported from a reference implementation the user provided — same
// ring-buffer-fed design, same log-spaced/mirrored band layout — adapted
// to use kiss_fftr, which this project already links, instead of adding
// the plain complex kiss_fft as a second FFT backend for no real benefit
// on a real-valued signal.)
class FftVisualizer {
public:
    FftVisualizer();
    ~FftVisualizer();

    FftVisualizer(const FftVisualizer&) = delete;
    FftVisualizer& operator=(const FftVisualizer&) = delete;

    // Called from the AUDIO CALLBACK thread, once per chunk it processes.
    // Downmixes to mono if needed by the caller before calling this (the
    // player is already mono end-to-end, so this expects mono frames).
    void push_samples(const float* samples, size_t count, int sample_rate);

    // Called from the main/render thread. Returns `num_bars` levels in
    // [0,8] — one glyph pair worth of range per bar (bottom row gets
    // min(level,4), top row gets the overflow above 4), rendered with the
    // ⣀⣤⣶⣿ glyph set in app.cpp.
    //
    // Motion is a lightweight spring-damper approximation of "fluid"
    // behavior rather than a real fluid simulation: each bar is pulled
    // toward its target with momentum (spring_k_), resisted by a
    // viscosity-like damping term, and blended slightly toward its
    // neighbors each frame (surface tension). `dt` is the real elapsed
    // time since the last call, so motion speed doesn't depend on how
    // often the caller happens to redraw.
    std::vector<int> compute_bars(int num_bars, double dt);

    // 1 (snappy) .. 10 (very fluid/viscous). Matches the settings panel's
    // "Visualizer fluidity" slider. Controls the RISE only now -- fall
    // speed and neighbor-blending are separate knobs (see below), since
    // coupling all three to one slider was what made high-fluidity
    // settings feel unresponsive to sudden drops (a loud->silent moment
    // still fell at a fluid-and-heavily-blended pace instead of snapping
    // down while the rise stayed floaty).
    void set_fluidity(int level);

    // 1 (instant cutoff) .. 10 (slow fade). How fast a bar releases back
    // down once its target drops below its current height. Independent
    // of fluidity so a slow, floaty rise can still pair with a fast,
    // percussive fall -- which is how real audio meters read as
    // "responsive" despite also looking smooth.
    void set_degradation_speed(int level);

    // 0 (bars move independently) .. 10 (heavy neighbor blending / surface
    // tension). Was previously derived from the fluidity level; exposed
    // directly now so a single bar's drop isn't visually propped up by
    // its still-elevated neighbors.
    void set_viscosity(int level);

    // Clears the ring and motion state — call when a new track starts so
    // the old track's tail doesn't linger into the new one's first frame.
    void reset();

private:
    static constexpr int kFftSize = 2048;
    static constexpr int kMaxBands = 32;

    std::mutex mtx_; // guards ring_/ring_write_/sample_rate_ across the audio-callback/render threads
    std::array<float, kFftSize> ring_{};
    int ring_write_ = 0;
    int sample_rate_ = 44100;

    void* cfg_ = nullptr; // kiss_fftr_cfg
    std::array<float, kMaxBands> smooth_bands_{}; // 0..70 (dB-ish range, pre-boost) — render thread only, no lock needed

    void compute_bands_locked(const std::array<float, kFftSize>& ring_copy, int write_pos, int sample_rate);

    // Motion state (spring-damper + neighbor blend) applied on top of the
    // 32-band snapshot when downsampling to `num_bars` in compute_bars().
    std::vector<float> smoothed_bars_;
    std::vector<float> velocity_;
    float spring_k_ = 50.0f;
    float damping_ = 0.25f;
    float viscosity_ = 0.05f;
    float release_k_ = 30.0f; // fall/degradation speed, independent of spring_k_ (rise)
};

} // namespace muisc
