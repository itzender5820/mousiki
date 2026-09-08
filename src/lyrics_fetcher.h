#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// One LRC line, optionally with per-word timestamps (enhanced/A2 LRC),
// which is what makes word-level highlighting possible during playback.
struct LyricLine {
    double start_time = 0.0;
    std::string full_text;
    std::vector<std::pair<double, std::string>> words; // empty if not word-synced
};

enum class LyricsStatus {
    Ok,
    ModuleMissing,   // syncedlyrics not installed -> show pip-install hint
    PythonMissing,   // python3 not found on PATH
    NotFound,        // ran fine, no lyrics available for this track
    Error,
};

struct LyricsResult {
    LyricsStatus status = LyricsStatus::Error;
    std::vector<LyricLine> lines;
    std::string message;   // human-readable status/error, shown in the lyrics panel
    std::string source;    // "local" | "syncedlyrics" | ""
    std::string raw_lrc;   // the raw LRC text, kept so it can be cached to a sidecar file
};

// Priority chain: a local sidecar .lrc file next to `track_path` (checked
// first, no subprocess spawned at all) -> syncedlyrics (word-level
// "enhanced" search, falling back to plain line-synced search). Paxsenix
// was previously a fallback source here but was dropped for being
// unreliable — syncedlyrics is slower but consistently accurate, which
// matters more than speed for a background fetch. Whatever comes back
// from a network fetch is written back to the sidecar file, so the next
// time this track plays (even offline) it's a local-file hit.
//
// "Better Lyrics" was requested for this chain too but isn't wired in:
// its API needs a YouTube video ID plus a Google API key behind a
// self-hosted Cloudflare Worker (see better-lyrics/api on GitHub) — not
// something with a simple public endpoint to call directly, so it's
// skipped rather than faked.
LyricsResult fetch_synced_lyrics(const std::string& title, const std::string& artist,
                                  const std::string& helper_script_path,
                                  const fs::path& track_path = fs::path());

} // namespace muisc
