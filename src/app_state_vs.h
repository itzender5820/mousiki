// vs_: the `vs_*` data of this feature, grouped (they were 9 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `vs_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::VsState {
    // SEARCH YT
    std::string query, query_for, msg;
    std::vector<OnlineResult> results;
    std::future<std::vector<OnlineResult>> future;
    bool busy = false, warn = false, by_tags = false;   // by_tags: the query is "artist title" (CTRL+t), else the file name
    int pane = 0, sel = 0;
    fs::path target;                 // the song the download is for
    bool mp3 = true;                 // SEARCH YT: also make an mp3 of the sound (CTRL+a); on by default when no song is the target
    mutable int pic = 0;                                       // SEARCH YT: the thumbnail is a picture in gfx_ (1) / gfx2_ (2) inside the panel, 0: braille
    mutable std::string pic_key;
    void video_search_start();
};

} // namespace muisc
