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
    // Used to lowercase everything and collapse spaces/punctuation down
    // to underscores -- safe, but the cached filename stopped reading as
    // the actual title at all. Now only strips characters that are
    // genuinely illegal/problematic as filenames (path separators, null,
    // plus a few Windows-reserved characters kept out defensively even
    // on POSIX, in case this cache dir ever ends up synced onto a
    // filesystem that cares) -- everything else, including spaces, case,
    // and most punctuation, is kept as-is.
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c == '/' || c == '\\' || c == '\0' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            continue; // drop -- genuinely unsafe/reserved
        }
        out += static_cast<char>(c);
    }

    // Trim leading/trailing whitespace and dots -- trailing dots/spaces
    // are also filesystem-problematic on some platforms (Windows/FAT).
    size_t b = 0, e = out.size();
    while (b < e && (out[b] == ' ' || out[b] == '.')) ++b;
    while (e > b && (out[e - 1] == ' ' || out[e - 1] == '.')) --e;
    out = out.substr(b, e - b);

    if (out.empty()) out = "untitled";

    // Cap length defensively (most filesystems limit filenames to 255
    // bytes; a long title + extension could exceed that) -- back off
    // from any trailing UTF-8 continuation byte first so this can't
    // slice a multi-byte character in half and leave invalid UTF-8 in
    // the filename.
    constexpr size_t kMaxLen = 200;
    if (out.size() > kMaxLen) {
        size_t cut = kMaxLen;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out = out.substr(0, cut);
    }

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
