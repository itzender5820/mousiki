#pragma once
// Which video belongs to which track, set by hand (the meta editor's [ Video Option ] overlay and Search YT downloads). A link is a
// pair of file paths (UTF-8); the file is video_links.txt next to config.txt, one "track<TAB>video" line each. The pair follows the
// files: rename_paths() is called wherever the app renames a file, and a video that is gone from its place is looked up again by
// its file name (see find_moved()). Thread safe.
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace muisc {

class VideoLinks {
public:
    static VideoLinks& get();
    // "" = no link.
    std::string video_for(const std::string& track) const;
    // every track that points at this video
    std::vector<std::string> tracks_for(const std::string& video) const;
    std::vector<std::pair<std::string, std::string>> all() const;   // every (track, video) pair
    void set(const std::string& track, const std::string& video);   // an empty video removes the link
    bool remove(const std::string& track);
    bool has(const std::string& track) const;                        // a real link (not the "no video" mark)
    // "No video for this track": removing a connection must also stop the automatic pairing by file name (video_source_for()); the mark is
    // a link to "-". video_for(), has(), tracks_for() and all() do not show it.
    void mark_none(const std::string& track);
    bool is_none(const std::string& track) const;
    size_t size() const;
    // Files were renamed / moved: both sides of every link follow. Returns the number of links changed (saved at once when > 0).
    int rename_paths(const std::vector<std::pair<std::string, std::string>>& renames);
    // A folder was renamed / moved: everything below `from` now lives below `to`.
    int rename_folder(const std::string& from, const std::string& to);
    // A link whose video is no longer at its place is mended when one of `existing_videos` (the library's video files) has the same
    // file name. Nothing is ever dropped (a drive that is not plugged in must not lose its links). Returns the number of links changed.
    int mend(const std::vector<std::string>& existing_videos);
    // test hooks: another file, and reload from disk
    void use_file(const std::filesystem::path& p);
    void reload();
    std::filesystem::path file() const;

private:
    VideoLinks();
    void load_locked();
    void save_locked() const;
    struct Impl;
    Impl* d_;
};

}  // namespace muisc
