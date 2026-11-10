// The waveform: turns decoded audio into a braille envelope of the whole track, with columns drawn at
// a chosen resolution.
#include "waveform.h"
#include "path_utf8.h"
#include "process_util.h"
#include "miniaudio.h"
#include "string_util.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <string>
#include <memory>
#include <thread>
#include <vector>

namespace muisc {

BrailleColumn WaveformQuantizer::get_column(int level) {
    switch (level) {
        case 0: return {" ", "\u2836", " "};
        case 1: return {" ", "\u28FF", " "};
        case 2: return {"\u28C0", "\u28FF", "\u2809"};
        case 3: return {"\u28E4", "\u28FF", "\u281B"};
        case 4: return {"\u28F6", "\u28FF", "\u283F"};
        case 5: return {"\u28FF", "\u28FF", "\u28FF"};
        default: return {" ", "\u2836", " "};
    }
}

std::vector<float> WaveformQuantizer::generate_high_res_envelope(const std::vector<float>& pcm_data,
                                                                   int resolution, bool smooth, int channels) {
    std::vector<float> high_res(resolution, 0.0f);
    if (pcm_data.empty() || resolution <= 0) return high_res;

    const size_t ch = channels >= 2 ? 2 : 1;
    const size_t total_frames = pcm_data.size() / ch;
    if (total_frames == 0) return high_res;

    size_t chunk_size = total_frames / static_cast<size_t>(resolution);
    if (chunk_size == 0) chunk_size = 1;

    std::vector<float> raw_rms(resolution, 0.0f);
    for (int i = 0; i < resolution; ++i) {
        // PERF: was `double sum_sq` — the float→double promotion on every
        // iteration prevented ARM NEON auto-vectorization and roughly halved
        // throughput vs. float on Termux/Android.  Float precision is more
        // than sufficient for a 6-level visual bar (the final output is
        // quantized to 0-5 anyway).
        float sum_sq = 0.0f;
        size_t start = static_cast<size_t>(i) * chunk_size;
        size_t end = std::min(start + chunk_size, total_frames);
        size_t count = end > start ? end - start : 0;
        if (count > 0) {
            if (ch == 1) {
                for (size_t j = start; j < end; ++j) {
                    sum_sq += pcm_data[j] * pcm_data[j];
                }
            } else {
                for (size_t j = start; j < end; ++j) {
                    const float m = 0.5f * (pcm_data[2 * j] + pcm_data[2 * j + 1]);
                    sum_sq += m * m;
                }
            }
            raw_rms[i] = std::sqrt(sum_sq / static_cast<float>(count));
        }
    }

    return finish_envelope(raw_rms, resolution, smooth);
}

// From the per-bin RMS to the 0..1 model (smoothing, normalization, contrast curve).
std::vector<float> WaveformQuantizer::finish_envelope(const std::vector<float>& raw_rms, int resolution, bool smooth) {
    std::vector<float> high_res(resolution, 0.0f);
    const std::vector<float>* source = &raw_rms;
    std::vector<float> smoothed;
    if (smooth) {
        smoothed.assign(resolution, 0.0f);
        // Same narrow, center-weighted kernel as before — a wider 5-tap
        // kernel spreads a loud bin's energy into its neighbors almost
        // as strongly as its own value, which is what made a sudden
        // drop look "extended" past where it actually happened.
        const float weights[3] = {0.15f, 0.70f, 0.15f};
        for (int i = 0; i < resolution; ++i) {
            float sum = 0.0f, weight_sum = 0.0f;
            for (int j = -1; j <= 1; ++j) {
                int idx = i + j;
                if (idx >= 0 && idx < resolution) {
                    sum += raw_rms[idx] * weights[j + 1];
                    weight_sum += weights[j + 1];
                }
            }
            smoothed[i] = sum / weight_sum;
        }
        source = &smoothed;
    }

    float global_max = 0.0001f;
    for (float v : *source) {
        if (v > global_max) global_max = v;
    }

    for (int i = 0; i < resolution; ++i) {
        float normalized = (*source)[i] / global_max;
        high_res[i] = std::clamp(std::pow(normalized, 2.5f), 0.0f, 1.0f);
    }

    return high_res;
}

std::vector<float> WaveformQuantizer::envelope_from_bins(const std::vector<float>& sumsq, size_t gran, size_t total_frames,
                                                          int resolution, bool smooth) {
    std::vector<float> raw_rms(static_cast<size_t>(std::max(0, resolution)), 0.0f);
    if (resolution <= 0 || sumsq.empty() || gran == 0) return std::vector<float>(static_cast<size_t>(std::max(0, resolution)), 0.0f);
    if (total_frames == 0) total_frames = sumsq.size() * gran;
    // the same bins as generate_high_res_envelope (total / resolution frames each), summed from the fine bins; a fine
    // bin that straddles a boundary counts in proportion
    std::vector<double> prefix(sumsq.size() + 1, 0.0);
    for (size_t i = 0; i < sumsq.size(); ++i) prefix[i + 1] = prefix[i] + sumsq[i];
    auto cum = [&](size_t f) {   // sum of squares of frames [0, f)
        const size_t b = f / gran;
        if (b >= sumsq.size()) return prefix.back();
        return prefix[b] + sumsq[b] * static_cast<double>(f - b * gran) / static_cast<double>(gran);
    };
    size_t chunk = total_frames / static_cast<size_t>(resolution);
    if (chunk == 0) chunk = 1;
    for (int i = 0; i < resolution; ++i) {
        const size_t a = static_cast<size_t>(i) * chunk, e = std::min(a + chunk, total_frames);
        if (e > a) raw_rms[static_cast<size_t>(i)] = static_cast<float>(std::sqrt(std::max(0.0, cum(e) - cum(a)) / static_cast<double>(e - a)));
    }
    return finish_envelope(raw_rms, resolution, smooth);
}

std::vector<int> WaveformQuantizer::resample_for_ui(const std::vector<float>& high_res_model, int terminal_width) {
    std::vector<int> ui_waveform(std::max(0, terminal_width), 0);
    if (terminal_width <= 0 || high_res_model.empty()) return ui_waveform;

    float ratio = static_cast<float>(high_res_model.size()) / static_cast<float>(terminal_width);

    for (int i = 0; i < terminal_width; ++i) {
        int start_idx = static_cast<int>(i * ratio);
        int end_idx = static_cast<int>((i + 1) * ratio);
        start_idx = std::clamp(start_idx, 0, static_cast<int>(high_res_model.size()));
        end_idx = std::clamp(end_idx, start_idx, static_cast<int>(high_res_model.size()));
        if (end_idx == start_idx && start_idx < static_cast<int>(high_res_model.size())) {
            end_idx = start_idx + 1; // resolution > terminal_width in every realistic case, but guard the edge anyway
        }

        // Peak decimation: the loudest bin in this column's bucket wins,
        // so a brief transient never gets averaged away into nothing —
        // it's why this needs the pre-computed high-res model in the
        // first place rather than a plain low-res RMS pass at whatever
        // width happened to be current at load time.
        float local_peak = 0.0f;
        for (int j = start_idx; j < end_idx; ++j) {
            if (high_res_model[j] > local_peak) local_peak = high_res_model[j];
        }

        if (local_peak < 0.08f) local_peak = 0.0f;
        ui_waveform[i] = static_cast<int>(std::round(local_peak * 5.0f));
    }

    return ui_waveform;
}

// Primary decode path: miniaudio's own built-in decoder, entirely
// in-process — no subprocess, no shell, nothing that depends on where
// (or whether) a shell binary happens to live on this device. Covers
// WAV/MP3/FLAC/OGG directly. This is the same approach as the reference
// implementation that prompted this rewrite: ma_decoder_init_file() +
// ma_decoder_read_pcm_frames() in a loop, chunk by chunk, which is what
// lets it start producing frames almost immediately with no process-spawn
// overhead at all.
static bool stream_decode_miniaudio(const fs::path& file_path, StreamingPcm& pcm,
                                     const std::function<void(const float*, size_t)>& on_chunk) {
    ma_decoder decoder;
    const int channels = pcm.channels >= 2 ? 2 : 1;
    // At the rate the buffer was set up with -- the file's own rate (see native_sample_rate()), so nothing is
    // resampled. If the file turns out to have another rate, the ffmpeg path converts it instead: miniaudio's own
    // converter is a plain linear one.
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, static_cast<ma_uint32>(channels), 0);
    // ma_decoder_init_file() takes a narrow path, which Windows resolves
    // through the ANSI code page -- so a track whose name contains anything
    // that code page can't express simply fails to open, and every such file
    // silently fell through to the (much slower) ffmpeg fallback even for
    // formats miniaudio handles natively. Worse, getting the narrow string
    // out of the path in the first place meant .string(), which throws on
    // exactly those filenames. miniaudio ships a wide-char entry point for
    // this; on every other platform the narrow one is already UTF-8.
#if defined(_WIN32)
    ma_result init_rc = ma_decoder_init_file_w(file_path.c_str(), &config, &decoder);
#else
    ma_result init_rc = ma_decoder_init_file(file_path.c_str(), &config, &decoder);
#endif
    if (init_rc != MA_SUCCESS) {
        return false; // let the caller fall back to the ffmpeg path (e.g. Opus, which this can't touch)
    }
    if (static_cast<int>(decoder.outputSampleRate) != pcm.sample_rate) {
        ma_decoder_uninit(&decoder);
        return false; // another rate than the buffer's: ffmpeg resamples it properly
    }

