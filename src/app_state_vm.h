// vm_: the `vm_*` data of this feature, grouped (they were 8 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `vm_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::VmState {
    std::vector<VMEntry> all;
    std::vector<int> view;
    std::string query;
    int sel = 0, filter = 0;                       // filter: 0 all videos, 1 coupled with a song, 2 video only
    std::string confirm;                               // the video whose deletion waits for the confirmation
    std::future<std::pair<std::string, std::string>> mp3_fut;   // an mp3 being made from a video file: (mp3 path, error)
    std::string mp3_video;                             // ... and the video it comes from
    mutable std::string pic_key;
    void video_manage_refilter();
};

} // namespace muisc
