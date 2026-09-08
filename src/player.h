#pragma once
#include <atomic>
#include <memory>
#include "miniaudio.h"
#include "streaming_pcm.h"
#include "fft_visualizer.h"

namespace muisc {

// Plays back a StreamingPcm buffer through a real audio device via
// miniaudio, using the backend pinned in audio_backend.h
// (PulseAudio/ALSA -> PipeWire on Linux, WASAPI on Windows, OpenSL ES on
// Android).
//
// This reads from a buffer that may STILL BE FILLING IN — play() can be
// called the moment decode starts (as soon as duration is known from
// ffprobe and the buffer's capacity is reserved), and the callback below
// just plays silence for any frame past what's been decoded so far,
// self-correcting once the decode thread catches up. That's what lets
// playback start almost immediately instead of waiting for the whole
// track to decode first.
//
// This replaced an earlier ffplay-subprocess design. ffplay's audio
// output goes through SDL, and SDL's Android backend expects to be
// running inside a proper Activity with its Java glue — a plain Termux
// CLI process has neither, so SDL_OpenAudioDevice effectively never
// succeeds there and nothing plays, silently. Talking to OpenSL ES
// directly through miniaudio sidesteps that entirely.
class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // `pcm` must stay alive for as long as playback is active — App holds
    // it via shared_ptr and only replaces it once stop() has fully torn
    // the device down. `fft_sink`, if given, gets push_samples() called
    // from the audio callback with each chunk actually played (nullptr
    // to disable — e.g. not needed for a plain smoke test).
    bool play(std::shared_ptr<StreamingPcm> pcm, double start_sec, int volume_pct,
              FftVisualizer* fft_sink = nullptr);

    void pause();
    void resume();
    bool is_paused() const { return paused_; }

    void seek_relative(double delta_sec);
    void set_volume(int volume_pct);
    int volume() const { return volume_pct_; }

    double poll_elapsed() const;
    bool finished() const { return finished_.load(); }
    // Synchronously clears a stale finished flag left over from the
    // previous track. play() itself resets this too, but play() now
    // runs on a detached background thread (device init can genuinely
    // stall) — without this, there's a window where has_track_ is
    // already true for the NEW track but finished_ is still true from
    // the OLD one, and the main loop's "if (has_track_ && finished())
    // advance_track()" check fires again immediately, skipping straight
    // past the track that was just supposed to start.
    void clear_finished() { finished_.store(false); }

    void stop();

private:
    ma_context context_{};
    bool context_ready_ = false;
    ma_device device_{};
    bool device_ready_ = false;

    std::shared_ptr<StreamingPcm> pcm_;
    FftVisualizer* fft_sink_ = nullptr;
    int sample_rate_ = 44100;
    std::atomic<long long> cursor_frames_{0};
    std::atomic<bool> finished_{false};
    std::atomic<float> gain_{0.7f};
    std::atomic<bool> paused_{false};
    int volume_pct_ = 70;

    static void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count);
};

} // namespace muisc
