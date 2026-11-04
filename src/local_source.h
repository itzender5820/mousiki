// The local library (see local_source.cpp): LocalTrack describes one file, LocalSource scans the music
// folders and finds tracks.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// A folder with a file of this name is the creator's folder (the ^ overlay): the library scan leaves its files out, they are
// only reachable through the overlay's "Created files..." list.
inline constexpr const char* kCreatorMarker = ".mousiki-creator";

struct LocalTrack {
    std::string title;   // filename stem
    fs::path path;
    std::string folder_artist; // parent directory name, used as a cheap
                                // "Artist" column guess for the list view
                                // (real tag lookup is reserved for the
                                // currently-loaded track's metadata panel)
};

class LocalSource {
public:
    // Scans the provided paths, falling back to defaults if empty.
    // `diagnostics`, if non-null, gets one line appended per root -- whether
    // it existed as a directory and how many audio files were found under
    // it (or, if it didn't exist/wasn't a directory, says so). Left null by
    // find()'s call site below, since that's an internal lookup rather than
    // a user-visible scan the log should report on.
    std::vector<LocalTrack> scan(const std::vector<std::string>& custom_paths = {},
                                  std::vector<std::string>* diagnostics = nullptr) const;

    // Case-insensitive substring match against title.
    std::optional<LocalTrack> find(const std::string& query, const std::vector<std::string>& custom_paths = {}) const;

    // Video files (mp4, mkv, mov, avi, webm ...): listed only while the Video mode is on (set_include_video(true)); they play as audio
    // (ffmpeg decodes the sound) and the Video view shows their picture. ".webm" can also be an audio-only download.
    static bool is_video_file(const fs::path& p);
    void set_include_video(bool on) { include_video_ = on; }
    bool include_video() const { return include_video_; }

private:
    bool include_video_ = false;
    static bool is_audio_file(const fs::path& p);
};

} // namespace muisc
