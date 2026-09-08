#include "fft_visualizer.h"
#include "kiss_fftr.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace muisc {

FftVisualizer::FftVisualizer() {
    cfg_ = kiss_fftr_alloc(kFftSize, 0, nullptr, nullptr);
}

FftVisualizer::~FftVisualizer() {
    if (cfg_) std::free(cfg_);
}

void FftVisualizer::set_fluidity(int level) {
    level = std::clamp(level, 1, 10);
    float t = static_cast<float>(level - 1) / 9.0f;
    // Rise-only now (see header comment for why fall/blend were split
    // out). spring_k*dt ~= 0.032 at level 10 at dt~0.08s, meaning it
    // takes many frames to close the gap, with damping low enough
    // (0.06) to let real overshoot happen rather than just slowly
    // creeping — that's what reads as "fluid" rather than merely
    // "smoothed".
    spring_k_ = 55.0f - t * 51.0f;    // 55 (snappy) -> 4 (very fluid)
    damping_ = 0.35f - t * 0.29f;     // 0.35 -> 0.06: less resistance to velocity = more sustained motion
}

void FftVisualizer::set_degradation_speed(int level) {
    level = std::clamp(level, 1, 10);
    float t = static_cast<float>(level - 1) / 9.0f;
    // Inverted from fluidity's scale on purpose: level 1 = slow fade
    // (matches an old-school VU meter's decay), level 10 = near-instant
    // cutoff. release_k of 15 barely outpaces a mid-fluidity rise;
    // 140 snaps a full-height bar to zero in a couple of frames.
    release_k_ = 15.0f + t * 125.0f;  // 15 (slow fade) -> 140 (instant)
}

void FftVisualizer::set_viscosity(int level) {
    level = std::clamp(level, 0, 10);
    viscosity_ = static_cast<float>(level) / 10.0f * 0.7f; // 0 -> 0.7, matches the old derived range's top end
}

void FftVisualizer::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    ring_.fill(0.0f);
    ring_write_ = 0;
    smooth_bands_.fill(0.0f);
    smoothed_bars_.clear();
    velocity_.clear();
}

// Ported from the user-provided reference (compute_bands_locked): 16
// log-spaced frequency edges -> 16 half-bands -> mirrored to 32 total, so
// bass sits in the CENTER of the array and treble falls off toward both
// edges — this mirroring is also what gives compute_bars() its
// bass-center/treble-edges symmetry for free, without needing separate
// distance-from-center logic. Swapped their plain kiss_fft (complex) for
// kiss_fftr (real-input) since the signal is real and this project
// already links kiss_fftr; the band math is otherwise unchanged. Now
// runs on the render thread only (see push_samples), operating on a
// snapshot rather than the live ring, so it needs no lock of its own.
void FftVisualizer::compute_bands_locked(const std::array<float, kFftSize>& ring_copy, int write_pos, int sr) {
    std::array<float, kFftSize> windowed{};
    for (int i = 0; i < kFftSize; ++i) {
        int idx = (write_pos + i) % kFftSize;
        float w = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (kFftSize - 1)));
        windowed[i] = ring_copy[idx] * w;
    }

    int usable_bins = kFftSize / 2;
    std::vector<kiss_fft_cpx> spectrum(usable_bins + 1);
    kiss_fftr(static_cast<kiss_fftr_cfg>(cfg_), windowed.data(), spectrum.data());

    const float norm_ref = static_cast<float>(kFftSize) * static_cast<float>(kFftSize);
    const float fr = static_cast<float>(sr) / static_cast<float>(kFftSize);

    static constexpr float EDGES[17] = {
        30,   50,   80,   120,  180,  250,  350,  500,
        700, 1000, 1400, 2000, 2800, 4000, 5600, 8000, 12000
    };
    constexpr int HALF = kMaxBands / 2; // 16

    float right[HALF] = {};
    for (int b = 0; b < HALF; ++b) {
        int lo = std::clamp(static_cast<int>(EDGES[b] / fr), 1, usable_bins - 1);
        int hi = std::clamp(static_cast<int>(EDGES[b + 1] / fr), lo + 1, usable_bins);
        float e = 0.0f;
        for (int k = lo; k < hi; ++k) {
            float re = spectrum[k].r, im = spectrum[k].i;
            e += re * re + im * im;
        }
        e /= static_cast<float>(hi - lo);
        float db = 10.0f * std::log10(e / norm_ref + 1e-12f);
        right[b] = std::clamp(db + 70.0f, 0.0f, 70.0f);
    }

    float full[kMaxBands];
    for (int i = 0; i < HALF; ++i) full[i] = right[HALF - 1 - i];       // descending -> bass toward center
    for (int i = 0; i < HALF; ++i) full[HALF + i] = right[i];           // ascending -> bass toward center

    // Light fixed smoothing at the band-acquisition stage (matches the
    // reference's own UP/UP-DOWN constants at density~5); the real
    // "fluidity" motion the user controls via the settings slider happens
    // downstream in compute_bars()'s spring-damper step.
    for (int b = 0; b < kMaxBands; ++b) {
        float a = (full[b] > smooth_bands_[b]) ? 0.55f : 0.30f;
        smooth_bands_[b] = smooth_bands_[b] * (1.0f - a) + full[b] * a;
    }
}

