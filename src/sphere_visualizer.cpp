#include "sphere_visualizer.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace muisc {

namespace {

constexpr float kPi = 3.14159265358979323846f;
// Deliberately much lower than the reference's 36x72 — this renders into
// a small embedded panel (lyrics-sized, not full-screen), so a mesh that
// dense would mostly overdraw the same handful of braille cells for no
// visible benefit while still costing the same per-frame compute.
// Was 14x28, tuned for the old (much smaller) projected size — with the
// sphere now rendering significantly larger, that mesh would show
// visible gaps between points instead of a solid-looking surface.
constexpr int kSphereLat = 20;
constexpr int kSphereLon = 40;

const uint8_t kBrailleMap[4][2] = {
    {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}
};

struct Vec3 { float x, y, z; };

Vec3 rotate_yaw_pitch(Vec3 p, float pitch, float yaw) {
    float y1 = p.y * std::cos(pitch) - p.z * std::sin(pitch);
    float z1 = p.y * std::sin(pitch) + p.z * std::cos(pitch);
    float x2 = p.x * std::cos(yaw) + z1 * std::sin(yaw);
    float z2 = -p.x * std::sin(yaw) + z1 * std::cos(yaw);
    return {x2, y1, z2};
}

Vec3 project(Vec3 v, float scale, float distance, int pw, int ph) {
    float z = 1.0f / (distance - v.z);
    return {v.x * z * scale + (pw / 2.0f), v.y * z * scale + (ph / 2.0f), z};
}

// Appends the UTF-8 encoding of a Braille Patterns codepoint
// (U+2800-U+28FF, always 3 bytes) — same technique used elsewhere in
// this codebase for dynamically-computed braille glyphs.
void append_braille(std::string& out, int pattern) {
    int cp = 0x2800 + pattern;
    out += static_cast<char>(0xE2);
    out += static_cast<char>(0xA0 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
}

} // namespace

std::vector<std::string> SphereVisualizer::render(int cols, int rows, const std::vector<int>& bars, double dt) {
    std::vector<std::string> out(std::max(0, rows));
    if (cols <= 0 || rows <= 0) return out;

    int pix_w = cols * 2, pix_h = rows * 4;
    std::vector<bool> buf(static_cast<size_t>(pix_w) * pix_h, false);
    auto set_pixel = [&](int x, int y) {
        if (x >= 0 && x < pix_w && y >= 0 && y < pix_h) buf[static_cast<size_t>(y) * pix_w + x] = true;
    };

    // Reuses the SAME band data the main spectrum strip just computed
    // this frame — no separate FFT here. Bass sits at the CENTER of
    // `bars` (FftVisualizer mirrors bass-to-center/treble-to-edges), so
    // the indices below are chosen relative to that, unlike the
    // reference implementation's own layout where bass was at index 0.
    float bass = 0.0f, macro = 0.0f, micro = 0.0f;
    if (!bars.empty()) {
        size_t n = bars.size();
        size_t center = n / 2;
        size_t inner = center > n / 4 ? center - n / 4 : 0;
        bass = bars[center] / 8.0f;
        macro = bars[inner] / 8.0f;
        micro = bars[0] / 8.0f; // one of the treble edges
    }

    yaw_ += static_cast<float>(dt) * 0.5f;

    const float pitch = 0.3f;
    // Was base_radius=2.0f, camera_dist=12.0f, visual_scale=min_pix*1.5f
    // — proportionally copied from a reference tuned for a different
    // (much larger) rendering context, and far too small in this
    // panel: the projected radius only ever reached about 1 character
    // cell. Bringing the camera closer has a much bigger effect on
    // perspective-projected size than scaling alone (it's a 1/(d-z)
    // term), so that's the main lever pulled here. Points that land
    // outside the panel are already safely clipped by set_pixel's bounds
    // check, so there's no risk in sizing this generously.
    const float base_radius = 2.6f;
    const float camera_dist = 7.0f;
    const float visual_scale = std::min(pix_w, pix_h) * 2.8f;
    float bass_scale = std::clamp(bass * 0.9f, 0.0f, 1.4f);

    for (int lat = 1; lat < kSphereLat; ++lat) {
        float phi = (kPi * lat) / kSphereLat - (kPi / 2.0f);
        for (int lon = 0; lon < kSphereLon; ++lon) {
            float theta = (2.0f * kPi * lon) / kSphereLon;

            float lat_factor = std::abs(std::cos(phi));
            float lon_factor = std::abs(std::sin(theta * 2.0f));
            float surface_map = (lat_factor * 0.6f) + (lon_factor * 0.4f);
            size_t band_idx = bars.empty() ? 0
                : static_cast<size_t>(std::clamp(surface_map, 0.0f, 1.0f) * (bars.size() - 1));
            float base_displace = bars.empty() ? 0.0f : (bars[band_idx] / 8.0f) * 0.6f;

            float macro_warp = macro * 0.35f * std::sin(3.0f * theta) * std::cos(2.0f * phi);
            float micro_warp = micro * 0.22f * std::cos(8.0f * theta);

            float reactive_r = base_radius + bass_scale + base_displace + macro_warp + micro_warp;
            float r_plane = reactive_r * std::cos(phi);
            float z_val = reactive_r * std::sin(phi);
            Vec3 p = {r_plane * std::cos(theta), r_plane * std::sin(theta), z_val};

            Vec3 proj = project(rotate_yaw_pitch(p, pitch, yaw_), visual_scale, camera_dist, pix_w, pix_h);
            set_pixel(static_cast<int>(proj.x), static_cast<int>(proj.y));
        }
    }

    for (int ty = 0; ty < rows; ++ty) {
        std::string line;
        for (int tx = 0; tx < cols; ++tx) {
            uint8_t pattern = 0;
            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 2; ++px) {
                    if (buf[static_cast<size_t>(ty * 4 + py) * pix_w + (tx * 2 + px)]) pattern |= kBrailleMap[py][px];
                }
            }
            if (pattern == 0) line += ' ';
            else append_braille(line, pattern);
        }
        out[ty] = line;
    }
    return out;
}

} // namespace muisc
