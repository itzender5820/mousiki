// Download cache: names and locates the files of online tracks (YouTube, SoundCloud, Bandcamp) in the
// download folder, sanitises titles into safe file names, and records finished downloads in the
// archive.
#include "cache_manager.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "path_utf8.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <vector>

namespace muisc {

CacheManager::CacheManager() {
    const char* home = std::getenv("HOME");
    // HOME is stored as UTF-8 by win_bootstrap_env() (it comes out of
    // GetEnvironmentVariableW, which is UTF-16). fs::path(std::string)
    // would re-read those bytes as ANSI, so a user profile with a
    // non-ASCII name produced a garbled base directory.
    fs::path base = home ? path_from_utf8(home) : fs::path(".");
    cache_dir_ = base / ".cache" / "mousiki";
    download_dir_ = cache_dir_; // until Settings says otherwise (see set_download_dir)
    std::error_code ec;
    fs::create_directories(cache_dir_, ec); // ignore failure, we surface it on first write instead
}

void CacheManager::set_download_dir(const fs::path& dir) {
    if (dir.empty() || dir == cache_dir_) {
        download_dir_ = cache_dir_;
        return;
    }
    std::error_code ec;
    fs::create_directories(dir, ec); // a not-yet-existing folder is normal: this is where the next download goes
    download_dir_ = dir;
}

// Turns a track title into a filename.
//
// The previous version kept only [A-Za-z0-9] (lowercased) plus space/dash/
// underscore and dropped every other byte. For a UTF-8 title that is a
// disaster: a Japanese, Cyrillic or Greek title has no ASCII alphanumerics at
// all, so it collapsed to "untitled" -- and a mixed title like
// "\u591c\u306b\u99c6\u3051\u308b (YOASOBI)" kept only the latin fragment, which is exactly the
// "part of the title" showing up in the local list, since the list labels
// cached tracks by their filename stem. Worse, std::isalnum() is
// locale-dependent: under a single-byte locale it accepts bytes >= 0x80, so
// some UTF-8 lead bytes survived and were then case-mapped individually,
// producing broken sequences rather than clean removal.
//
// Non-ASCII characters are perfectly legal in NTFS filenames. The only
// characters Windows actually forbids are ASCII (< > : " / \\ | ? * and the
// control range), all of which this already excludes by construction. So the
// rule is now: fold ASCII letters to lowercase, keep ASCII digits, turn
// space/dash/underscore into a single underscore, keep every non-ASCII
// codepoint verbatim, and drop the rest.
std::string CacheManager::sanitize(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());

    size_t i = 0;
    while (i < raw.size()) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c < 0x80) {
            if (ascii_alnum(c)) out += ascii_lower(static_cast<char>(c));
            else if (c == ' ' || c == '-' || c == '_') out += '_';
            // everything else (slashes, quotes, colons, ...) is dropped
            ++i;
            continue;
        }
        // Multi-byte sequence: copy it whole, or skip a malformed lead byte.
        size_t len = static_cast<size_t>(utf8_seq_len(c));
        if (len <= 1 || i + len > raw.size()) { ++i; continue; }
        bool well_formed = true;
        for (size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(raw[i + k]) & 0xC0) != 0x80) { well_formed = false; break; }
        }
        if (!well_formed) { ++i; continue; }
        out.append(raw, i, len);
        i += len;
    }

    while (out.find("__") != std::string::npos) {
        out.replace(out.find("__"), 2, "_");
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    // A trailing dot or space is silently stripped by Windows itself, which
    // would make the name we write and the name we later look up disagree.
    while (!out.empty() && (out.back() == '_' || out.back() == '.' || out.back() == ' ')) out.pop_back();

    // Keep the stem well inside MAX_PATH once the cache directory and
    // extension are prepended. Cut on a codepoint boundary so the result is
    // still valid UTF-8.
    constexpr size_t kMaxStemBytes = 120;
    if (out.size() > kMaxStemBytes) {
        size_t cut = kMaxStemBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }

    if (out.empty()) out = "untitled";
    return out;
}

// The pre-UTF-8 rule, kept for one purpose only: finding tracks that were
// already downloaded under the old naming scheme, so changing the scheme
// doesn't silently orphan an existing cache and re-download everything.
std::string CacheManager::legacy_sanitize(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (ascii_alnum(c)) out += ascii_lower(static_cast<char>(c));
        else if (c == ' ' || c == '-' || c == '_') out += '_';
    }
    while (out.find("__") != std::string::npos) {
        out.replace(out.find("__"), 2, "_");
    }
    if (out.empty()) out = "untitled";
    return out;
}

