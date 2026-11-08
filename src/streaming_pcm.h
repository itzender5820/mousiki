#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace muisc {

// A growing, single-producer/single-consumer PCM buffer: one decode
// thread appends to it while the audio callback (and, once decode
// finishes, the waveform pass) read whatever's been decoded so far.
//
// Audio is stored as interleaved stereo (L,R,L,R,...), kChannels samples
// per frame. Everything that counts in "frames" (available, cursor,
// capacity_frames()) is per-frame, never per-sample. Consumers that only
// want a single channel (waveform envelope, FFT) use mono_mixdown().
//
// Capacity is reserved up front from ffprobe's duration estimate, and
// append() DELIBERATELY REFUSES to grow past that reserved capacity
// (it silently clamps instead of reallocating). That's not laziness —
// it's what makes the read side safe without a lock: once `available`
// is published (release-store), every element up to that count is
// guaranteed already written AND the vector's backing pointer is
// guaranteed to never have moved, because it never reallocates. A
// reader that does `size_t n = available.load(acquire); read
// samples[0..n*kChannels)` is race-free by construction. The tradeoff is a
// duration estimate that's wildly wrong (rare — ffprobe reads the
// container header, it's usually right) truncates the last bit of a track
// rather than risking a use-after-free on a mid-playback reallocation
// touched by another thread. That trade is worth it here.
struct StreamingPcm {
    static constexpr size_t kChannels = 2;

    std::vector<float> samples;         // interleaved, kChannels per frame
    std::atomic<size_t> available{0};   // frames safe to read right now
    std::atomic<bool> decode_done{false};
    std::atomic<bool> decode_failed{false};
    std::atomic<bool> capacity_exceeded{false}; // diagnostic only
    int sample_rate = 44100;

    void reserve_for_seconds(double seconds, int sr) {
        sample_rate = sr;
        size_t est = static_cast<size_t>(std::max(1.0, seconds) * sr * 1.25); // 25% headroom
        size_t frames = std::max<size_t>(est, static_cast<size_t>(sr) * 5);   // at least 5s worth
        samples.reserve(frames * kChannels);
    }

    // Reserved capacity, in frames (the eventual max track length).
    size_t capacity_frames() const { return samples.capacity() / kChannels; }

    // Decode thread only. `interleaved` holds frame_count * kChannels floats.
    void append(const float* interleaved, size_t frame_count) {
        size_t room = (samples.capacity() - samples.size()) / kChannels;
        size_t n = std::min(frame_count, room);
        if (n > 0) {
            samples.insert(samples.end(), interleaved, interleaved + n * kChannels);
            available.store(samples.size() / kChannels, std::memory_order_release);
        }
        if (n < frame_count) capacity_exceeded.store(true, std::memory_order_relaxed);
    }

    // Average of all channels for the first `frames` frames. Safe to call
    // with frames <= a previously acquire-loaded `available`.
    std::vector<float> mono_mixdown(size_t frames) const {
        std::vector<float> mono(frames);
        for (size_t i = 0; i < frames; ++i) {
            float sum = 0.0f;
            for (size_t c = 0; c < kChannels; ++c) sum += samples[i * kChannels + c];
            mono[i] = sum / static_cast<float>(kChannels);
        }
        return mono;
    }
};

} // namespace muisc
