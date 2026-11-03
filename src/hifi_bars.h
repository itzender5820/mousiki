// The Hi-Fi spectrum bars (hifi_bars.cpp): a live stereo spectrum drawn as braille columns, one dot column per frequency step.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace muisc {

// Fed by the audio thread with the stereo frames that are played (the same feed as the spectrogram's), read by the render thread
// once per frame. The analysis is its own small FFT: it follows the frame rate of the program, not the spectrogram's settings.
class HifiBars {
public:
    HifiBars();
    ~HifiBars();
    HifiBars(const HifiBars&) = delete;
    HifiBars& operator=(const HifiBars&) = delete;

    // Audio thread (interleaved stereo). Does nothing unless the bars were drawn a moment ago.
    void push(const float* lr, size_t frames, int rate);
    // True while a frame has drawn the bars in the last half second (the audio thread feeds only then).
    bool active() const;
    // Clears the samples and the motion (a new track).
    void reset();
    // The analysis scale (render thread): the frequency scale (0 linear, 1 logarithmic, 2 mel, 3 bark, 4 erb), a gain in dB (moves the
    // levels up or down) and the range in dB shown from the bottom to the top (smaller = steeper).
    void set_analysis(int fscale, float gain_db, float range_db) { fscale_ = fscale; gain_ = gain_db; range_ = range_db; }
    float adapt_avg = 0.3f;   // the slowly moving mean level of the spectrum (for the adaptive height)

    // Render thread: `n` bands from low to high of one channel (0 left, 1 right, 2 the mix of both), 0..1 each, with the motion
    // (rise / fall / neighbour blending, the three bar settings) applied. Marks the bars as in use.
    const std::vector<float>& bands(int channel, int n, double dt, int fluidity, int degradation, int viscosity);

private:
    int fscale_ = 2;
    float gain_ = 0.0f, range_ = 68.0f;
    static constexpr int kN = 4096;
    std::mutex mtx_;
    std::vector<float> ring_[2];
    int write_ = 0;
    int rate_ = 48000;
    std::atomic<long long> used_ms_{-100000};
    void* cfg_ = nullptr;
    std::vector<float> state_[3];
    std::vector<float> out_;
    std::vector<float> spec_[2];     // the magnitudes of the last analysis of channel 0 / 1
    long long spec_ms_ = -1;
    void analyse();
    static long long now_ms();
};

HifiBars& hifi();
// Audio thread: forwards to hifi().push().
void hifi_feed_push(const float* lr, size_t frames, int rate);
bool hifi_feed_active();

// What the bars look like (the three motion settings come with the call to bands()).
struct HifiView {
    int cells = 0;        // width in braille cells
    int rows = 2;         // height in text rows (2 .. 4); every row has 4 dots
    int resolution = 0;   // 0 fine (a band per dot column), 1 rough (a band per two dot columns = one cell)
    int channels = 0;     // 0 stereo, 1 mono
    int adapt = 0;        // 1: the height adapts to the song (dense, compressed music is lowered more)
    int split = 1;        // 1: one empty dot column between the two channels
    int fscale = 2;       // the frequency scale: 0 linear, 1 logarithmic, 2 mel, 3 bark, 4 erb
    int gain = 0;         // dB, -20 .. 20
    int range = 68;       // dB from the bottom to the top of the bars, 40 .. 100
    int scale = 1;        // the height: 0 full, 1 medium, 2 low (a curve that keeps the peaks visible but lowers the middle levels)
    int color = 0;        // 0 gradient (left to right), 1 corresponding (the colours of the spectrogram, by height)
    int fluidity = 10, degradation = 8, viscosity = 3;
    double dt = 0.033;
};
// The `rows` text rows of the bars, each `cells` cells wide, with their colours. `gradient(t)` gives the ANSI colour for a place
// t = 0..1 from left to right, `by_height(f)` the one for a height f = 0..1 (both return the escape sequence that sets the colour).
std::vector<std::string> hifi_rows(HifiBars& h, const HifiView& v, const std::function<std::string(float)>& gradient,
                                   const std::function<std::string(float)>& by_height);

} // namespace muisc
