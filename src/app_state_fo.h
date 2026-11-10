// fo_: the `fo_*` data of this feature, grouped (they were 12 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `fo_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::FoState {
    bool search = false;                 // the search field has the keyboard
    std::string query;
    int sel = 0, scroll = 0;
    int rows = kListVisibleRows;         // list rows the overlay fits; recomputed every frame
    std::set<std::string> expanded;
    std::set<std::string> marked;          // audio book mode: files (full paths) that carry a bookmark
    std::vector<FoRow> view;             // the visible rows (tree walk + search filter)
    std::map<std::string, std::vector<std::string>> subdirs;   // folder -> sorted subfolders
    std::map<std::string, std::vector<std::string>> files;     // folder -> sorted files
    std::map<std::string, int> count;                          // folder -> tracks at or below it
    std::vector<std::string> roots;
    int media = -1;       // SHIFT+v in the big folder overlay
    void folder_overlay_rebuild_view();
};

} // namespace muisc
