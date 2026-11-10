// kanji_: the `kanji_*` data of this feature, grouped (they were 5 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `kanji_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::KanjiState {
    std::shared_ptr<KanjiJob> job;
    std::chrono::steady_clock::time_point next{};
    bool broken = false;
    int fails = 0;
    fs::path script;
};

} // namespace muisc