    float buf[4096 * 2]; // 4096 frames, up to 2 channels
    ma_uint64 frames_read = 0;
    for (;;) {
        if (pcm.cancelled.load()) break;   // let go of the file now (quitting, a cut, a rename)
        ma_result result = ma_decoder_read_pcm_frames(&decoder, buf, 4096, &frames_read);
        if (frames_read > 0) {
            pcm.append(buf, static_cast<size_t>(frames_read));
            if (on_chunk) on_chunk(buf, static_cast<size_t>(frames_read));
        }
        if (result != MA_SUCCESS || frames_read == 0) break;
    }
    ma_decoder_uninit(&decoder);
    return true;
}

// Fallback for formats miniaudio's built-in decoders don't cover -- Opus
// (yt-dlp's cache format) being the main one this project actually needs.
//
// The spawn itself lives in process_util.cpp behind spawn_capture(), so
// this function is identical on every platform: the program is started
// directly from its argument list, with no shell on any platform.
// The stdin-detachment that fixed the render-loop freeze (ffmpeg grabbing the
// terminal for its interactive controls and leaving it in line-buffered mode
// on exit) is part of that shared contract.
static void stream_decode_ffmpeg_fallback(const fs::path& file_path, StreamingPcm& pcm,
                                           const std::function<void(const float*, size_t)>& on_chunk) {
    // -nostdin: tells ffmpeg outright not to expect interactive keyboard
    // input. Belt-and-suspenders -- the real fix is the stdin redirect in
    // spawn_capture(), but this makes the intent explicit and costs nothing.
    const int channels = pcm.channels >= 2 ? 2 : 1;
    // The buffer's rate is the file's own (see native_sample_rate()), so usually nothing is resampled; when it is
    // (a file above 96 kHz, or an unknown rate), with a long, sharp filter instead of ffmpeg's default one.
    const std::string sr = std::to_string(pcm.sample_rate);
    const Args cmd = {"ffmpeg", "-nostdin", "-v", "error", "-i", path_utf8(file_path),
                      "-af", "aresample=" + sr + ":filter_size=64:phase_shift=10:cutoff=0.97",
                      "-f", "f32le", "-ac", std::to_string(channels), "-ar", sr, "-"};

    std::unique_ptr<ChildProcess> child = spawn_capture(cmd, /*merge_stderr=*/false);
    if (!child) {
        pcm.decode_failed.store(true);
        pcm.decode_done.store(true);
        return;
    }

    // Two levels of leftovers: at most sizeof(float)-1 = 3 BYTES between reads
    // (a float split across two pipe reads), and at most channels-1 FLOATS
    // (a stereo frame split across two chunks) held in `pending` until the
    // rest of the frame arrives. The old std::vector::erase approach was an
    // O(N) shift per chunk across the whole decode; this only ever moves a
    // handful of values.
    char carry[sizeof(float) - 1];
    size_t carry_len = 0;
    std::vector<float> pending;
    std::array<char, 65536> buf{};
    std::ptrdiff_t n;
    while (!pcm.cancelled.load() && (n = child->read(buf.data(), buf.size())) > 0) {   // (cancelled: quitting, a cut, a rename)
        size_t total = carry_len + static_cast<size_t>(n);
        size_t whole_floats = total / sizeof(float);
        size_t whole_bytes  = whole_floats * sizeof(float);

        if (whole_floats > 0) {
            const size_t old_pending = pending.size();
            pending.resize(old_pending + whole_floats);
            char* dst = reinterpret_cast<char*>(pending.data() + old_pending);
            size_t out_byte = 0;
            for (size_t i = 0; i < carry_len && out_byte < whole_bytes; ++i, ++out_byte)
                dst[out_byte] = carry[i];
            size_t from_buf = whole_bytes - carry_len;
            std::memcpy(dst + carry_len, buf.data(), from_buf);

            const size_t frames = pending.size() / static_cast<size_t>(channels);
            if (frames > 0) {
                pcm.append(pending.data(), frames);
                if (on_chunk) on_chunk(pending.data(), frames);
                const size_t used = frames * static_cast<size_t>(channels);
                std::copy(pending.begin() + static_cast<std::ptrdiff_t>(used), pending.end(), pending.begin());
                pending.resize(pending.size() - used);
            }

            size_t leftover_start = from_buf;
            carry_len = static_cast<size_t>(n) - from_buf;
            for (size_t i = 0; i < carry_len; ++i)
                carry[i] = buf[leftover_start + i];
        } else {
            // Less than one float across carry+buf combined -- absorb into carry.
            for (size_t i = 0; i < static_cast<size_t>(n) && carry_len < sizeof(carry); ++i)
                carry[carry_len++] = buf[i];
        }
    }

    int exit_code = child->wait();
    if (exit_code != 0 && pcm.available.load() == 0) {
        pcm.decode_failed.store(true);
    }
    pcm.decode_done.store(true);
}

