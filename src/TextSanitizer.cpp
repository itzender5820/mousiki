#include "TextSanitizer.h"
#include "utf8_util.h"
#include <cstdint>

namespace muisc {

static constexpr uint32_t blacklist[] = {
    0x3164, // HANGUL FILLER
};

std::string sanitize_lyric_text(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        size_t start = i;
        uint32_t cp = utf8_decode(text, i);
        
        bool blacklisted = false;
        for (uint32_t b : blacklist) {
            if (cp == b) {
                blacklisted = true;
                break;
            }
        }
        
        if (!blacklisted) {
            result.append(text, start, i - start);
        }
    }
    return result;
}

} // namespace muisc
