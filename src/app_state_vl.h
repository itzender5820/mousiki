// vl_: the `vl_*` data of this feature, grouped (they were 10 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `vl_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::VlState {
    std::vector<VLEntry> tracks, videos;
    std::vector<int> tview, vview;
    std::string tq, vq, msg;
    int pane = 0, tsel = 0, vsel = 0, filter = 0;
    fs::path lock_track, lock_video;
    bool warn = false, confirm = false, del_confirm = false;   // (del_confirm: "Delete COMPLETE VIDEO FILE connected to track" waits for its second ENTER)
    Mode return_mode = Mode::Browse;
    mutable int marq[6] = {-1, -1, -1, -1, -1, -1};     // marquees of the VIDEO OPTION panel: song row, video row, locked song, locked video, 'now' line, folder line
    mutable std::chrono::steady_clock::time_point marq_since[6]{};
    // The "Style Option" overlay (next to the Cover Options button): the look and the Cover Contrast / Brightness / Colors of one track
    // (cover_store.h CoverStyle), and the look of its whole album. "As in Settings" = nothing of its own.
    // Style Option preview as a real picture: 0 = none this frame, 1 = it is in gfx_, 2 = in gfx2_ (over the main screen, whose own cover
    // picture keeps gfx_). The picture lies inside its own panel, so the panel must not crop it.
    mutable int pic = 0;                                       // VIDEO OPTION: the preview of the connected video is a picture in gfx_ (1) / gfx2_ (2) inside the panel, 0: none
    void video_link_refresh_flags();
    void video_link_rebuild();
    std::string video_link_label(const fs::path& p) const;
};

} // namespace muisc