// ---- Long tracks: a window of the track at a time (StreamingPcm windowed mode) ------------------------------------
namespace {

// Reads a track from any frame on, at the buffer's rate and channel count: miniaudio's own decoder when it can
// (WAV/FLAC/MP3/OGG at the file's own rate; MP3 with a seek table, so seeking is quick), else ffmpeg started at the
// position (-ss before -i: a quick and sample-accurate seek when decoding).
class TrackReader {
public:
    TrackReader(const fs::path& path, int rate, int channels, bool seekable)
        : path_(path), rate_(rate), ch_(channels >= 2 ? 2 : 1) {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, static_cast<ma_uint32>(ch_), 0);
        if (seekable) cfg.seekPointCount = 4096;
#if defined(_WIN32)
        ma_result rc = ma_decoder_init_file_w(path.c_str(), &cfg, &dec_);
#else
        ma_result rc = ma_decoder_init_file(path.c_str(), &cfg, &dec_);
#endif
        if (rc == MA_SUCCESS) {
            if (static_cast<int>(dec_.outputSampleRate) == rate_) ma_ok_ = true;
            else ma_decoder_uninit(&dec_);
        }
        if (!ma_ok_) start_ffmpeg(0);
        else if (seekable) calibrate();
    }
    ~TrackReader() { if (ma_ok_) ma_decoder_uninit(&dec_); }
    TrackReader(const TrackReader&) = delete;
    TrackReader& operator=(const TrackReader&) = delete;

