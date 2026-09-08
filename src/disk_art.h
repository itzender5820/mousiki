#pragma once
#include <string>
#include <vector>

namespace muisc {

// Renders the rotating Braille-dot "disk" seen in the mockup. This is a
// direct port of the rotation algorithm supplied in beta-ui.txt (decode
// the artwork into individual dots once, then rotate the dot cloud around
// its center each frame and re-encode to Braille) — logic unchanged, just
// wrapped in a reusable class instead of a standalone main().
class DiskArt {
public:
    DiskArt();

    // One rotated frame at `angle_radians`, as printable lines (no trailing
    // newlines). Always `height()` lines of `width()` Braille cells each.
    std::vector<std::string> frame(double angle_radians) const;

    int width() const { return width_; }
    int height() const { return height_; }

private:
    struct Dot { double x; double y; };

    std::vector<Dot> dots_;
    int width_ = 0;
    int height_ = 0;
};

} // namespace muisc
