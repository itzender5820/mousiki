// device_: the `device_*` data of this feature, grouped (they were 10 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `device_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::DeviceState {
    std::thread worker_thread;
    std::mutex request_mutex;
    std::condition_variable request_cv;
    std::atomic<int> gen{0}; // incremented each launch; guards against a stale request read racing a newer post
    // Generation of the device handoff currently in flight: set the moment a
    // request is posted, cleared by the worker once player_.play() has
    // actually swapped the new track in (0 = none in flight). While this is
    // nonzero the main loop must NOT act on player_.finished(): until play()
    // runs, the PREVIOUS pcm is still the installed one, so a track that
    // already ended keeps re-latching finished_ from the audio callback every
    // few milliseconds. clear_finished() in poll_pending_load() erases it
    // once, but the old device immediately set it again -- which made a freshly
    // loaded track flip straight back to "no track loaded" (advance_track() ->
    // Stop mode -> has_track_ = false) a frame later, while its own audio
    // started a few hundred ms after that. Pressing play again "fixed" it only
    // because by then the installed pcm was the *playing* track, which can't
    // latch the flag. This holds the stale flag off for the whole handoff.
    std::atomic<int> play_pending_gen{0};
    bool worker_stop = false;
    bool request_ready = false;
    DevicePlayRequest request;
    // Any other call that opens, moves or closes the audio device (Player::stop(), reopen_device()) runs on the device worker too,
    // and the caller waits until it is done: device_run() posts `fn` and blocks until the worker has run it (under
    // player_mutex_). When the worker is not running (yet / any more) `fn` runs right here. device_stop() = device_run() of
    // player_.stop(), and a track switch that is still waiting for the worker is dropped (it would start the old track again).
    std::deque<std::function<void()>> tasks;   // guarded by request_mutex
    bool worker_running = false;               // guarded by request_mutex
    void stop_device_worker();       // called once, from run()'s shutdown, before joining device_.worker_thread
};

} // namespace muisc
