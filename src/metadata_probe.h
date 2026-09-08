#pragma once
#include <filesystem>
#include <string>

namespace muisc {

namespace fs = std::filesystem;

struct TrackMetadata {
    std::string name;
    std::string artist = "-";
    std::string year = "-";
    std::string sampling = "-";
    std::string type = "audio only";
    std::string format = "-";
    std::string file_size = "-";
    std::string location = "-";
    std::string extra_label;  // e.g. "Video ID" — empty if unused
    std::string extra_value;
};

// Runs `ffprobe` once against `file` and fills in what it can. Missing
// tags are left as "-" rather than failing the whole probe.
// Fast, narrow probe for just the duration (used to render the mm:ss
// column in file lists without paying for a full metadata probe per row).
// Returns -1 on failure.
double probe_duration_seconds(const fs::path& file);

// One ffprobe call for both duration and the real artist tag — used to
// populate the file list's Artist column properly instead of guessing
// from the parent folder name.
struct RowMeta {
    std::string artist; // empty if untagged
    double duration_sec = -1.0;
};
RowMeta probe_row_meta(const fs::path& file);

TrackMetadata probe_metadata(const fs::path& file, const std::string& fallback_name,
                              const std::string& fallback_artist, const std::string& location_label);

} // namespace muisc
