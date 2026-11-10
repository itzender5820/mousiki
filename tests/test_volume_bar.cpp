// The VOLUME BAR's "disk" look: the fast version (only the points of the disc that can land in the 4 dot rows) gives exactly what
// turning every point of the disc gives.
#include "test_framework.h"
#include "volume_bar.h"

#include <cmath>

using namespace muisc;

// The first version: every point of the disc turned and rounded.
static std::vector<std::string> disk_cells_every_point(double angle) {
    using namespace volume_bar_detail;
    const int pw = 2 * kVolBarCells, ph = 4;
    std::vector<unsigned char> lit(static_cast<size_t>(pw * ph), 0);
    const double cx = kDiskCx, cy = kDiskCy, c = std::cos(angle), s = std::sin(angle);
    for (const auto& p : disc().pts) {
        const double x = p.first, y = p.second;
        const int nx = static_cast<int>(std::round(x * c - y * s + cx));
        const int ny = static_cast<int>(std::round(x * s + y * c + cy));
        if (nx >= 0 && nx < pw && ny >= 0 && ny < ph) lit[static_cast<size_t>(ny * pw + nx)] = 1;
    }
    std::vector<std::string> cells;
    for (int i = 0; i < kVolBarCells; ++i) {
        auto d = [&](int row, int col) { return lit[static_cast<size_t>(row * pw + 2 * i + col)] != 0; };
        unsigned bits = 0;
        if (d(0, 0)) bits |= 1u << 0;
        if (d(1, 0)) bits |= 1u << 1;
        if (d(2, 0)) bits |= 1u << 2;
        if (d(0, 1)) bits |= 1u << 3;
        if (d(1, 1)) bits |= 1u << 4;
        if (d(2, 1)) bits |= 1u << 5;
        if (d(3, 0)) bits |= 1u << 6;
        if (d(3, 1)) bits |= 1u << 7;
        cells.push_back(braille(bits));
    }
    return cells;
}

TEST(the_volume_disk_is_the_same_as_turning_every_point_of_the_disc) {
    const double pi = 3.14159265358979323846;
    int bad = 0, n = 0;
    for (int i = -400; i <= 4400; ++i) {   // -0.4 .. 4.4 turns, in small steps
        const double a = 2.0 * pi * i / 1000.0;
        ++n;
        if (volume_bar_detail::disk_cells(a) != disk_cells_every_point(a)) ++bad;
    }
    for (const double a : {0.0, pi / 2, pi, 1.5 * pi, 2 * pi, -pi / 2, 1e-12, pi - 1e-12, 100.0, -100.0}) {   // the straight-on angles too
        ++n;
        if (volume_bar_detail::disk_cells(a) != disk_cells_every_point(a)) ++bad;
    }
    CHECK(n > 4000);
    CHECK(bad == 0);
}
