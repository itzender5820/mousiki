// What the last video picture cost in Mousiki's own work (milliseconds): the look (effects and colour grade), the colour reduction and
// the encoding of the picture for the terminal. Only read for the "slow frame" line of the console log.
#pragma once
#include <atomic>

namespace muisc {
struct VideoTiming {
    std::atomic<double> look_ms{0.0}, quant_ms{0.0}, encode_ms{0.0}, scope_ms{0.0};   // (scope_ms: the oscilloscope picture of the big video overlay)
    std::atomic<double> grade_ms{0.0}, fx_ms{0.0};   // the look split: the colour grade (contrast, colours, brightness, chromatic aberration) and the effect
    std::atomic<int> threads{1}, fx{0};              // threads the looks used last, and the effect number
};
inline VideoTiming& video_timing() {
    static VideoTiming t;
    return t;
}
}  // namespace muisc
