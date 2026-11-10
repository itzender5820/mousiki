// vfs_: the `vf_*` data of this feature, grouped (they were 22 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `vfs_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::VfState {
    std::unordered_map<std::string, std::string> replace;   // song -> the downloaded video a refetch replaces (deleted when the new one has arrived)
    std::string v_auto;                                  // the song whose refetch chooses the closest hit and downloads by itself
    std::unordered_map<std::string, VFEntry> saved;   // what video_fetch_state.json holds (songs that are not listed at the moment too)
    bool saved_loaded = false;
    std::vector<VFEntry> all;
    std::vector<int> view;
    std::string query, msg;
    int focus = 1, sel = 0, cand_sel = 0, src = 0, filter = 0;   // filter: 0 missing video only, 1 all songs, 2 songs with a video, 3 the songs with a chosen video; focus: 0 filter field, 1 songs, 2 videos; src: 0 file name + tags, 1 file name, 2 tags
    mutable int page_rows = 10;
    mutable int art_col = 0;                         // the screen column where the right pane starts (the preview picture)
    bool warn = false;
    std::string confirm;                               // the song whose re-fetch waits for the confirmation
    std::string hover;                                // the song the cursor rests on (for the automatic search)
    std::chrono::steady_clock::time_point hover_since{};
    std::deque<std::string> queue;                     // songs waiting for their search
    std::string last_queries;                          // the texts the running / last search used (shown when it finds nothing)
    std::string searching;                             // the song whose search runs
    std::future<std::vector<OnlineResult>> search;
    mutable std::string pic_key;
    mutable int marq = -1;                             // marquee of the song under the cursor (restarts when the cursor moves)
    mutable std::chrono::steady_clock::time_point marq_since{};
    int tab = 0;                                       // 0 SONGS AND VIDEOS, 1 VIDEO FILES (focus: 0 filter field, 1 list)
    void video_fetch_save_state();                       // the found videos and the choices go to video_fetch_state.json (kept until downloaded)
    void video_fetch_refilter();
    const VFEntry* video_fetch_hovered() const;
    void video_fetch_enqueue(const std::string& path, bool front);
    int video_fetch_nav(int selected, int total, bool up) const;
};

} // namespace muisc
