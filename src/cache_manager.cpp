#include "cache_manager.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace muisc {

CacheManager::CacheManager() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::path(".");
    cache_dir_ = base / ".cache" / "mousiki";
    std::error_code ec;
    fs::create_directories(cache_dir_, ec); // ignore failure, we surface it on first write instead
}

std::string CacheManager::sanitize(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c)) {
            out += static_cast<char>(std::tolower(c));
        } else if (c == ' ' || c == '-' || c == '_') {
            out += '_';
        }
        // everything else (slashes, quotes, emoji, etc.) is dropped
    }
    while (out.find("__") != std::string::npos) {
        out.replace(out.find("__"), 2, "_");
    }
    if (out.empty()) out = "untitled";
    return out;
}

fs::path CacheManager::path_for(const std::string& title, const std::string& ext) const {
    return cache_dir_ / (sanitize(title) + "." + ext);
}

bool CacheManager::is_cached(const std::string& title, const std::string& ext) const {
    std::error_code ec;
    auto p = path_for(title, ext);
    return fs::exists(p, ec) && fs::file_size(p, ec) > 0;
}

} // namespace muisc
