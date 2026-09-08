#include "local_source.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace muisc {

bool LocalSource::is_audio_file(const fs::path& p) {
    static const std::set<std::string> exts = {".wav", ".mp3", ".opus", ".flac", ".ogg", ".m4a", ".aac", ".webm"};
    return exts.count(p.extension().string()) > 0;
}

std::vector<LocalTrack> LocalSource::scan(const std::vector<std::string>& custom_paths) const {
    std::vector<LocalTrack> tracks;
    std::vector<fs::path> roots;

    if (!custom_paths.empty()) {
        for (const auto& cp : custom_paths) {
            roots.push_back(fs::path(cp));
        }
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            roots.push_back(fs::path(home) / "Music");
            roots.push_back(fs::path(home) / "disk" / "Music");
        }
    }

    if (roots.empty()) return tracks;

    // Dedupe by resolved (symlink-following) path — if one root is a
    // symlink that overlaps with the other (common on Android, e.g.
    // "disk" pointing into shared storage that also contains "Music"),
    // recursive_directory_iterator would otherwise walk and list the
    // exact same file twice, once per root.
    std::set<std::string> seen_canonical;

    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (entry.is_regular_file() && is_audio_file(entry.path())) {
                std::error_code cec;
                fs::path canon = fs::canonical(entry.path(), cec);
                std::string key = cec ? entry.path().string() : canon.string();
                if (!seen_canonical.insert(key).second) continue; // already listed via another root

                std::string folder = entry.path().parent_path().filename().string();
                if (folder.empty() || fs::path(folder) == root.filename()) folder = "-";
                tracks.push_back({entry.path().stem().string(), entry.path(), folder});
            }
        }
    }
    return tracks;
}

std::optional<LocalTrack> LocalSource::find(const std::string& query, const std::vector<std::string>& custom_paths) const {
    std::string needle = query;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);

    for (const auto& track : scan(custom_paths)) {
        std::string hay = track.title;
        std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
        if (hay.find(needle) != std::string::npos) return track;
    }
    return std::nullopt;
}

} // namespace muisc