    bool ok() const { return ma_ok_ || child_ != nullptr; }
    // exact length in frames when the decoder knows it (0 = unknown)
    size_t length() {
        ma_uint64 n = 0;
        if (ma_ok_ && ma_decoder_get_length_in_pcm_frames(&dec_, &n) == MA_SUCCESS) return static_cast<size_t>(n);
        return 0;
    }
    bool seek(size_t frame) {
        if (ma_ok_) {
            // with the offset the seek table is off by (see calibrate); the frames before it are read and dropped
            const long long at = static_cast<long long>(frame) - seek_off_;
            if (ma_decoder_seek_to_pcm_frame(&dec_, static_cast<ma_uint64>(std::max(0ll, at))) != MA_SUCCESS) return false;
            for (long long skip = at < 0 ? -at : 0; skip > 0;) {
                float tmp[1024 * 2];
                ma_uint64 got = 0;
                ma_decoder_read_pcm_frames(&dec_, tmp, static_cast<ma_uint64>(std::min(skip, 1024ll)), &got);
                if (got == 0) break;
                skip -= static_cast<long long>(got);
            }
            return true;
        }
        start_ffmpeg(frame);
        return child_ != nullptr;
    }
    // up to `frames` frames; 0 = the end
    size_t read(float* out, size_t frames) {
        if (ma_ok_) {
            ma_uint64 got = 0;
            ma_decoder_read_pcm_frames(&dec_, out, frames, &got);
            return static_cast<size_t>(got);
        }
        if (!child_) return 0;
        const size_t need = frames * static_cast<size_t>(ch_) * sizeof(float);
        while (pend_.size() < need && !eof_) {
            char buf[65536];
            const std::ptrdiff_t n = child_->read(buf, sizeof buf);
            if (n <= 0) { eof_ = true; break; }
            pend_.insert(pend_.end(), buf, buf + n);
        }
        const size_t frame_bytes = static_cast<size_t>(ch_) * sizeof(float);
        const size_t take = std::min(need, pend_.size() / frame_bytes * frame_bytes);
        std::memcpy(out, pend_.data(), take);
        pend_.erase(pend_.begin(), pend_.begin() + static_cast<long>(take));
        return take / frame_bytes;
    }

private:
    // An MP3 seek through the seek table can land a fixed number of frames off (47 in tests with LAME files: the
    // decoder's priming frames are counted differently there). Found once by comparing a stretch read straight through
    // with the same stretch reached by a seek; seek() then corrects it, so a seek lands on the exact frame.
    void calibrate() {
        const size_t ch = static_cast<size_t>(ch_);
        const size_t F = static_cast<size_t>(rate_) * 10, kBack = 1024, kN = 2048;
        ma_uint64 len = 0;
        if (ma_decoder_get_length_in_pcm_frames(&dec_, &len) == MA_SUCCESS && len > 0 && len < F + 8192) return;
        std::vector<float> ref((kBack + kN + kBack) * ch), test(kN * ch), tmp(8192 * ch);
        size_t pos = 0, filled = 0;
        while (pos < F + kN + kBack) {   // straight through up to F + kN + kBack, keeping [F - kBack, F + kN + kBack)
            ma_uint64 got = 0;
            ma_decoder_read_pcm_frames(&dec_, tmp.data(), 8192, &got);
            if (got == 0) break;
            for (size_t i = 0; i < got; ++i, ++pos)
                if (pos >= F - kBack && pos < F + kN + kBack) { std::copy_n(tmp.data() + i * ch, ch, ref.data() + (pos - (F - kBack)) * ch); ++filled; }
        }
        ma_uint64 got = 0;
        if (filled == ref.size() / ch && ma_decoder_seek_to_pcm_frame(&dec_, F) == MA_SUCCESS) {
            ma_decoder_read_pcm_frames(&dec_, test.data(), kN, &got);
            double energy = 0;
            for (size_t i = 0; i < kN * ch; ++i) energy += static_cast<double>(test[i]) * test[i];
            if (got == kN && energy > 1e-6) {
                // the smallest offset with an exact match: test frame i == straight-through frame F + off + i
                for (long long d = 0; d <= static_cast<long long>(kBack); ++d)
                    for (long long off : {d, -d}) {
                        bool same = true;
                        for (size_t i = 0; i < kN * ch && same; ++i)
                            same = std::fabs(test[i] - ref[static_cast<size_t>(static_cast<long long>(kBack) + off) * ch + i]) < 1e-6f;
                        if (same) { seek_off_ = off; d = static_cast<long long>(kBack) + 1; break; }
                    }
            }
        }
        ma_decoder_seek_to_pcm_frame(&dec_, 0);
    }
    long long seek_off_ = 0;

