// The audio player: plays a StreamingPcm buffer through miniaudio, with volume, pause, seeking and a
// soft limiter at the output.
#include "player.h"
#include "scope_window.h"
#include "spectrogram.h"
#include "audio_backend.h"
#include "console_log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace muisc {

namespace {

// Transparent below the knee, then eases into +/-1.0 with a continuous
// slope instead of hard-clipping. Only used while normalisation is on, where
// boosting a quiet track could otherwise push its peaks over full scale.
inline float soft_limit(float x) {
    constexpr float kKnee = 0.891f; // -1 dBFS
    const float a = std::fabs(x);
    if (a <= kKnee) return x;
    const float y = kKnee + (1.0f - kKnee) * std::tanh((a - kKnee) / (1.0f - kKnee));
    return x < 0.0f ? -y : y;
}

} // namespace

Player::Player() = default;
Player::~Player() { stop(); }

void Player::data_callback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frame_count) {
    Player* self = static_cast<Player*>(device->pUserData);
    float* out = static_cast<float*>(output); // interleaved stereo: frame_count * 2 floats

    // Paused: silence. With Pause Fade the level goes down over about 8 ms first (and up again on resume), so the cut does not click.
    const bool want_paused = self && self->paused_.load();
    const bool smooth = self && self->pause_fade_.load(std::memory_order_relaxed);
    float ramp = self ? self->pause_ramp_.load(std::memory_order_relaxed) : 0.0f;
    if (!self || !self->pcm_ || (want_paused && (!smooth || ramp <= 0.0f))) {
        if (self) self->pause_ramp_.store(smooth ? 0.0f : 1.0f, std::memory_order_relaxed);
        std::memset(out, 0, static_cast<size_t>(frame_count) * 2 * sizeof(float));
        return;
    }
    if (!smooth) ramp = 1.0f;

    StreamingPcm& pcm = *self->pcm_;
    long long cur = self->cursor_frames_.load();
    float gain = self->gain_.load() * self->fade_.load();

    // Channel handling. The device is always stereo. A stereo buffer plays as
    // stereo when the option is on, otherwise it is folded to mono; a mono
    // buffer is just duplicated.
    const int pcm_ch = pcm.channels >= 2 ? 2 : 1;
    const bool play_stereo = pcm_ch == 2 && self->stereo_enabled_.load();
    const bool fold_to_mono = pcm_ch == 2 && !play_stereo;

    // Loudness normalisation: target gain from this track's measured LUFS.
    // Until the measurement exists (or with normalisation off) the target is
    // unity. The applied gain follows the target with a ~1 s time constant,
    // so toggling it, or the estimate being refined as decoding proceeds,
    // never produces a click or a sudden jump. When a stereo buffer is being
    // folded to mono the mono-mix figure is the one that applies, since that
    // is what actually reaches the speakers.
    const bool norm_on = self->norm_enabled_.load();
    const float lufs = (fold_to_mono ? pcm.loudness_mono_lufs : pcm.loudness_lufs).load(std::memory_order_relaxed);
    float norm_target = 1.0f;
    float norm_db = 0.0f;
    if (norm_on && !std::isnan(lufs)) {
        norm_db = std::clamp(self->norm_target_lufs_.load() - lufs, -30.0f, self->norm_max_boost_db_.load());
        norm_target = std::pow(10.0f, norm_db / 20.0f);
    }
    self->track_lufs_.store(lufs);
    self->norm_gain_db_.store(norm_db);
    float norm_cur = self->norm_cur_ < 0.0f ? norm_target : self->norm_cur_;
    const int sr_now = std::max(1, self->sample_rate_.load());
    const float norm_alpha = 1.0f - std::exp(-1.0f / (1.0f * static_cast<float>(sr_now)));

    // Equaliser: pick up a changed band layout (or a new sample rate) once per
    // block. configure() does ten sin/cos pairs and no allocation, so doing it
    // here on the audio thread is fine. Disabled -> bypass entirely.
    const bool eq_on = self->eq_enabled_.load(std::memory_order_relaxed);
    if (eq_on) {
        const unsigned gen = self->eq_gen_.load(std::memory_order_acquire);
        if (gen != self->eq_seen_gen_ || sr_now != self->eq_sr_) {
            EqGains g{};
            for (int b = 0; b < kEqBands; ++b) g[b] = self->eq_gains_[b].load(std::memory_order_relaxed);
            self->eq_.configure(g, sr_now);
            self->eq_seen_gen_ = gen;
            self->eq_sr_ = sr_now;
        }
    } else if (self->eq_seen_gen_ != 0) {
        self->eq_.reset();          // stale filter memory must not leak into the next enable
        self->eq_seen_gen_ = 0;
    }
    const bool eq_run = eq_on && self->eq_.active();

    // Acquire-load: pairs with the release-store in StreamingPcm::append(),
    // guaranteeing every frame below `avail` was fully written by the
    // decode thread before we read it here.
    // Windowed buffer (long tracks): only [lo, avail) is held, frame k at ring slot pcm.slot(k).
    size_t lo = 0, avail = 0;
    pcm.readable(lo, avail);
    const float* src = pcm.data.data();
    const bool windowed = pcm.windowed;
    if (windowed) pcm.play_pos.store(cur, std::memory_order_relaxed);

    const bool feed_fft = self->fft_sink_ && self->fft_mono_.size() >= frame_count;
    float* mono_out = feed_fft ? self->fft_mono_.data() : nullptr;
    // Oscilloscope music mode: the scopes get the decoded signal itself -- before the mono fold, the equalizer, the
    // normalization, the limiter and the volume -- so figures keep their exact shape and size.
    // The spectrogram analyses the decoded signal too (like Audacity analyses the file), whatever the mode.
    const bool raw_scope = self->scope_raw_on_.load(std::memory_order_relaxed);
    const bool raw_spectro = spectro_feed_active() || spectro_window_feed_active();   // terminal and / or window
    float* raw_out = (raw_scope || raw_spectro) && self->scope_raw_.size() >= 2 * static_cast<size_t>(frame_count) ? self->scope_raw_.data() : nullptr;

    // Playback speed (time_stretch.h; only the audio book player sets it): at any speed but 1.0 the frames come from the
    // stretcher, which reads the buffer itself. It starts over when the speed has just left 1.0 or the position was moved
    // from outside (a seek) since the last block.
    const double speed = self->speed_.load(std::memory_order_relaxed);
    const bool stretch = std::fabs(speed - 1.0) > 1e-3;
    if (stretch) {
        if (!self->ts_active_ || cur != self->ts_last_cur_) self->ts_.reset(static_cast<double>(cur));
        self->ts_active_ = true;
    } else {
        self->ts_active_ = false;
    }
    // Frames past the end of the track count as silence once the end is known; frames still being decoded do not.
    const size_t total_now = windowed ? pcm.total_frames.load(std::memory_order_acquire) : 0;
    const bool at_end = windowed ? (total_now > 0 && avail >= total_now) : pcm.decode_done.load();
    auto ts_avail = [&](long long i0, long long i1) {
        if (i1 > static_cast<long long>(avail) && !at_end) return false;
        if (windowed && i0 >= 0 && static_cast<size_t>(i0) < lo) return false;
        return true;
    };
    auto ts_sample = [&](long long i, int ch) -> float {
        if (i < 0 || static_cast<size_t>(i) >= avail || (windowed && static_cast<size_t>(i) < lo)) return 0.0f;
        const size_t k = pcm.slot(static_cast<size_t>(i));
        return (pcm_ch == 1 || ch == 0) ? src[k] : src[k + 1];
    };

    // Windowed: the position only moves on over frames that are actually there (after a seek the decoder needs a
    // moment to get there: that moment is a short silence, not a skipped stretch of the track).
    const float ramp_step = 1.0f / std::max(8.0f, 0.008f * static_cast<float>(sr_now));   // 8 ms from full to silent
    long long played = 0;
    bool gap = false;
    for (ma_uint32 i = 0; i < frame_count; ++i) {
        long long idx = cur + played;
        if (!windowed) idx = cur + static_cast<long long>(i);
        norm_cur += (norm_target - norm_cur) * norm_alpha;
        float l = 0.0f, r = 0.0f;
        if (raw_out) raw_out[2 * i] = raw_out[2 * i + 1] = 0.0f;
        float a = 0.0f, b = 0.0f;   // the source frame (channel 0 / 1; a mono buffer gives the same value twice)
        bool have = false;
        if (stretch) {
            have = self->ts_.next(speed, ts_avail, ts_sample, a, b);
        } else if (!gap && idx >= 0 && static_cast<size_t>(idx) >= lo && static_cast<size_t>(idx) < avail) {
            ++played;
            const size_t k = pcm.slot(static_cast<size_t>(idx));   // sample index of channel 0
            a = src[k];
            b = pcm_ch == 1 ? src[k] : src[k + 1];
            have = true;
        } else if (windowed) gap = true;
        if (have) {
            if (raw_out) {
                raw_out[2 * i] = a;
                raw_out[2 * i + 1] = b;
            }
            if (pcm_ch == 1) {
                l = r = a;
            } else if (play_stereo) {
                l = a;
                r = b;
            } else {
                l = r = 0.5f * (a + b);
            }
            if (eq_run) self->eq_.process(l, r); // before volume/normalisation: tone shaping on the raw signal
            if (smooth) {
                if (want_paused) ramp = std::max(0.0f, ramp - ramp_step);
                else if (ramp < 1.0f) ramp = std::min(1.0f, ramp + ramp_step);
            }
            const float g = norm_cur * gain * ramp;
            l *= g;
            r *= g;
        }
        if (norm_on || norm_cur > 1.001f || eq_run) { l = soft_limit(l); r = soft_limit(r); }
        out[2 * i]     = l;
        out[2 * i + 1] = r;
        if (mono_out) mono_out[i] = 0.5f * (l + r);
    }
    self->norm_cur_ = norm_cur;
    self->pause_ramp_.store(ramp, std::memory_order_relaxed);

    // The visualizer wants one channel: feed it the mono mix of what was
    // actually played (the buffer is pre-sized in play(); a callback larger
    // than that just skips the feed rather than allocating on the audio thread).
    if (feed_fft) self->fft_sink_->push_samples(mono_out, frame_count, self->sample_rate_.load());

    // The oscilloscope wants the raw time-domain signal instead: the very
    // same interleaved stereo block just written to `out` (post gain,
    // post normalisation, post limiter), left and right kept apart so it
    // can draw a stereo pair or fold to the mono mix itself.
    // push_frames() writes a fixed-size ring, so -- like the FFT feed
    // above -- this never allocates on the audio thread. While paused the
    // callback returns before any of this runs, which freezes the scope on
    // the last drawn waveform, exactly like the frozen spectrum bars.
    const float* scope_src = raw_out && raw_scope ? raw_out : out;
    if (raw_out && raw_spectro) {
        spectro_feed_push(raw_out, frame_count, self->sample_rate_.load());
        spectro_window_feed_push(raw_out, frame_count, self->sample_rate_.load());   // the spectrogram window (SHIFT+8)
    }
    if (self->scope_sink_) self->scope_sink_->push_frames(scope_src, frame_count);
    scope_feed_push(scope_src, frame_count, self->sample_rate_.load());   // the scope window (SHIFT+9); nothing while it is closed

    long long new_cur = cur + static_cast<long long>(frame_count);
    if (stretch) new_cur = static_cast<long long>(std::llround(self->ts_.heard_pos()));   // the source time of what is heard
    if (windowed) {
        if (!stretch) new_cur = cur + played;
        const size_t total = pcm.total_frames.load(std::memory_order_acquire);
        if (total > 0 && new_cur >= 0 && static_cast<size_t>(new_cur) >= total) {
            new_cur = static_cast<long long>(total);
            self->finished_.store(true);
        } else if (pcm.decode_failed.load()) self->finished_.store(true);
    } else
    // Only truly "finished" once decode is done AND playback has caught
    // all the way up to everything it ever produced — not just the
    // current available count, which may still be growing while we play.
    if (pcm.decode_done.load() &&
        new_cur >= 0 && static_cast<size_t>(new_cur) >= pcm.available.load(std::memory_order_acquire)) {
        self->finished_.store(true);
    }
    // a seek (seek_relative) while this block was being made wins over the position this block reached
    self->ts_last_cur_ = new_cur;
    if (self->cursor_frames_.compare_exchange_strong(cur, new_cur))
        self->clock_stamp_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
}

