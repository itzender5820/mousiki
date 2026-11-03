// The Hi-Fi spectrum bars: a stereo FFT of the samples that are played, turned into columns of braille dots.
#include "hifi_bars.h"
#include "kiss_fftr.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace muisc {

long long HifiBars::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

HifiBars::HifiBars() {
    cfg_ = kiss_fftr_alloc(kN, 0, nullptr, nullptr);
    ring_[0].assign(kN, 0.0f);
    ring_[1].assign(kN, 0.0f);
}

HifiBars::~HifiBars() { if (cfg_) std::free(cfg_); }

bool HifiBars::active() const { return now_ms() - used_ms_.load(std::memory_order_relaxed) < 500; }

void HifiBars::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::fill(ring_[0].begin(), ring_[0].end(), 0.0f);
    std::fill(ring_[1].begin(), ring_[1].end(), 0.0f);
    write_ = 0;
    for (auto& s : state_) s.clear();
    spec_ms_ = -1;
}

void HifiBars::push(const float* lr, size_t frames, int rate) {
    if (!active() || !lr) return;
    std::lock_guard<std::mutex> lk(mtx_);
    rate_ = rate > 0 ? rate : 48000;
    for (size_t i = 0; i < frames; ++i) {
        ring_[0][static_cast<size_t>(write_)] = lr[2 * i];
        ring_[1][static_cast<size_t>(write_)] = lr[2 * i + 1];
        write_ = (write_ + 1) % kN;
    }
}

// The magnitudes (in units of a full-scale sine = 1) of both channels over the last kN samples.
void HifiBars::analyse() {
    std::vector<float> a[2];
    int rate;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (int c = 0; c < 2; ++c) {
            a[c].resize(kN);
            for (int i = 0; i < kN; ++i) a[c][static_cast<size_t>(i)] = ring_[c][static_cast<size_t>((write_ + i) % kN)];
        }
        rate = rate_;
    }
    (void)rate;
    static std::vector<float> win;
    if (win.empty()) {
        win.resize(kN);
        for (int i = 0; i < kN; ++i) win[static_cast<size_t>(i)] = 0.5f * (1.0f - std::cos(2.0f * 3.14159265f * static_cast<float>(i) / (kN - 1)));
    }
    std::vector<kiss_fft_cpx> fo(kN / 2 + 1);
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < kN; ++i) a[c][static_cast<size_t>(i)] *= win[static_cast<size_t>(i)];
        kiss_fftr(static_cast<kiss_fftr_cfg>(cfg_), a[c].data(), fo.data());
        spec_[c].resize(kN / 2 + 1);
        for (int k = 0; k <= kN / 2; ++k)
            spec_[c][static_cast<size_t>(k)] = std::sqrt(fo[static_cast<size_t>(k)].r * fo[static_cast<size_t>(k)].r + fo[static_cast<size_t>(k)].i * fo[static_cast<size_t>(k)].i) / (kN / 4.0f);
    }
}

namespace {
float mel_of(float f) { return 2595.0f * std::log10(1.0f + f / 700.0f); }
float hz_of(float m) { return 700.0f * (std::pow(10.0f, m / 2595.0f) - 1.0f); }
// the frequency scales of the spectrogram options: 0 linear, 1 logarithmic, 2 mel, 3 bark, 4 erb
float warp_of(int scale, float f) {
    switch (scale) {
        case 0: return f;
        case 1: return std::log(std::max(f, 1.0f));
        case 3: return 13.0f * std::atan(0.00076f * f) + 3.5f * std::atan((f / 7500.0f) * (f / 7500.0f));
        case 4: return 21.4f * std::log10(1.0f + 0.00437f * f);
        default: return mel_of(f);
    }
}
float unwarp_of(int scale, float w, float lo, float hi) {   // the frequency in [lo, hi] whose warp is w (the scales rise steadily)
    if (scale == 2) return hz_of(w);
    if (scale == 0) return w;
    if (scale == 1) return std::exp(w);
    for (int i = 0; i < 40; ++i) { const float mid = 0.5f * (lo + hi); (warp_of(scale, mid) < w ? lo : hi) = mid; }
    return 0.5f * (lo + hi);
}
}

