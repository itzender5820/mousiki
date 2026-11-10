// The small data types that the App declares for itself (a row of the search index, the state of a video download, a pending load ...), kept
// out of app.h so that it holds the class only. They are the members of namespace app_types; the App has `using` names for them
// (App::PendingLoad ...), so nothing that writes App::X had to change. Only app.h includes this file.
#pragma once

namespace muisc {
namespace app_types {
namespace fs = std::filesystem;

struct SearchIdx {
    std::string key;
    std::string title_n, folder_artist_n;
    std::string tag_title_n, artist_n, album_n;
    bool tags_seen = false;
};

struct VideoDownload {
    bool active = false;
    std::thread thread;
    std::mutex m;
    std::mutex child_m;
    ChildProcess* child = nullptr;
    std::string progress, file, error, title;
    fs::path track, dir;
    std::string base, video_id;
    bool done = false;
    bool want_mp3 = false;               // the sound of the video is saved as an mp3 song in the regular download folder
    fs::path mp3_dir;
    std::string mp3_file, mp3_error;
};

struct VFEntry {
    fs::path path;
    std::string label, folder, hay;
    std::string tags;                // title / artist / album of the song as far as its tags give them (shown under "Song:"; read when the cursor rests on it)
    double seconds = -1.0;
    bool has_video = false;          // (only the playing song is listed with a video: it can be changed)
    bool refetch = false;            // the re-fetch of a song with a video was confirmed (then it is searched like the others)
    bool searched = false, searching = false, queued = false;
    std::vector<OnlineResult> cands;
    int choice = -1;                 // the active choice (index into cands), -1 = none
};

struct VideoFetchRun {
    std::thread thread;
    mutable std::mutex m, child_m;
    ChildProcess* child = nullptr;
    std::atomic<bool> cancel{false}, linked{false};
    bool running = false, finished = false;
    int total = 0, done = 0, ok = 0, failed = 0;
    std::string status, last_error;
    std::vector<std::string> done_paths;               // songs that have their video now
};

struct VMEntry {
    fs::path path;
    std::string label, folder, hay;
    std::uintmax_t bytes = 0;
    std::vector<std::string> songs;      // the songs that show this video (a connection by hand, or the same file name beside it)
    bool coupled = false;
    std::int64_t mtime = 0;              // last change of the file (seconds): newly added / downloaded files have the latest
};

struct FoRow {
    std::string path;    // folder or file path (UTF-8)
    std::string label;
    bool is_dir = false;
    bool expanded = false;
    int depth = 0;
    int count = 0;       // tracks below a folder
};

struct CvFile {
    std::string path;    // UTF-8
    std::string name;    // file name with extension
    std::string ext;     // lower case, with the dot
    long long size = 0;
};

struct BulkAddResult {
    bool success = false;
    std::string error;
    std::vector<OnlineResult> items;
};

struct PendingLoad {
    bool success = false;
    std::string title, artist, location_label, error;
    fs::path path;
    std::shared_ptr<StreamingPcm> pcm;
    size_t total_sec = 0;
    TrackMetadata metadata;
    bool is_local = true;   // for snapshot/resume identity -- which of path/video_id is authoritative
    std::string video_id;   // valid if !is_local
    bool fresh = false;     // online: the file was made by this load (it was not in the download folder / cache before)
};

struct DevicePlayRequest {
    std::shared_ptr<StreamingPcm> pcm;
    int volume = 70;
    double start_sec = 0.0;
    int generation = 0;
};

struct PathRow {
    enum class Kind { Path, AddPath, Header, Note };
    Kind kind = Kind::Path;
    int sel = -1;              // selectable index, -1 for headers (unselectable)
    int path_index = -1;       // Path: index inside the owning vector
    bool playlist_path = false; // Path/AddPath: true = playlist paths, false = local music paths
    // Path: the single DOWNLOAD PATH field. It reads/writes
    // settings_.download_folder instead of either vector, hence the
    // path_index = -1 that never reaches them -- every consumer
    // switches on this flag first.
    bool download_folder = false;
    bool history_folder = false;   // Path: the single HISTORY PATH field (settings_.history_path)
    bool export_folder = false;    // Path: the single PLAYLIST EXPORT PATH field (settings_.playlist_export_path)
    bool import_folder = false;    // Path: the single PLAYLIST IMPORT PATH field (settings_.playlist_import_path)
    bool conversion_folder = false; // Path: the single CONVERSION EXPORT PATH field (settings_.conversion_export_path)
    bool audiobook_folder = false;  // Path: the single AUDIO BOOK PATH field (settings_.audiobook_path)
    const char* label = "";    // Header: section title; AddPath: "+ new path"
};

} // namespace app_types
} // namespace muisc