bool Player::play(std::shared_ptr<StreamingPcm> pcm, double start_sec, int volume_pct,
                   FftVisualizer* fft_sink, OscilloscopeVisualizer* scope_sink) {
    // Give the loudness measurement a moment to exist so the track starts at
    // its final level instead of gliding into it. Decoding runs far faster
    // than real time, so this normally returns immediately; capped so a slow
    // source can't delay playback noticeably. Done BEFORE taking mutex_ so
    // the main thread's stop() is never held up behind it.
    if (pcm && norm_enabled_.load()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
        while (std::isnan(pcm->loudness_lufs.load(std::memory_order_relaxed)) &&
               !pcm->loudness_final.load(std::memory_order_acquire) &&
               !pcm->decode_failed.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::lock_guard<std::mutex> lk(mutex_);
    stop_locked();
    if (!pcm) return false;

    if (!context_ready_) {
        const char* use_null = std::getenv("MOUSIKI_NULL_AUDIO");   // headless runs (tests, CI): a silent device
        if (use_null && *use_null == '1') {
            ma_backend nb[] = { ma_backend_null };
            context_ready_ = ma_context_init(nb, 1, nullptr, &context_) == MA_SUCCESS;
        } else
        context_ready_ = init_platform_audio_context(context_);
        // Not fatal if this fails — ma_device_init(nullptr, ...) below
        // falls back to miniaudio's own default backend selection.
    }

    pcm_ = std::move(pcm);
    fft_sink_ = fft_sink;
    scope_sink_ = scope_sink;
    fft_mono_.assign(16384, 0.0f); // > any realistic device period (16384 frames = 370 ms at 44.1 kHz)
    scope_raw_.assign(2 * 16384, 0.0f);
    sample_rate_.store(pcm_->sample_rate > 0 ? pcm_->sample_rate : 44100);
    volume_pct_.store(std::clamp(volume_pct, 0, 100));
    gain_.store(volume_pct_.load() / 100.0f);
    finished_.store(false);
    paused_.store(false);
    pause_ramp_.store(1.0f);
    eq_.reset();       // device isn't running yet: safe to touch audio-thread state
    eq_seen_gen_ = 0;
    eq_sr_ = 0;
    ts_.configure(sample_rate_.load());   // the speed stretcher's buffers for this rate (allocates: only while stopped)
    ts_active_ = false;
    norm_cur_ = -1.0f; // audio device isn't running yet: safe to reset; first callback snaps to the target gain
    track_lufs_.store(std::numeric_limits<float>::quiet_NaN());
    norm_gain_db_.store(0.0f);
    cursor_frames_.store(static_cast<long long>(std::max(0.0, start_sec) * sample_rate_.load()));
    if (pcm_->windowed) pcm_->play_pos.store(cursor_frames_.load(), std::memory_order_relaxed);

    if (!open_device_locked()) { pcm_.reset(); return false; }
    return true;
}

// Opens and starts the device for the current pcm_ / sample rate: the chosen output (set_device()) when it is there, otherwise
// the system's default. mutex_ held, device not open.
bool Player::open_device_locked() {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2; // always stereo; mono buffers are duplicated in the callback
    cfg.sampleRate = static_cast<ma_uint32>(sample_rate_.load());
    // If the device cannot take the track's rate, miniaudio converts it: with the steepest low-pass it offers instead
    // of the default (order 4), so no aliasing and no dull top end. (WASAPI shared mode uses Windows' own converter.)
    cfg.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;
    cfg.dataCallback = data_callback;
    cfg.pUserData = this;

    ma_context* ctx = context_ready_ ? &context_ : nullptr;
    ma_device_id chosen{};
    std::string want;
    { std::lock_guard<std::mutex> dk(device_name_mutex_); want = device_name_; }
    if (ctx && find_output_device(*ctx, want, chosen)) cfg.playback.pDeviceID = &chosen;
    else if (!want.empty()) ConsoleLog::instance().log_basic("audio: the device \"" + want + "\" is not there -- the default device plays");
    ma_result init_res = ma_device_init(ctx, &cfg, &device_);
    if (init_res != MA_SUCCESS) {
        ConsoleLog::instance().log_verbose(
            std::string("audio: ma_device_init failed: ") + ma_result_description(init_res));
        return false;
    }
    ma_result start_res = ma_device_start(&device_);
    if (start_res != MA_SUCCESS) {
        ma_device_uninit(&device_);
        ConsoleLog::instance().log_verbose(
            std::string("audio: ma_device_start failed: ") + ma_result_description(start_res));
        return false;
    }
    ConsoleLog::instance().log_verbose(
        std::string("audio: device started, backend=") + ma_get_backend_name(device_.pContext->backend) +
        ", rate=" + std::to_string(sample_rate_.load()) + "Hz");

    {
        std::lock_guard<std::mutex> dk(device_name_mutex_);
        active_device_ = device_.playback.name;
    }
    device_ready_ = true;
    return true;
}

bool Player::reopen_device() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!device_ready_ || !pcm_) return true;   // nothing open: the next play() uses the new choice
    ma_device_uninit(&device_);                 // blocks until the callback has stopped; position, pause and pcm_ stay
    device_ready_ = false;
    if (open_device_locked()) return true;
    { std::lock_guard<std::mutex> dk(device_name_mutex_); device_name_.clear(); }   // the chosen one failed: back to the default
    return open_device_locked();
}

