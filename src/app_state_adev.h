// adev_: the `adev_*` data of this feature, grouped (they were 5 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `adev_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::AdevState {
    bool from_settings = false;
    int sel = 0;                          // 0 = System Default, 1.. = list
    std::vector<AudioOutputDevice> list;  // read when the overlay opens ([r] reads it again)
    std::string err;
    std::string applied;
};

} // namespace muisc
