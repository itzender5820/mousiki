// cheat_: the `cheat_*` data of this feature, grouped (they were 6 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `cheat_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::CheatState {
    // SHIFT+s inside the cheat sheet: search the commands (help_search.h). The result list is rebuilt by every frame
    // (it is const), so the key handler only reads which table row the selected result stands for.
    bool searching = false;
    std::string query;
    mutable int sel = 0;
    mutable std::vector<int> result_rows;   // the table row of each result, best first
    mutable int first = 0;                  // the first result shown (the list scrolls to keep the selected one in view)
    mutable int jump_row = -1;              // set by ENTER: the next frame scrolls the table to this row
};

} // namespace muisc
