// bulk_: the `bulk_*` data of this feature, grouped (they were 9 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `bulk_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::BulkState {
    // --- bulk add (paste a YouTube playlist link while Queue is
    // focused; hovering-song add on 'a' stays the List-panel behavior) --
    // Floating panel (see draw_floating_panel()), fixed total line count
    // across both phases -- unused rows are just blank-padded rather
    // than the panel changing size -- so its on-screen footprint never
    // moves/resizes frame to frame while open. Two phases:
    //   1. Input: add_results_ready == false -- typing the link.
    //   2. Results: fetch succeeded -- a compact starred checklist,
    //      navigable with Up/Down, toggled per-row with Space. "a" adds
    //      every fetched track regardless of star state ("ALL"); Enter
    //      adds only the starred ones ("[SELECT]").
    std::string add_buffer;
    bool add_results_ready = false;
    std::vector<bool> add_selected;   // parallel to pending_bulk_add_.items, default all-starred
    int add_cursor = 0;
    int add_scroll = 0;
    std::thread add_thread;
    std::mutex add_mutex;
    std::atomic<bool> add_in_progress{false};
    std::atomic<bool> add_ready{false};
};

} // namespace muisc
