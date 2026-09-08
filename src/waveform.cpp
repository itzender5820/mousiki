#include "waveform.h"
#include "process_util.h"
#include "miniaudio.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

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
                                                                   int resolution, bool smooth) {
    std::vector<float> high_res(resolution, 0.0f);
    if (pcm_data.empty() || resolution <= 0) return high_res;

    size_t chunk_size = pcm_data.size() / static_cast<size_t>(resolution);
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
        size_t end = std::min(start + chunk_size, pcm_data.size());
        size_t count = end - start;
        if (count > 0) {
            for (size_t j = start; j < end; ++j) {
                sum_sq += pcm_data[j] * pcm_data[j];
            }
            raw_rms[i] = std::sqrt(sum_sq / static_cast<float>(count));
        }
    }

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
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, 44100);
    if (ma_decoder_init_file(file_path.string().c_str(), &config, &decoder) != MA_SUCCESS) {
        return false; // let the caller fall back to the ffmpeg path (e.g. Opus, which this can't touch)
    }

    float buf[4096];
    ma_uint64 frames_read = 0;
    for (;;) {
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

// Fallback for formats miniaudio's built-in decoders don't cover — Opus
// (yt-dlp's cache format) being the main one this project actually needs.
// Was hardcoding "/bin/sh" here, which doesn't exist on Termux (its whole
// filesystem lives under its own prefix, not the standard FHS layout) —
// posix_spawn would just fail outright with no diagnostic the user could
// see, meaning decode silently never happened. posix_spawnp with a bare
// "sh" resolves through PATH instead, which finds Termux's shell
// wherever it actually lives.
static void stream_decode_ffmpeg_fallback(const fs::path& file_path, StreamingPcm& pcm,
                                           const std::function<void(const float*, size_t)>& on_chunk) {
    // -nostdin: tells ffmpeg outright not to expect interactive
    // keyboard input. Belt-and-suspenders -- the real fix is the stdin
    // redirect below, which means ffmpeg never even sees our terminal's
    // fd, but this makes the intent explicit and costs nothing.
    std::string cmd = "ffmpeg -nostdin -v error -i " + shell_quote(file_path.string())
                     + " -f f32le -ac 1 -ar 44100 -";

    int out_pipe[2];
    if (pipe(out_pipe) != 0) {
        pcm.decode_failed.store(true);
        pcm.decode_done.store(true);
        return;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // See process_util.cpp's run_capture() for the full explanation --
    // this was the actual freeze bug: ffmpeg inheriting the terminal's
    // raw-mode stdin and, on exit, leaving it back in canonical mode,
    // turning our non-blocking key read into a blocking one. This path
    // in particular is hit on essentially every YouTube-sourced Opus
    // track, since the primary in-process decoder can't handle Opus and
    // always falls back to here.
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);

    const char* argv[] = {"sh", "-c", cmd.c_str(), nullptr};
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, "sh", &actions, nullptr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);

    if (rc != 0) {
        close(out_pipe[0]);
        pcm.decode_failed.store(true);
        pcm.decode_done.store(true);
        return;
    }

    // BUG FIX #3: the old design used std::vector<char> leftover with
    // erase(begin, begin+n) — an O(N) shift called once per read()
    // chunk across the whole decode. Replaced with a tiny fixed carry
    // buffer: at most sizeof(float)-1 = 3 bytes can ever be left over
    // between chunks, so we never need more than 3 bytes of carry state.
    char carry[sizeof(float) - 1];
    size_t carry_len = 0;
    std::array<char, 65536> buf{};
    ssize_t n;
    while ((n = read(out_pipe[0], buf.data(), buf.size())) > 0) {
        // Prepend any bytes left from the previous read.
        size_t total = carry_len + static_cast<size_t>(n);
        size_t whole_floats = total / sizeof(float);
        size_t whole_bytes  = whole_floats * sizeof(float);

        if (whole_floats > 0) {
            // The first `carry_len` bytes come from the carry buffer;
            // the remainder from the current read.  We assemble only as
            // many complete floats as we need, never allocating a
            // temporary vector for the whole chunk.
            std::vector<float> chunk(whole_floats);
            size_t out_byte = 0;
            // Copy carry bytes first.
            for (size_t i = 0; i < carry_len && out_byte < whole_bytes; ++i, ++out_byte)
                reinterpret_cast<char*>(chunk.data())[out_byte] = carry[i];
            // Then copy from the current read buffer.
            size_t from_buf = whole_bytes - carry_len;
            std::memcpy(reinterpret_cast<char*>(chunk.data()) + carry_len,
                        buf.data(), from_buf);

            pcm.append(chunk.data(), chunk.size());
            if (on_chunk) on_chunk(chunk.data(), chunk.size());

            // Save the remaining 0-3 bytes as new carry.
            size_t leftover_start = from_buf;
            carry_len = static_cast<size_t>(n) - from_buf;
            for (size_t i = 0; i < carry_len; ++i)
                carry[i] = buf[leftover_start + i];
        } else {
            // Less than one float across carry+buf combined — absorb into carry.
            for (size_t i = 0; i < static_cast<size_t>(n) && carry_len < sizeof(carry); ++i)
                carry[carry_len++] = buf[i];
        }
    }
    close(out_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0) && pcm.available.load() == 0) {
        pcm.decode_failed.store(true);
    }
    pcm.decode_done.store(true);
}

void stream_decode_ffmpeg(const fs::path& file_path, StreamingPcm& pcm,
                           const std::function<void(const float*, size_t)>& on_chunk) {
    if (stream_decode_miniaudio(file_path, pcm, on_chunk)) {
        pcm.decode_done.store(true);
        if (pcm.available.load() == 0) pcm.decode_failed.store(true);
        return;
    }
    stream_decode_ffmpeg_fallback(file_path, pcm, on_chunk);
}

} // namespace muisc
