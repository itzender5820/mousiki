#pragma once
// ---------------------------------------------------------------------------
// Local-files-only playlists.
//
// A playlist is deliberately just a list of local file paths, one per
// line, saved as a plain UTF-8 .txt file -- no database, no IDs, nothing
// that can get out of sync with the filesystem. This is on purpose: it
// means a playlist file is trivially readable/editable by hand, survives
// a mousiki version bump with zero migration code, and "the playlist" IS
// the file rather than a cache of one.
//
// Deliberately does NOT support online/YouTube entries -- see the
// App-side playlist editor, which only ever lets you pick from the local
// library.
//
// Every function here is stateless: the playlists folder is passed in on
// every call (see App::playlists_dir()) rather than cached, so it always
// reflects whatever local_music_paths[0] currently is, even if that
// changes at runtime via Settings.
// ---------------------------------------------------------------------------
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// One track inside a saved playlist.
struct PlaylistTrack {
    std::string title;   // filename stem, same convention as LocalTrack::title
    std::string artist;  // parent directory name, same convention as LocalTrack::folder_artist
    fs::path path;
    // True if this file no longer exists on disk at load time. Missing
    // tracks are kept in the list (not silently dropped) so re-editing a
    // playlist doesn't lose track of what was supposed to be there just
    // because a drive is temporarily unmounted -- but callers that queue
    // playback (see App::playlist_add_selected_to_queue()) skip them.
    bool missing = false;
};

struct Playlist {
    std::string name;
    bool imported = false;   // lives in <playlists>/imported_lists (a copy made by the import) instead of the folder itself (created here)
    std::vector<PlaylistTrack> tracks;
};

// Lightweight summary for list views ("browse saved playlists", the main
// UI's "/p:" search) -- avoids resolving every track of every playlist
// just to show a name and a count.
struct PlaylistSummary {
    std::string name;
    size_t track_count = 0;
    bool imported = false;   // in <playlists>/imported_lists (given), else created in the program
};

class PlaylistManager {
public:
    // The playlists folder keeps the playlists created in the program; the copies the import makes go to this subfolder, so the two
    // kinds are easy to tell apart. list() / load() / remove() / rename_paths() look into both; save() writes to the one Playlist::imported says.
    static constexpr const char* kImportedDir = "imported_lists";

    // Rewrites every line of every playlist .txt under `dir` that equals an
    // old path in `renames` (old -> new, both UTF-8) to the new path. Used
    // after the meta editor renamed files, so saved playlists keep pointing
    // at them. Files without a match are left untouched. Returns how many
    // playlists were changed.
    static int rename_paths(const fs::path& dir, const std::vector<std::pair<std::string, std::string>>& renames);

    // Lists saved playlists under `dir` (the playlists folder itself, e.g.
    // from App::playlists_dir() -- NOT its parent), sorted alphabetically,
    // case-insensitively. Empty if the folder doesn't exist yet.
    static std::vector<PlaylistSummary> list(const fs::path& dir);

    // Loads one playlist by name from `dir`. std::nullopt if no such
    // playlist file exists.
    static std::optional<Playlist> load(const fs::path& dir, const std::string& name);

    // Writes (overwrites) the playlist as dir/<sanitized name>.txt,
    // creating `dir` if needed. Returns false and sets *error on failure.
    static bool save(const fs::path& dir, const Playlist& pl, std::string* error = nullptr);

    // Deletes dir/<sanitized name>.txt. Returns false and sets *error if
    // the file doesn't exist or couldn't be removed.
    static bool remove(const fs::path& dir, const std::string& name, std::string* error = nullptr);

    // Filesystem-safe version of a playlist name, for use as a filename
    // stem (strips path separators and other reserved characters).
    static std::string sanitize_name(const std::string& name);
};

} // namespace muisc
