#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include "cache_manager.h"

namespace muisc {

namespace fs = std::filesystem;

struct SongResult {
    std::string title;
    std::string artist;   // best-effort, may be empty
    fs::path cached_path;
    bool from_cache = false;
};

class YoutubeSource {
public:
    explicit YoutubeSource(CacheManager& cache) : cache_(cache) {}

    // Resolves `query` to a locally cached .opus file, downloading via
    // yt-dlp only if it isn't already cached. Downloading to disk first
    // (instead of piping yt-dlp -> ffplay) is deliberate: yt-dlp/YouTube
    // throttle concurrent connections per video, so a single cached file
    // is what lets ffplay, the waveform decoder, and any future visualizer
    // all read the same audio independently without re-hitting YouTube.
    std::optional<SongResult> resolve(const std::string& query, std::string* error_out = nullptr);

    // Downloads a specific already-known video (title/id came from a prior
    // search, e.g. OnlineSource::search) straight to cache, skipping the
    // extra "resolve a query" round-trip resolve() does.
    std::optional<SongResult> resolve_by_id(const std::string& video_id, const std::string& title,
                                             const std::string& artist, std::string* error_out = nullptr);

private:
    CacheManager& cache_;
};

} // namespace muisc