void Player::set_device(const std::string& name) {
    std::lock_guard<std::mutex> dk(device_name_mutex_);
    device_name_ = name;
}

std::string Player::active_device() const {
    std::lock_guard<std::mutex> dk(device_name_mutex_);
    return active_device_;
}

void Player::pause() { paused_.store(true); }
void Player::resume() { paused_.store(false); }

int Player::volume() const {
    // Lock-free on purpose: called every frame by the UI, and mutex_ can be
    // held for hundreds of ms by play() during a device swap.
    return volume_pct_.load();
}

void Player::seek_relative(double delta_sec) {
    // try_lock, not lock: if play()/stop() is mid device swap the old pcm_ is
    // being torn down anyway, so dropping the seek is right -- and blocking
    // the main thread on it would freeze the UI.
    std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
    if (!lk.owns_lock() || !pcm_) return;
    long long delta_frames = static_cast<long long>(delta_sec * sample_rate_.load());
    long long cur = cursor_frames_.load();
    // Clamp against reserved capacity (the eventual max), not the
    // currently-decoded amount — seeking a bit ahead of what's decoded
    // so far is fine, it just plays silence until decode catches up.
    long long cap = static_cast<long long>(pcm_->seek_limit_frames());
    long long next = std::clamp<long long>(cur + delta_frames, 0, cap);
    cursor_frames_.store(next);
    if (pcm_->windowed) pcm_->play_pos.store(next, std::memory_order_relaxed);   // the decoder moves there now, even while paused
    if (next < cap) finished_.store(false);
}

