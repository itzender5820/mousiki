// Download cache (see cache_manager.cpp): where downloaded online tracks are stored, how their file
// names are built, and whether a track is already cached.
#pragma once
#include <filesystem>
#include <string>

namespace muisc {

namespace fs = std::filesystem;

class CacheManager {
public:
    CacheManager();

    // $HOME/.cache/muisc  (created if missing)
    const fs::path& cache_dir() const { return cache_dir_; }

    // Where yt-dlp writes: the configured download folder, or -- when none
    // is set -- the cache folder itself. Kept separate from cache_dir_ so
    // anything else living under ~/.cache/mousiki stays where it always was,
    // and so a folder set later still finds the downloads already in the
    // cache (path_for() falls back to it).
    const fs::path& download_dir() const { return download_dir_; }
    // Empty (or exactly the cache folder) restores the default; anything
    // else replaces it, creating the folder if needed.
    void set_download_dir(const fs::path& dir);

    // Deterministic, filesystem-safe path for a given song title, e.g.
    // "Never Gonna Give You Up" -> ~/.cache/muisc/never_gonna_give_you_up.opus
    fs::path path_for(const std::string& title, const std::string& ext = "opus") const;

    bool is_cached(const std::string& title, const std::string& ext = "opus") const;

    // The download archive shared with yt-dlp runs made outside Mousiki: <download folder>/history/downloaded_history.txt,
    // in yt-dlp's own format ("youtube <video id>" per line), so `yt-dlp --download-archive <this file>` in a terminal
    // skips what Mousiki fetched and the other way round. Kept in a sub-folder so it is not moved along with the music.
    fs::path download_archive_path() const;
    // Records a finished download ("youtube", "dQw4w9WgXcQ") unless the archive has it already. The file is only ever
    // appended to -- Mousiki itself does not skip downloads because of it (a deleted song can always be fetched again).
    // A downloaded_history.txt lying directly in the download folder is moved into the history folder first.
    void record_download(const std::string& extractor, const std::string& id) const;

    // Videos: the same idea in a file of their own (the same YouTube id can be a song AND a video, so they must not share one):
    // <download folder>/history/downloaded_videos.txt ("youtube <video id>", for `yt-dlp --download-archive` -- the PowerShell /
    // shell function down_vid uses it) and downloaded_videos_files.txt ("<id> TAB <file>", Mousiki's own: which file a video was
    // saved as, so a second download of the same video connects the first file instead of fetching it again).
    fs::path video_archive_path() const;
    void record_video_download(const std::string& id, const std::string& file) const;
    // The file a video was downloaded to before (by Mousiki: the file list; by down_vid: "<title> [<id>].mp4" in video_down);
    // empty when there is none or it was deleted since (then it can be fetched again).
    fs::path find_video_download(const std::string& id) const;

private:
    fs::path cache_dir_;
    fs::path download_dir_; // == cache_dir_ unless Settings sets a folder
    static void append_unique_line(const fs::path& file, const std::string& entry);
    static std::string sanitize(const std::string& raw);
    // Old ASCII-only naming rule, used only to locate files cached
    // before sanitize() became UTF-8 aware.
    static std::string legacy_sanitize(const std::string& raw);
};

} // namespace muisc
