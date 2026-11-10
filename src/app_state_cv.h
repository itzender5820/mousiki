// cv_: the `cv_*` data of this feature, grouped (they were 25 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `cv_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::CvState {
    bool search = false;
    std::string query;
    int sel = 0, scroll = 0;
    int rows = kListVisibleRows;          // file rows the overlay fits; recomputed every frame
    int type = 1;                         // pre-filter: 0 all, 1 lossless, 2 FLAC, 3 WAV, 4 M4A, 5 OGG, 6 OPUS, 7 AAC, 8 MP3
    int minkbps = 0;                      // pre-filter: index into the minimum-bitrate steps (0 = any)
    bool hires = false;                   // pre-filter: only more than 16 bit or 48 kHz
    int preset = 0;                       // target (convert_preset())
    std::vector<CvFile> files;            // every library file outside the export folder
    std::vector<int> view;                // indices into files: what the filters + the search leave
    std::set<std::string> marked;
    // Search bar modes: 0 = files ("/"), 1 = folders ("/f:"), 2 = playlists ("/p:"). In the last two the list shows the
    // folders / playlists matching the query; ENTER on one narrows the file list to its files (the "scope").
    int pick = 0;
    std::vector<FolderSummary> folders;
    std::vector<PlaylistSummary> playlists;
    bool scope = false;
    std::string scope_label;             // "folder Rock" / "playlist Road trip"
    std::set<std::string> scope_paths;   // the files of the scope (UTF-8 paths)
    mutable std::mutex mu;
    std::map<std::string, AudioInfo> info;   // probe results (guarded by mu)
    std::thread probe_th;
    std::atomic<bool> probe_stop{false};
    std::atomic<int> probe_done{0};       // probes finished since the overlay opened
    int probe_seen = -1;                  // what the last view rebuild saw
    int probe_total = 0;                  // files the probe thread was asked for
    int last_done = -1;                   // conversion progress last reported on the status line
    AudioInfo cv_info_of(const std::string& path) const;
    void convert_overlay_probe_restart();
    int convert_overlay_total() const;        // rows of whatever the list shows right now (files, folders or playlists)
};

} // namespace muisc