void Player::set_normalization(bool enabled, float target_lufs, float max_boost_db) {
    norm_target_lufs_.store(std::clamp(target_lufs, -40.0f, 0.0f));
    norm_max_boost_db_.store(std::clamp(max_boost_db, 0.0f, 24.0f));
    norm_enabled_.store(enabled);
}

void Player::set_equalizer(bool enabled, const EqGains& gains_db) {
    for (int b = 0; b < kEqBands; ++b)
        eq_gains_[b].store(std::clamp(gains_db[b], kEqMinDb, kEqMaxDb), std::memory_order_relaxed);
    eq_gen_.fetch_add(1, std::memory_order_release); // publish AFTER the gains are stored
    eq_enabled_.store(enabled);
}

void Player::set_volume(int volume_pct) {
    // Atomics only -- see volume().
    const int v = std::clamp(volume_pct, 0, 100);
    volume_pct_.store(v);
    gain_.store(v / 100.0f);
}

double Player::poll_elapsed() const {
    // Lock-free on purpose -- see volume().
    const int sr = sample_rate_.load();
    if (sr <= 0) return 0.0;
    return static_cast<double>(cursor_frames_.load()) / sr;
}

double Player::smooth_elapsed() const {
    const double base = poll_elapsed();
    const long long stamp = clock_stamp_ns_.load(std::memory_order_relaxed);
    if (paused_.load() || stamp == 0) return base;
    const long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double dt = static_cast<double>(now - stamp) / 1e9;
    if (dt < 0.0 || dt > 0.25) return base;   // (no step for a long time: the device stands, so does the clock)
    return base + dt * speed();
}

void Player::stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    stop_locked();
}

void Player::stop_locked() {
    if (device_ready_) {
        ma_device_uninit(&device_);
        device_ready_ = false;
    }
    pcm_.reset();
    fft_sink_ = nullptr;
    scope_sink_ = nullptr;
}

} // namespace muisc
