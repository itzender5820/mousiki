// creator_state.h -- the creator overlay's state, kept apart from app.h so that a change here only rebuilds the files that include this header
// and not the whole program. App holds it behind a pointer (App::creator_).
#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <memory>
#include <thread>
#include <future>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "mic_capture.h"
#include "creator.h"
#include "local_source.h"
#include "oscilloscope_visualizer.h"
#include "spectrogram.h"

namespace muisc {

namespace fs = std::filesystem;

struct CreatorState {
    int row = 0;                               // index into creator_rows()
    int mode = 0;                              // 0 text to sound, 1 microphone
    bool grid = true;                          // the grid of the scope previews (text to scope and microphone)
    int live_view = 0;                         // microphone view: 0 spectrogram (default), 1 oscilloscope XY, 2 oscilloscope YT
    std::string text = "MOUSIKI";
    bool list_open = false;                    // the list of created files is shown instead of the rows
    std::vector<fs::path> files;
    int list_sel = 0;
    int target = 0;                            // 0 oscilloscope (XY), 1 spectrogram
    int len = 1;                               // index into the length list
    int invert = 0;                            // 0 auto, 1 on, 2 off
    int fps = 1;                               // scope: frame rate list index
    int motion = 0;                            // scope: Motion
    int detail = 1;                            // scope: 0 low .. 2 high
    int scale = 0;                             // spectrogram: Scale
    int octaves = 5;                           // spectrogram: 3..6
    int reverb = 1;                            // spectrogram: 0 off, 1 light, 2 hall
    std::string status;
    std::vector<std::string> preview;          // braille lines of the current picture
    std::string loaded_key;                    // what preview / gray were made from
    std::shared_ptr<void> gray;                // a creator::Gray (kept opaque so app.h does not need creator.h)
    bool show = false;
    int preview_kind = 0;                      // 0 = oscilloscope (XY), 1 = spectrogram, 2 = oscilloscope (YT, microphone only)
    bool live = false;
    std::unique_ptr<MicCapture> mic;
    bool rec_on = false;
    std::vector<float> rec;                    // interleaved L/R of the running recording
    int rec_rate = 44100;
    std::shared_ptr<creator::Stereo> audio;
    LocalTrack track;
    size_t fed = 0;                            // frames of audio given to the scope / spectrogram
    double last_pos = 0.0;
    OscilloscopeVisualizer scope;
    SpectroAnalyzer spectro;
    SpectroGfx spectro_gfx;
    SpectroSettings spectro_set;
    bool prev_valid = false;                   // a song was playing before the creator started its file: it comes back when the preview closes
    LocalTrack prev;
    bool resume = false;
};

} // namespace muisc
