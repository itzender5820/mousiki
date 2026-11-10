// sleep_: the `sleep_*` data of this feature, grouped (they were 5 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `sleep_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::SleepState {
    // Shift+Z (HKeySleepTimer): the sleep timer overlay (Mode::SleepTimer), a
    // small floating panel over the live playback UI. Pick 15 / 30 / 60 / 90 /
    // 120 minutes (playback is PAUSED when it runs out, so it can be resumed),
    // "stop after current song", or Off. Up/Down pick a row, ENTER sets it and
    // closes, ESC / Shift+Z close without changing anything. Playback keeps
    // running underneath. Not persisted (a sleep timer must not survive a restart).
    //
    // Interplay with the Stop play mode (settings_.play_mode == 3): "stop after
    // current song" does NOT touch play_mode. It is a one-shot flag consumed in
    // advance_track(), checked BEFORE the repeat / stop / queue logic, so it wins
    // over Repeat and over the queue, and the play mode is exactly what it was
    // afterwards. With Stop mode already on it is simply redundant. The minute
    // timers and the one-shot flag are mutually exclusive (setting one clears the
    // other); a minute timer never changes the play mode either, it only pauses.
    int menu_row = 0;
    int timer_minutes = 0;                       // the minute choice that is running (0 = none)
    bool timer_active = false;
    std::chrono::steady_clock::time_point timer_deadline{};
    bool stop_after_track = false;               // one-shot: end playback when the current song ends
    std::string sleep_timer_label() const;              // "" when nothing is armed, else e.g. "SLEEP 24:10"
};

} // namespace muisc