    void start_ffmpeg(size_t frame) {
        child_.reset();   // closes the pipe: the old ffmpeg ends
        pend_.clear();
        eof_ = false;
        const std::string sr = std::to_string(rate_);
        char ss[64];
        std::snprintf(ss, sizeof ss, "%.6f", static_cast<double>(frame) / rate_);
        Args cmd = {"ffmpeg", "-nostdin", "-v", "error"};
        if (frame > 0) cmd.insert(cmd.end(), {"-ss", ss});
        cmd.insert(cmd.end(), {"-i", path_utf8(path_),
                               "-af", "aresample=" + sr + ":filter_size=64:phase_shift=10:cutoff=0.97",
                               "-f", "f32le", "-ac", std::to_string(ch_), "-ar", sr, "-"});
        child_ = spawn_capture(cmd, /*merge_stderr=*/false);
    }
    fs::path path_;
    int rate_, ch_;
    ma_decoder dec_{};
    bool ma_ok_ = false;
    std::unique_ptr<ChildProcess> child_;
    std::vector<char> pend_;
    bool eof_ = false;
};

} // namespace

void stream_decode_windowed(const fs::path& file_path, StreamingPcm& pcm, const std::function<bool()>& abandoned) {
    TrackReader rd(file_path, pcm.sample_rate, pcm.channels, true);
    if (!rd.ok()) { pcm.decode_failed.store(true); pcm.decode_done.store(true); return; }
    if (const size_t len = rd.length()) pcm.total_frames.store(len, std::memory_order_release);
    const size_t ch = static_cast<size_t>(pcm.channels);
    std::vector<float> buf(4096 * ch);
    size_t pos = 0;
    bool at_end = false;
    while (!abandoned()) {
        size_t to;
        if (pcm.need_seek(to)) {
            if (!rd.seek(to)) { pcm.decode_failed.store(true); pcm.decode_done.store(true); return; }
            pcm.reposition(to);
            pos = to;
            at_end = false;
            continue;
        }
        if (at_end) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }   // stays: a seek back may come
        const size_t n = rd.read(buf.data(), 4096);
        if (n == 0) {
            at_end = true;
            if (pos == 0) { pcm.decode_failed.store(true); pcm.decode_done.store(true); return; }
            // the real end: from here on the length is exact (an estimate from the header may be off)
            pcm.total_frames.store(pos, std::memory_order_release);
            pcm.decode_done.store(true);
            continue;
        }
        if (pcm.write_window(buf.data(), n, abandoned)) pos += n;
        // else: a seek is due (handled at the top) or nobody wants the track any more
    }
}

