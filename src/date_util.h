// date_util.h -- calendar helpers of the listening history (the player's history.cpp and the radio's radio_history.cpp share them).
#pragma once
#include <cstdio>
#include <ctime>
#include <string>
#if defined(_WIN32)
#include "win_compat.h"   // win_localtime -- MSVC's localtime_s has swapped arguments
#endif

namespace muisc {

// Local time of a unix time (thread safe).
inline std::tm local_tm(long long unix_sec) {
    std::tm out{};
    std::time_t t = static_cast<std::time_t>(unix_sec);
#if defined(_WIN32)
    out = win_localtime(t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

// "2026-09-27" -- the day bucket the per-day totals are summed into.
inline std::string day_key(long long unix_sec) {
    std::tm tm = local_tm(unix_sec);
    char buf[48];   // room for any int, so the compiler can see the text always fits
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

// "YYYY-MM-DD" -> days since 1970-01-01 (proleptic Gregorian, H. Hinnant's days_from_civil). Pure date arithmetic, so
// no time zone or DST can make two calendar days come out 23 or 25 hours apart.
inline bool civil_day_number(const std::string& key, long long& out) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(key.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    y -= m <= 2 ? 1 : 0;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    out = era * 146097 + doe - 719468;
    return true;
}

// Weekday of a civil_day_number(), 0 = Monday (1970-01-01 was a Thursday).
inline int monday_first_weekday(long long days) {
    return static_cast<int>(((days % 7) + 7 + 3) % 7);
}

} // namespace muisc
