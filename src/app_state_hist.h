// hist_: the listening history overlay's own data (tab, cursor, scroll, the Top Tracks view, the smart-grid cursor).
// Kept in its own header so that a change here only rebuilds the files that use `hist_` (see app.h). The store itself
// (`history_`, also used outside the overlay) stays in App.
#pragma once
#include "app.h"

namespace muisc {

struct App::HistState {
    int tab = 0;                     // 0=History, 1=Top Tracks, 2=Top Playlists, 3=Habits
    static constexpr int kTabs = 4;
    static constexpr int kTabTop = 1, kTabPlaylists = 2, kTabHabits = 3;
    int selected = 0;                // cursor within tabs 0/1
    int scroll = 0;                  // manual scroll offset (tab 2's content)
    int view_top = 0;                // first visible list row on tabs 0/1 (window moves only when the cursor leaves it)
    bool most_first = true;          // 'r' on the Top Tracks tab
    std::vector<HistoryTopRow> top_view;   // rebuilt by history_refresh_top()
    std::string status;              // footer status line, set by 'r' / the queue adds
    // Top Tracks tab only: it is split into two stacked panes. 0 = the track
    // list (Up/Down move its cursor), 1 = the "ADD SMART HISTORY TO QUEUE"
    // pane below it: a 4 x 4 grid of lists (column 0 Top 10/25/50/100, then
    // top of the week/month/quarter/year, top by time of day, and the
    // "rediscover" lists). Arrows move in the grid, Enter queues the list,
    // TAB toggles the panes, ESC leaves the grid for the track list.
    int pane = 0;
    int add_sel = 0;                 // row 0..3 in the smart grid
    int add_col = 0;                 // column 0..3 in the smart grid
    static constexpr int kAddCounts[4] = {10, 25, 50, 100};
    static constexpr int kSmartListSize = 25;   // every smart list except column 0
};

} // namespace muisc