const std::vector<float>& HifiBars::bands(int channel, int n, double dt, int fluidity, int degradation, int viscosity) {
    used_ms_.store(now_ms(), std::memory_order_relaxed);
    n = std::max(1, n);
    // one analysis per frame (the left, right and mix calls of one frame share it)
    const long long t = now_ms();
    if (spec_ms_ < 0 || t - spec_ms_ >= 2 || spec_[0].empty()) { analyse(); spec_ms_ = t; }
    int rate;
    { std::lock_guard<std::mutex> lk(mtx_); rate = rate_; }
    const float nyq = static_cast<float>(rate) / 2.0f;
    const float fmax = std::min(18000.0f, nyq * 0.98f);
    const int fsc = std::clamp(fscale_, 0, 4);
    const float m0 = warp_of(fsc, 30.0f), m1 = warp_of(fsc, fmax);
    const float lim_lo = 30.0f, lim_hi = fmax;
    const float bin_hz = static_cast<float>(rate) / kN;
    std::vector<float> target(static_cast<size_t>(n), 0.0f);
    auto mag_at = [&](float f) {   // the magnitude at a frequency, between two bins
        const float x = f / bin_hz;
        const int k = std::clamp(static_cast<int>(x), 0, kN / 2 - 1);
        const float fr = std::clamp(x - static_cast<float>(k), 0.0f, 1.0f);
        const auto sample = [&](int kk) {
            return channel == 2 ? 0.5f * (spec_[0][static_cast<size_t>(kk)] + spec_[1][static_cast<size_t>(kk)]) : spec_[channel][static_cast<size_t>(kk)];
        };
        return sample(k) * (1.0f - fr) + sample(k + 1) * fr;
    };
    for (int i = 0; i < n; ++i) {
        const float fa = unwarp_of(fsc, m0 + (m1 - m0) * static_cast<float>(i) / n, lim_lo, lim_hi);
        const float fb = unwarp_of(fsc, m0 + (m1 - m0) * static_cast<float>(i + 1) / n, lim_lo, lim_hi);
        float mag;
        if (fb - fa < bin_hz) mag = mag_at(0.5f * (fa + fb));
        else {   // the loudest bin of the band
            mag = 0.0f;
            const int ka = std::clamp(static_cast<int>(fa / bin_hz), 0, kN / 2), kb = std::clamp(static_cast<int>(fb / bin_hz), 0, kN / 2);
            for (int k = ka; k <= kb; ++k) {
                const float v = channel == 2 ? 0.5f * (spec_[0][static_cast<size_t>(k)] + spec_[1][static_cast<size_t>(k)]) : spec_[channel][static_cast<size_t>(k)];
                mag = std::max(mag, v);
            }
        }
        const float f = 0.5f * (fa + fb);
        const float db = 20.0f * std::log10(mag + 1e-7f) + 3.0f * std::max(0.0f, std::log2(f / 300.0f));   // a tilt, so the highs show
        const float rg = std::max(10.0f, range_);
        target[static_cast<size_t>(i)] = std::clamp((db + gain_ + 12.0f + rg) / rg, 0.0f, 1.0f);   // the top stays at -12 dB (minus the gain)
    }
    // the motion: rise and fall with their own time constants, then a blend with the neighbours
    auto& st = state_[channel];
    if (static_cast<int>(st.size()) != n) st.assign(static_cast<size_t>(n), 0.0f);
    const float tr = static_cast<float>(std::clamp(fluidity, 1, 10) - 1) / 9.0f;
    const float tf = static_cast<float>(std::clamp(degradation, 1, 10) - 1) / 9.0f;
    const float tau_rise = 0.004f + tr * 0.11f;          // seconds: 4 ms (snappy) .. 0.11 s (floaty)
    const float tau_fall = 0.03f + (1.0f - tf) * 0.45f;  // 0.03 s (instant) .. 0.48 s (slow fade)
    const float d = static_cast<float>(std::clamp(dt, 0.001, 0.2));
    const float ka = 1.0f - std::exp(-d / tau_rise), kf = 1.0f - std::exp(-d / tau_fall);
    for (int i = 0; i < n; ++i) {
        float& s = st[static_cast<size_t>(i)];
        const float tg = target[static_cast<size_t>(i)];
        s += (tg - s) * (tg > s ? ka : kf);
    }
    out_ = st;
    const float b = static_cast<float>(std::clamp(viscosity, 0, 10)) / 10.0f * 0.5f;
    if (b > 0.0f && n > 2) {
        for (int i = 0; i < n; ++i) {
            const float l = st[static_cast<size_t>(std::max(0, i - 1))], r = st[static_cast<size_t>(std::min(n - 1, i + 1))];
            out_[static_cast<size_t>(i)] = st[static_cast<size_t>(i)] * (1.0f - b) + 0.5f * (l + r) * b;
        }
    }
    return out_;
}

HifiBars& hifi() {
    static HifiBars h;
    return h;
}
void hifi_feed_push(const float* lr, size_t frames, int rate) { hifi().push(lr, frames, rate); }
bool hifi_feed_active() { return hifi().active(); }

