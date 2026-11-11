// The library's tags between runs (see tag_cache.h).
//
// One line per file, tab-separated: path, size, mtime, duration, title, artist, album, year. A tab, line break or backslash
// inside a field is written as \t, \n, \r or \\. The first line names the format; a file with another first line is ignored
// (read again from scratch), so a later change of the format only costs one sweep.
#include "app_dirs.h"
#include "tag_cache.h"
#include "path_utf8.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace muisc {

namespace {
constexpr const char* kHeader = "# mousiki tag cache 1";

std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\t') o += "\\t";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}

std::string unesc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        const char n = s[++i];
        o += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
    }
    return o;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t t = line.find('\t', start);
        out.push_back(line.substr(start, t == std::string::npos ? std::string::npos : t - start));
        if (t == std::string::npos) break;
        start = t + 1;
    }
    return out;
}
} // namespace

TagStamp tag_stamp(const fs::path& file) {
    TagStamp st;
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) return st;
    const auto t = fs::last_write_time(file, ec);
    if (ec) return st;
    st.size = static_cast<long long>(size);
    st.mtime = static_cast<long long>(t.time_since_epoch().count());
    return st;
}

fs::path tag_cache_path(bool books) {
    const char* home = std::getenv("HOME");   // (UTF-8 on Windows too, see win_bootstrap_env())
    const fs::path base = home ? path_from_utf8(home) : fs::path(".");
    return muisc::app_dirs::cache() / (books ? "tag_cache_books.txt" : "tag_cache.txt");
}

TagCacheMap tag_cache_load(const fs::path& file) {
    TagCacheMap map;
    std::ifstream in(file, std::ios::binary);
    if (!in) return map;
    std::string line;
    if (!std::getline(in, line)) return map;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != kHeader) return map;   // another format: read everything again
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> f = split_tabs(line);
        if (f.size() != 8 || f[0].empty()) continue;
        TagCacheEntry e;
        try {
            e.stamp.size = std::stoll(f[1]);
            e.stamp.mtime = std::stoll(f[2]);
            e.meta.duration_sec = std::stod(f[3]);
        } catch (...) { continue; }
        e.meta.title = unesc(f[4]);
        e.meta.artist = unesc(f[5]);
        e.meta.album = unesc(f[6]);
        e.meta.year = unesc(f[7]);
        e.meta.tags_resolved = true;
        map[unesc(f[0])] = std::move(e);
    }
    return map;
}

bool tag_cache_save(const fs::path& file, const TagCacheMap& map) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << kHeader << "\n";
        char num[32];
        for (const auto& [key, e] : map) {
            std::snprintf(num, sizeof num, "%.3f", e.meta.duration_sec);
            out << esc(key) << '\t' << e.stamp.size << '\t' << e.stamp.mtime << '\t' << num << '\t' << esc(e.meta.title) << '\t'
                << esc(e.meta.artist) << '\t' << esc(e.meta.album) << '\t' << esc(e.meta.year) << '\n';
        }
        if (!out) return false;
    }
    fs::rename(tmp, file, ec);
    if (ec) {   // (Windows: rename onto an existing file can fail -- remove it first)
        fs::remove(file, ec);
        ec.clear();
        fs::rename(tmp, file, ec);
    }
    return !ec;
}

} // namespace muisc
