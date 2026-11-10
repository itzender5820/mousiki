// qtp_: the `qtp_*` data of this feature, grouped (they were 7 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `qtp_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::QtpState {
    int stage = 0;                  // 0 = new / existing, 1 = pick the existing playlist
    int choice = 0;                 // 0 = New playlist, 1 = Existing playlist
    std::string query;
    std::vector<PlaylistSummary> view;
    int selected = 0;
    bool track = false;
    std::string new_name;           // stage 2: the name of a new playlist
};

} // namespace muisc