std::vector<std::string> hifi_rows(HifiBars& h, const HifiView& v, const std::function<std::string(float)>& gradient,
                                   const std::function<std::string(float)>& by_height) {
    h.set_analysis(v.fscale, static_cast<float>(v.gain), static_cast<float>(v.range));
    const int cells = std::max(0, v.cells);
    const int R = std::clamp(v.rows, 1, 8);
    std::vector<std::string> rows(static_cast<size_t>(R));
    if (cells == 0) return rows;
    const int H = 4 * R;                       // dots in a column
    std::vector<int> dots(static_cast<size_t>(2 * cells), 0);   // the lit height (0 .. H) of every dot column, left to right
    static const float kGamma[4] = {1.0f, 1.7f, 2.6f, 3.6f};
    float gamma = kGamma[std::clamp(v.scale, 0, 3)];
    if (v.adapt == 1) {   // dense, compressed music fills the whole range: bend more, so that the mean level stays near 30 %
        const float a = std::max(h.adapt_avg, 0.05f);
        gamma = std::min(5.0f, gamma * std::clamp(std::log(0.3f) / std::log(std::min(a, 0.95f)), 1.0f, 3.5f));
    }
    float sum = 0.0f; int cnt = 0;
    auto fill = [&](const std::vector<float>& lv, int first, int count, bool reversed) {   // lv: one level per band; count dot columns from `first`
        const int nb = static_cast<int>(lv.size());
        for (int j = 0; j < count; ++j) {
            const int b = v.resolution == 1 ? j / 2 : j;
            float x = lv[static_cast<size_t>(reversed ? nb - 1 - b : b)];
            sum += x; ++cnt;
            if (gamma != 1.0f) x = std::pow(x, gamma);
            dots[static_cast<size_t>(first + j)] = static_cast<int>(std::lround(x * static_cast<float>(H)));
        }
    };
    const bool stereo = v.channels == 0 && cells >= 2;
    if (!stereo) {
        const int nb = v.resolution == 1 ? cells : 2 * cells;
        fill(h.bands(2, nb, v.dt, v.fluidity, v.degradation, v.viscosity), 0, 2 * cells, false);
    } else {
        // the left channel in the left half with its bass at the middle (the treble at the left edge), the right channel mirrored
        const int cl = cells / 2, cr = cells - cl;
        const int nl = v.resolution == 1 ? cl : 2 * cl, nr = v.resolution == 1 ? cr : 2 * cr;
        fill(h.bands(0, nl, v.dt, v.fluidity, v.degradation, v.viscosity), 0, 2 * cl, true);
        // one empty dot column between the channels
        const int gap = v.split == 1 ? 1 : 0;
        fill(h.bands(1, nr, v.dt, v.fluidity, v.degradation, v.viscosity), 2 * cl + gap, 2 * cr - gap, false);
    }
    if (cnt > 0 && sum / static_cast<float>(cnt) > 0.02f)   // (silence leaves the mean as it is)
        h.adapt_avg += (sum / static_cast<float>(cnt) - h.adapt_avg) * (1.0f - std::exp(-static_cast<float>(v.dt) / 2.0f));
    static const int kBit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    for (int r = 0; r < R; ++r) {
        const int rb = R - 1 - r;              // the row counted from the bottom
        std::string line, last;
        for (int c = 0; c < cells; ++c) {
            int bits = 0, top = 0;
            for (int col = 0; col < 2; ++col) {
                const int k = std::clamp(dots[static_cast<size_t>(2 * c + col)] - rb * 4, 0, 4);   // dots lit in this cell, from its bottom
                for (int row = 4 - k; row < 4; ++row) bits |= kBit[row][col];
                top = std::max(top, k);
            }
            if (bits == 0) { line += ' '; continue; }
            // "corresponding": the colour is the one of the LEVEL at the highest lit dot of the cell (a step per dot line, as far as a cell
            // can hold one colour). The height of a dot is bent by the Height curve, so the level it stands for is height ^ (1 / gamma):
            // the colours keep their meaning (like the waterfall's) whatever Height is set to.
            const float y = (static_cast<float>(rb * 4 + top) - 0.5f) / static_cast<float>(H);
            const std::string ansi = v.color == 1 ? by_height(std::clamp(std::pow(std::clamp(y, 0.0f, 1.0f), 1.0f / gamma), 0.0f, 1.0f))
                                                  : gradient(cells > 1 ? static_cast<float>(c) / static_cast<float>(cells - 1) : 0.0f);
            if (ansi != last) { line += ansi; last = ansi; }
            const int cp = 0x2800 + bits;
            line += static_cast<char>(0xE0 | (cp >> 12));
            line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            line += static_cast<char>(0x80 | (cp & 0x3F));
        }
        line += "\x1b[0m";
        rows[static_cast<size_t>(r)] = line;
    }
    return rows;
}

} // namespace muisc
