#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "streaming_pcm.h"

namespace muisc {

namespace fs = std::filesystem;

struct BrailleColumn {
    std::string top;
    std::string mid;
    std::string bot;
};

class WaveformQuantizer {
public:
    static BrailleColumn get_column(int level);

    // "Backend" pass — run ONCE when a track loads, regardless of
    // terminal width. Downsamples raw PCM to `resolution` RMS-energy
    // bins (default 4096 — far finer than any realistic terminal width
    // could need, since our own W is clamped to 200 columns max) and
    // normalizes + applies the visual contrast curve. `smooth=true`
    // applies a light center-weighted blur across neighboring bins
    // first (takes the edge off per-sample noise); `smooth=false` keeps
    // the raw per-bin RMS — every real transient shows up exactly where
    // it happened, at the cost of looking a little more jagged. Returns
    // normalized values in [0,1]; NOT yet quantized to braille levels —
    // that happens per-column in resample_for_ui, since the noise gate
    // needs to see the post-decimation peak, not each raw high-res bin.
    static std::vector<float> generate_high_res_envelope(const std::vector<float>& pcm_data,
                                                           int resolution = 4096, bool smooth = true);

    // "Frontend" pass — cheap enough to run every frame (or at least on
    // every resize): resamples the fixed high-res envelope down to
    // whatever the terminal's CURRENT width actually is via peak
    // decimation (the loudest bin in each terminal column's bucket,
    // so transients never get averaged away), then noise-gates and
    // quantizes to a 0-5 braille level. This is what makes the waveform
    // correct at any width instead of the old fixed-100-column data
    // just going blank past column 100 on a wide terminal, or showing
    // an un-rescaled partial slice of the track on a narrow one.
    static std::vector<int> resample_for_ui(const std::vector<float>& high_res_model, int terminal_width);
};

// Decodes into `pcm` as data becomes available, instead of buffering the
// whole file before returning — this is what lets playback start after
// the first chunk instead of waiting for the entire track. Two paths,
// tried in order:
//   1. miniaudio's own built-in decoder, entirely in-process (no
//      subprocess at all) — covers WAV/MP3/FLAC/OGG. This is the primary
//      path and matches a known-working reference implementation.
//   2. ffmpeg via subprocess, for anything the above can't open — Opus
//      (yt-dlp's cache format) being the main real-world case.
// Runs on the calling thread until done; callers run this on its own
// background thread. Sets decode_done (and decode_failed on error) on
// `pcm` when finished. `on_chunk`, if given, is called after each chunk
// with the chunk that was just produced — used to feed the live FFT
// visualizer without it needing to touch `pcm` directly.
void stream_decode_ffmpeg(const fs::path& file_path, StreamingPcm& pcm,
                           const std::function<void(const float*, size_t)>& on_chunk = nullptr);

} // namespace muisc
