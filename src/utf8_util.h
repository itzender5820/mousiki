#pragma once
#include <string>
#include <cstdint>
#include <algorithm>

namespace muisc {

inline int utf8_seq_len(unsigned char lead) {
    if ((lead & 0x80) == 0x00) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

inline uint32_t utf8_decode(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int len = utf8_seq_len(c);
    len = static_cast<int>(std::min<size_t>(static_cast<size_t>(len), s.size() - i));
    uint32_t cp;
    if (len <= 1) {
        cp = c;
    } else {
        cp = c & (0xFF >> (len + 1));
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    i += static_cast<size_t>(len);
    return cp;
}

} // namespace muisc