fs::path CacheManager::path_for(const std::string& title, const std::string& ext) const {
    std::error_code ec;
    const std::string stem = sanitize(title);
    const std::string legacy_stem = legacy_sanitize(title);
    fs::path found;

    auto existing_in = [&](const fs::path& dir, const std::string& s) {
        if (!found.empty()) return;
        fs::path p = dir / (s + "." + ext);
        if (fs::exists(p, ec)) found = p;
    };

    // The download folder first -- current naming, then the pre-UTF-8 one.
    existing_in(download_dir_, stem);
    existing_in(download_dir_, legacy_stem);

    // A folder configured LATER must not orphan what is already downloaded:
    // before declaring a title never fetched, look in the default cache
    // folder too (which is the only candidate when no folder is configured).
    if (download_dir_ != cache_dir_) {
        existing_in(cache_dir_, stem);
        existing_in(cache_dir_, legacy_stem);
    }

    if (!found.empty()) return found;
    return download_dir_ / (stem + "." + ext); // doesn't exist yet: this is the name a download writes
}

fs::path CacheManager::download_archive_path() const {
    return download_dir_ / "history" / "downloaded_history.txt";
}

void CacheManager::record_download(const std::string& extractor, const std::string& id) const {
    // Only plain ids (a YouTube video id is 11 characters of A-Z a-z 0-9 _ -); URLs and odd input are not recorded.
    if (id.size() < 5 || id.size() > 40) return;
    for (unsigned char c : id) if (!(std::isalnum(c) || c == '_' || c == '-')) return;
    static std::mutex m;   // downloads run on several threads
    std::lock_guard<std::mutex> lk(m);
    std::error_code ec;
    const fs::path file = download_archive_path();
    fs::create_directories(file.parent_path(), ec);
    if (ec) return;
    const fs::path loose = download_dir_ / "downloaded_history.txt";   // an archive yt-dlp left in the download folder itself
    if (!fs::exists(file, ec) && fs::exists(loose, ec)) {
        fs::rename(loose, file, ec);
        if (ec) { ec.clear(); fs::copy_file(loose, file, ec); if (!ec) fs::remove(loose, ec); }
    }
    append_unique_line(file, extractor + " " + id);
}

void CacheManager::append_unique_line(const fs::path& file, const std::string& entry) {
    bool needs_newline = false;
    {
        std::ifstream in(file, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == entry) return;
        }
    }
    {
        std::error_code ec;
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        if (in && in.tellg() > 0) { in.seekg(-1, std::ios::end); char last = 0; in.get(last); needs_newline = last != '\n'; }
    }
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (out) out << (needs_newline ? "\n" : "") << entry << "\n";
}

fs::path CacheManager::video_archive_path() const { return download_dir_ / "history" / "downloaded_videos.txt"; }

static bool plain_video_id(const std::string& id) {
    if (id.size() < 5 || id.size() > 40) return false;
    for (unsigned char c : id) if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    return true;
}

void CacheManager::record_video_download(const std::string& id, const std::string& file) const {
    if (!plain_video_id(id)) return;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    std::error_code ec;
    const fs::path arch = video_archive_path();
    fs::create_directories(arch.parent_path(), ec);
    if (ec) return;
    append_unique_line(arch, "youtube " + id);
    // id TAB file: one line per video (a new download of the same id replaces the line)
    const fs::path map = arch.parent_path() / "downloaded_videos_files.txt";
    std::vector<std::string> lines;
    {
        std::ifstream in(map, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.compare(0, id.size() + 1, id + "\t") == 0) continue;
            lines.push_back(line);
        }
    }
    lines.push_back(id + "\t" + file);
    std::ofstream out(map, std::ios::binary | std::ios::trunc);
    for (const std::string& l : lines) out << l << "\n";
}

fs::path CacheManager::find_video_download(const std::string& id) const {
    if (!plain_video_id(id)) return {};
    std::error_code ec;
    const fs::path map = video_archive_path().parent_path() / "downloaded_videos_files.txt";
    {
        std::ifstream in(map, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, id.size() + 1, id + "\t") != 0) continue;
            const fs::path f = path_from_utf8(line.substr(id.size() + 1));
            if (fs::is_regular_file(f, ec) && fs::file_size(f, ec) > 0) return f;
        }
    }
    const std::string tag = "[" + id + "]";   // down_vid names its files "<title> [<id>].mp4"
    for (fs::directory_iterator it(download_dir_ / "video_down", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = path_utf8(it->path().filename());
        const std::string ext = path_utf8(it->path().extension());
        if (name.find(tag) == std::string::npos || ext == ".part" || ext == ".ytdl" || ext == ".temp") continue;
        std::error_code e2;
        if (fs::is_regular_file(it->path(), e2) && fs::file_size(it->path(), e2) > 0) return it->path();
    }
    return {};
}

bool CacheManager::is_cached(const std::string& title, const std::string& ext) const {
    std::error_code ec;
    auto p = path_for(title, ext);
    return fs::exists(p, ec) && fs::file_size(p, ec) > 0;
}

} // namespace muisc