void analyse_track(const fs::path& file_path, StreamingPcm& pcm, const std::function<bool()>& abandoned) {
    TrackReader rd(file_path, pcm.sample_rate, pcm.channels, false);
    if (rd.ok()) {
        const size_t ch = static_cast<size_t>(pcm.channels);
        std::vector<float> buf(4096 * ch);
        size_t total = 0;
        for (;;) {
            if (abandoned()) return;
            const size_t n = rd.read(buf.data(), 4096);
            if (n == 0) break;
            pcm.analyse(buf.data(), n);
            total += n;
        }
        pcm.analyse_end();
        if (total > 0) pcm.total_frames.store(total, std::memory_order_release);   // exact, also before playback gets there
    }
    pcm.finalize_loudness();
    pcm.analysis_done.store(true, std::memory_order_release);
}

int native_sample_rate(const fs::path& file_path, const std::string& sampling_hint, double duration_sec, bool cap_96k) {
    int rate = 0;
    ma_decoder decoder;
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);   // native format: only the header is read
#if defined(_WIN32)
    if (ma_decoder_init_file_w(file_path.c_str(), &config, &decoder) == MA_SUCCESS) {
#else
    if (ma_decoder_init_file(file_path.c_str(), &config, &decoder) == MA_SUCCESS) {
#endif
        rate = static_cast<int>(decoder.outputSampleRate);
        ma_decoder_uninit(&decoder);
    }
    if (rate <= 0) rate = muisc::atoi_safe(sampling_hint.c_str());   // ffprobe's figure ("48000KHz"), e.g. for Opus
    if (rate < 8000 || rate > 768000) return 44100;
    // Above 96 kHz it is brought down to that, which is still far beyond hearing and the scope. (Long tracks keep
    // their own rate too: they are held a window at a time -- see StreamingPcm's windowed mode.)
    (void)duration_sec;
    if (cap_96k && rate > 96000) rate = 96000;   // (Settings -> GENERAL -> AUDIO -> Rate Cap at 96 kHz)
    return rate;
}

void stream_decode_ffmpeg(const fs::path& file_path, StreamingPcm& pcm,
                           const std::function<void(const float*, size_t)>& on_chunk) {
    if (stream_decode_miniaudio(file_path, pcm, on_chunk)) {
        pcm.decode_done.store(true);
        if (pcm.available.load() == 0) pcm.decode_failed.store(true);
        pcm.finalize_loudness();
        return;
    }
    stream_decode_ffmpeg_fallback(file_path, pcm, on_chunk);
    pcm.finalize_loudness();
}

} // namespace muisc
