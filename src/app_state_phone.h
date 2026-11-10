// State of one feature of the App class
// Kept in its own header so that a change here only rebuilds the files that use `phone_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::PhoneState {
    double angle = 0.0;       // the phone's disk: its own turn at speed 0.10
    std::chrono::steady_clock::time_point last{}, built{};
    int view = 0;             // 0 the phone's main screen, 1 its LISTS screen
    int focus = 0;            // LISTS: the pane UP / DOWN page through (0 the local list, 1 the saved playlists)
    int page = 0;             // LISTS: the page of the local list ...
    int page2 = 0;            // ... and of the playlists
    mutable std::string cover_key;   // music player: the key of the cover picture the phone shows
    mutable PhonePic pic;     // audio book: where the phone draws the picture (cells) and which one
    std::set<std::string> book_open;
    std::string note;         // its status line ("queued 12 titles from ...")
    std::chrono::steady_clock::time_point clear_armed{};   // CLEAR QUEUE tapped once: a second tap within 5 s clears
};

} // namespace muisc
