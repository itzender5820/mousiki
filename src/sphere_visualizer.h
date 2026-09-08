#pragma once
#include <string>
#include <vector>

namespace muisc {

// A small Braille-rasterized, audio-reactive 3D sphere — rendered into
// the lyrics panel whenever synced lyrics aren't available (not found,
// still fetching, fetch failed), instead of leaving that space empty
// with just a status line. Ported from a reference volumetric-sphere
// visualizer the user provided, scaled down for a compact embedded panel
// and — importantly — reusing this project's EXISTING FftVisualizer band
// data rather than standing up a second independent FFT/ring-buffer
// pipeline. Same audio, one analysis path; this class is pure geometry
// and rasterization.
class SphereVisualizer {
public:
    // Renders into a cols x rows character grid (braille packs 2x4
    // sub-pixels per cell, so the working resolution is cols*2 x rows*4).
    // `bars` are the 0-8 level values FftVisualizer::compute_bars()
    // already computed for the main spectrum strip this frame — reused
    // here as the reactive displacement source instead of recomputing
    // anything. `dt` advances the rotation at a constant real-world rate.
    std::vector<std::string> render(int cols, int rows, const std::vector<int>& bars, double dt);

private:
    float yaw_ = 0.0f;
};

} // namespace muisc
