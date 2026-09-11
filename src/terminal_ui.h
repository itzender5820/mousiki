#pragma once
#include <string>

namespace muisc {

// Raw, non-canonical, no-echo terminal mode + non-blocking key reads.
// Panel/box drawing lives in app.cpp; this is just the terminal plumbing.
class TerminalIO {
public:
    TerminalIO();
    ~TerminalIO();

    void restore();

    // Non-blocking single "logical" key read. Arrow keys (3-byte escape
    // sequences) collapse to 'A'/'B'/'C'/'D' (up/down/right/left). A lone
    // Escape key returns 27. Backspace returns 127. Returns 0 if nothing
    // is waiting.
    int poll_key();

    int rows() const;
    int cols() const;

private:
    bool raw_mode_active_ = false;
    void reassert_raw_mode(); // see poll_key()'s definition for why this exists
};

// Truncates/right-pads (by byte length — good enough for the mostly-ASCII
// UI text here; multi-byte titles may render slightly short) to exactly
// `width` visible columns.
std::string pad_right(const std::string& s, int width);
std::string pad_left(const std::string& s, int width);
std::string truncate_str(const std::string& s, int width);
std::string utf8_take(const std::string& s, int width);

// Computes terminal display width of a UTF-8 string based on wcwidth.
int display_width(const std::string& s);

} // namespace muisc
