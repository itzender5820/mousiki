#include "disk_art.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace muisc {

namespace {

constexpr double PI = 3.14159265358979323846;

// Braille dot layout:
//   1 4
//   2 5
//   3 6
//   7 8
// Unicode Braille: U+2800 + dot bitmask

uint32_t decodeUTF8(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    if ((c & 0xE0) == 0xC0) {
        uint32_t r = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
        i += 2; return r;
    }
    if ((c & 0xF0) == 0xE0) {
        uint32_t r = ((c & 0x0F) << 12) |
                     ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                     (static_cast<unsigned char>(s[i + 2]) & 0x3F);
        i += 3; return r;
    }
    uint32_t r = ((c & 0x07) << 18) |
                 ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    i += 4; return r;
}

bool brailleDots(uint32_t cp, bool dots[4][2]) {
    if (cp < 0x2800 || cp > 0x28FF) return false;
    uint8_t bits = static_cast<uint8_t>(cp - 0x2800);
    dots[0][0] = bits & (1 << 0);
    dots[1][0] = bits & (1 << 1);
    dots[2][0] = bits & (1 << 2);
    dots[0][1] = bits & (1 << 3);
    dots[1][1] = bits & (1 << 4);
    dots[2][1] = bits & (1 << 5);
    dots[3][0] = bits & (1 << 6);
    dots[3][1] = bits & (1 << 7);
    return true;
}

std::string encodeBraille(bool dots[4][2]) {
    uint8_t bits = 0;
    if (dots[0][0]) bits |= 1 << 0;
    if (dots[1][0]) bits |= 1 << 1;
    if (dots[2][0]) bits |= 1 << 2;
    if (dots[0][1]) bits |= 1 << 3;
    if (dots[1][1]) bits |= 1 << 4;
    if (dots[2][1]) bits |= 1 << 5;
    if (dots[3][0]) bits |= 1 << 6;
    if (dots[3][1]) bits |= 1 << 7;

    uint32_t cp = 0x2800 + bits;
    std::string out;
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
    return out;
}

// The circular artwork from beta-ui.txt's disk rotation demo.
const char* kSourceArt =
"⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣀⣠⣤⣤⣤⣤⣄⣀⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⠀⠀⠀⣠⣴⣾⣿⣿⣿⣿⣿⣿⣿⣿⣿⠃⠀⠀⢀⠀⠀⠀⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⢀⣴⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡏⠀⠀⣠⣿⣿⣦⡀⠀⠀⠀⠀\n"
"⠀⠀⠀⣠⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠁⠀⣴⣿⣿⣿⣿⣿⣄⠀⠀⠀\n"
"⠀⠀⣰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠿⠿⢿⠃⢀⣾⣿⣿⣿⣿⣿⣿⣿⣆⠀⠀\n"
"⠀⢰⣿⣿⣿⣿⣿⣿⣿⣿⡿⠋⣡⣤⣶⣶⣤⣄⠘⢿⣿⣿⣿⣿⣿⣿⣿⣿⡆⠀\n"
"⠀⣾⣿⣿⣿⣿⣿⣿⣿⡿⠀⣾⣿⠟⠉⠉⠻⣿⣷⠀⢿⣿⣿⣿⣿⣿⣿⣿⣷⠀\n"
"⠀⣿⣿⣿⣿⣿⣿⣿⣿⡇⢸⣿⡇⠀⠀⠀⠀⢸⣿⡇⢸⣿⣿⣿⣿⣿⣿⣿⣿⠀\n"
"⠀⠿⠿⠟⠛⠛⢉⣉⣡⡤⠀⢿⣿⣤⣀⣀⣤⣿⡿⠀⣾⣿⣿⣿⣿⣿⣿⣿⡿⠀\n"
"⠀⠀⣤⣴⣶⡿⠟⢋⣡⣶⣶⣄⠙⠛⠿⠿⠛⠋⣠⣾⣿⣿⣿⣿⣿⣿⣿⣿⠇⠀\n"
"⠀⠀⠙⠋⢁⣠⣶⣿⣿⣿⣿⣿⣿⣷⣶⣶⣾⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠏⠀⠀\n"
"⠀⠀⠀⠰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠏⠀⠀⠀\n"
"⠀⠀⠀⠀⠈⠻⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠟⠁⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⠀⠀⠈⠙⠻⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠟⠋⠁⠀⠀⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠈⠉⠙⛛⛛⛛⛛⛛⠉⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀";

std::vector<std::string> split_lines(const std::string& source) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= source.size()) {
        size_t end = source.find('\n', start);
        if (end == std::string::npos) { lines.push_back(source.substr(start)); break; }
        lines.push_back(source.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

} // namespace

DiskArt::DiskArt() {
    std::vector<std::string> lines = split_lines(kSourceArt);

    height_ = static_cast<int>(lines.size());
    width_ = 0;
    for (const auto& line : lines) {
        int cells = 0;
        for (size_t i = 0; i < line.size();) { decodeUTF8(line, i); ++cells; }
        width_ = std::max(width_, cells);
    }

    for (int row = 0; row < height_; ++row) {
        int col = 0;
        for (size_t i = 0; i < lines[row].size();) {
            uint32_t cp = decodeUTF8(lines[row], i);
            bool b[4][2]{};
            if (brailleDots(cp, b)) {
                for (int dy = 0; dy < 4; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        if (!b[dy][dx]) continue;
                        dots_.push_back({col * 2.0 + dx, row * 4.0 + dy});
                    }
                }
            }
            ++col;
        }
    }
}

std::vector<std::string> DiskArt::frame(double angle) const {
    const int pixelWidth = width_ * 2;
    const int pixelHeight = height_ * 4;

    std::vector<std::vector<bool>> bitmap(pixelHeight, std::vector<bool>(pixelWidth, false));

    const double cx = (pixelWidth - 1) / 2.0;
    const double cy = (pixelHeight - 1) / 2.0;
    const double c = std::cos(angle);
    const double s = std::sin(angle);

    for (const auto& p : dots_) {
        double x = p.x - cx;
        double y = p.y - cy;
        // Clockwise in terminal coords (Y points down).
        double rx = x * c - y * s;
        double ry = x * s + y * c;
        int nx = static_cast<int>(std::round(rx + cx));
        int ny = static_cast<int>(std::round(ry + cy));
        if (nx >= 0 && nx < pixelWidth && ny >= 0 && ny < pixelHeight) {
            bitmap[ny][nx] = true;
        }
    }

    std::vector<std::string> result;
    result.reserve(height_);
    for (int row = 0; row < height_; ++row) {
        std::string line;
        for (int col = 0; col < width_; ++col) {
            bool dots[4][2]{};
            for (int dy = 0; dy < 4; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    int x = col * 2 + dx;
                    int y = row * 4 + dy;
                    if (x < pixelWidth && y < pixelHeight) dots[dy][dx] = bitmap[y][x];
                }
            }
            line += encodeBraille(dots);
        }
        result.push_back(std::move(line));
    }
    return result;
}

} // namespace muisc