void FftVisualizer::push_samples(const float* samples, size_t count, int sample_rate) {
    // Deliberately cheap: just ring writes under a short lock. The actual
    // FFT used to run right here, on the AUDIO CALLBACK thread — which is
    // real-time-sensitive and gets invoked far more often than the ~80ms
    // render tick actually needs fresh spectrum data. Moved the FFT work
    // itself into compute_bars() (below), which runs on the render
    // thread instead, so the audio thread never risks stalling on it.
    std::lock_guard<std::mutex> lk(mtx_);
    sample_rate_ = sample_rate > 0 ? sample_rate : sample_rate_;
    for (size_t i = 0; i < count; ++i) {
        ring_[ring_write_] = samples[i];
        ring_write_ = (ring_write_ + 1) % kFftSize;
    }
}

std::vector<int> FftVisualizer::compute_bars(int num_bars, double dt) {
    std::vector<int> bars(std::max(0, num_bars), 0);
    if (num_bars <= 0) return bars;

    // Snapshot the ring, then release the lock before doing the FFT —
    // this keeps the critical section as short as the audio thread's own
    // (just a memcpy-sized copy), so there's no meaningful contention
    // between the two threads either way.
    std::array<float, kFftSize> ring_copy{};
    int write_pos;
    int sr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ring_copy = ring_;
        write_pos = ring_write_;
        sr = sample_rate_;
    }
    compute_bands_locked(ring_copy, write_pos, sr); // "_locked" name is legacy; no lock needed here anymore — only the render thread ever calls this
    std::array<float, kMaxBands> bands_snapshot = smooth_bands_;

    // Sensitivity boost (pow < 1 is >= x for x in [0,1], so this is
    // guaranteed never less sensitive than a bare linear mapping at any
    // input level — verified against the -3..-60dB range before shipping
    // it earlier this session). LINEAR INTERPOLATION between the two
    // nearest bands (not nearest-neighbor rounding) — with up to 48 bars
    // reading from only 32 underlying bands, rounding to the nearest one
    // made several adjacent bars land on the exact same value, showing
    // as flat "steps" instead of a smooth curve between bars.
    std::vector<float> targets(num_bars);
    for (int i = 0; i < num_bars; ++i) {
        float t = (num_bars > 1) ? static_cast<float>(i) / static_cast<float>(num_bars - 1) : 0.0f;
        float pos = t * (kMaxBands - 1);
        int idx0 = std::clamp(static_cast<int>(std::floor(pos)), 0, kMaxBands - 1);
        int idx1 = std::min(idx0 + 1, kMaxBands - 1);
        float frac = pos - idx0;
        float band_val = bands_snapshot[idx0] * (1.0f - frac) + bands_snapshot[idx1] * frac;
        float raw = std::clamp(band_val / 70.0f, 0.0f, 1.0f);

        // Edge envelope: both ends of the strip are pulled toward the
        // baseline regardless of actual band energy there, so the whole
        // shape always reads as a single mountain rising from zero at
        // both edges rather than the edges being able to spike up
        // independently. Half-sine — 0 at i=0 and i=num_bars-1, 1 at the
        // center — applied as a multiplier on top of the boost curve.
        float envelope = (num_bars > 1)
            ? std::sin(3.14159265f * static_cast<float>(i) / static_cast<float>(num_bars - 1))
            : 1.0f;

        targets[i] = std::pow(raw, 0.35f) * envelope;
    }

    if (static_cast<int>(smoothed_bars_.size()) != num_bars) {
        smoothed_bars_.assign(num_bars, 0.0f);
        velocity_.assign(num_bars, 0.0f);
    }
    float dtf = static_cast<float>(std::clamp(dt, 0.0, 0.5));

    // Spring-damper step (momentum) — but with a MINIMUM release speed
    // enforced when falling. The fluidity slider's low spring_k/damping
    // at high settings gave a nice floaty rise, but applied the same way
    // to a falling bar meant it could take many seconds to settle back
    // down after a peak — "rarely has a chance to come down". Real audio
    // meters read as natural because they fall faster than they rise;
    // release_k_ (independent of fluidity's spring_k_ now) enforces that.
    for (int i = 0; i < num_bars; ++i) {
        bool falling = targets[i] < smoothed_bars_[i];
        // On a sudden drop, a bar that was still rising carries leftover
        // upward velocity — the increment-based integration below would
        // otherwise spend several frames just canceling that out before
        // the fall even starts, which is exactly the "sluggish on drops"
        // feel. Cutting most of it immediately on the rise->fall flip
        // lets the release force take over right away instead.
        if (falling && velocity_[i] > 0.0f) velocity_[i] *= 0.15f;
        float k = falling ? release_k_ : spring_k_;
        velocity_[i] += (targets[i] - smoothed_bars_[i]) * k * dtf;
        velocity_[i] *= (1.0f - std::clamp(damping_, 0.0f, 0.95f));
        smoothed_bars_[i] = std::clamp(smoothed_bars_[i] + velocity_[i] * dtf, 0.0f, 1.0f);
    }

    // Neighbor-blend pass (surface tension).
    if (viscosity_ > 0.0f && num_bars > 2) {
        std::vector<float> blended = smoothed_bars_;
        for (int i = 0; i < num_bars; ++i) {
            float left = (i > 0) ? smoothed_bars_[i - 1] : smoothed_bars_[i];
            float right = (i < num_bars - 1) ? smoothed_bars_[i + 1] : smoothed_bars_[i];
            blended[i] = smoothed_bars_[i] * (1.0f - viscosity_) + (left + right) * 0.5f * viscosity_;
        }
        smoothed_bars_.swap(blended);
    }

    // Force the two true edge bars to baseline after blending —
    // blending alone would let bar 0 drift toward bar 1's (possibly
    // nonzero) value, undermining "edges never leave bottom". Everything
    // else in between reaches its envelope-scaled taper naturally through
    // the spring converging toward its (already envelope-scaled) target,
    // so only the literal edges need forcing here.
    if (num_bars > 0) {
        smoothed_bars_[0] = 0.0f;
        smoothed_bars_[num_bars - 1] = 0.0f;
    }

    for (int i = 0; i < num_bars; ++i) {
        bars[i] = static_cast<int>(std::round(std::clamp(smoothed_bars_[i], 0.0f, 1.0f) * 8.0f));
    }
    return bars;
}

} // namespace muisc
