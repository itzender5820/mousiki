// Small text helpers shared by the whole program. Everything here works on bytes: UTF-8 sequences pass through
// unchanged, so they are safe to use on any text (see utf8_util.h for the width and case helpers).
#pragma once
#include <cctype>
#include <climits>
#include <exception>
#include <optional>
#include <string>

namespace muisc {

// True for the ASCII whitespace characters: space, tab, LF, VT, FF, CR. Written out instead of std::isspace, whose answer
// depends on the C locale (some Windows code pages report bytes above 127 as spaces).
inline bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// Strips leading and trailing whitespace (see is_ascii_space). Returns an empty string for blank input.
inline std::string trim_ws(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_ascii_space(s[a])) ++a;
    while (b > a && is_ascii_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// The number at the start of `s`, like std::atoi: leading whitespace, an optional sign, then digits; anything else after them is
// ignored, and text without digits gives 0. Unlike std::atoi it is defined for every input: a number outside the range of int
// stops at INT_MAX / INT_MIN instead of overflowing (undefined behaviour), and a null pointer gives 0.
inline int atoi_safe(const char* s) {
    if (!s) return 0;
    while (is_ascii_space(*s)) ++s;
    bool neg = false;
    if (*s == '+' || *s == '-') { neg = *s == '-'; ++s; }
    long long v = 0;
    for (; *s >= '0' && *s <= '9'; ++s) {
        v = v * 10 + (*s - '0');
        if (v > static_cast<long long>(INT_MAX) + 1) v = static_cast<long long>(INT_MAX) + 1;   // keeps counting digits without overflowing
    }
    if (neg) return v >= static_cast<long long>(INT_MAX) + 1 ? INT_MIN : static_cast<int>(-v);
    return v > INT_MAX ? INT_MAX : static_cast<int>(v);
}
inline int atoi_safe(const std::string& s) { return atoi_safe(s.c_str()); }

// std::stoi / std::stod without the exception: the same rules (leading whitespace, a sign, the digits at the start; text after them is
// ignored), and an empty answer where std::stoi / std::stod would throw (no number at all, or one outside the range of the type).
// Settings readers use them so that a bad value leaves the setting as it was.
inline std::optional<int> parse_int(const std::string& s) {
    try { return std::stoi(s); } catch (const std::exception&) { return std::nullopt; }
}
inline std::optional<double> parse_double(const std::string& s) {
    try { return std::stod(s); } catch (const std::exception&) { return std::nullopt; }
}

} // namespace muisc
