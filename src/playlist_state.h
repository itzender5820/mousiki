// playlist_state.h -- the playlist editor's state, kept apart from app.h so that a change here only rebuilds the files that include this header
// and not the whole program. App holds it behind a pointer (App::playlist_).
#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <memory>
#include <thread>
#include <future>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "playlist_manager.h"
#include "local_source.h"

namespace muisc {

namespace fs = std::filesystem;

struct PlaylistState {
    std::vector<PlaylistSummary> view;
    int tab = 0;
    int edit_focus = 0; // 0=name field, 1=library picker, 2=playlist-tracks list -- cycled with Tab
    std::string edit_name;
    bool edit_imported = false;   // the playlist being edited is a given (imported) one: it is saved back into imported_lists
    int manage_filter = 0;        // SAVED PLAYLISTS: 0 all, 1 created here (c), 2 given / imported (g)
    std::string edit_orig_name;   // the saved name of the playlist being edited ("" = a new one): a changed name renames it instead of copying it
    std::vector<PlaylistTrack> edit_tracks;
    std::string edit_lib_query;
    std::vector<LocalTrack> edit_lib_view;   // filter_and_rank_local(edit_lib_query)
    int edit_lib_selected = 0;
    int edit_sort = 0;   // LIBRARY sort (Shift+T): 0 folder order, 1 file names, 2 titles, 3 artists, 4 albums A-Z, 5 recently added (newest first)
    int edit_track_selected = 0;
    bool edit_dirty = false;    // true once tab 0 has unsaved changes -- see ESC's confirm prompt below
    bool confirm_exit = false;  // "save before exiting?" Y/N prompt, shown in place of the hint line
    std::vector<PlaylistSummary> manage_view; // tab 1's list
    int manage_selected = 0;
    std::string manage_query; // filter behind manage_view
    int manage_focus = 0;     // 0=search box, 1=list -- cycled with Tab
    bool confirm_delete = false; // "really delete this playlist?" Y/N prompt (tab 1, DEL key)
    bool export_open = false;
    std::string export_name;     // the playlist being exported
    std::string export_dir;      // the folder field
    bool export_m3u = false;     // false = .m3u8 (UTF-8, the default), true = .m3u
    int export_field = 0;        // 0 = folder field, 1 = format row, 2 = scope row
    bool export_all = false;     // scope: false = the hovered playlist, true = the whole folder (every playlist the list shows)
    std::string export_status;   // shown inside the overlay
    bool import_open = false;
    std::string import_dir;                 // the folder field
    int import_field = 0;                   // 0 = folder field, 1 = file list, 2 = scope row
    bool import_all = false;                // scope: false = the marked (else the hovered) files, true = the whole folder
    std::vector<std::string> import_files;  // .m3u / .m3u8 files found in the folder (full paths)
    std::vector<int> import_counts;         // entries per file
    std::vector<char> import_marked;
    int import_selected = 0;
    std::string import_status;              // shown inside the overlay
    std::string status; // shown at the bottom of the overlay; cleared on (re)entry
    std::unordered_set<std::string> folder_open;
    int edit_row_sel = 0;   // cursor row in the folder view
    std::vector<std::vector<fs::path>> add_undo;   // one entry per add (a file or a whole folder): SHIFT+u takes the last one back
};

} // namespace muisc
