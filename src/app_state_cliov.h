// cliov_: the script overlay (Mode::CliScripts): the library of the scripts on the left, an editor on the right (see app_cliov.cpp)
// Kept in its own header so that a change here only rebuilds the files that use `cliov_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::CliOvState {
    std::vector<std::string> names;    // the scripts of the cli folder, plus startup and aliases (which may not exist yet)
    std::vector<std::string> texts;    // their text, for the search
    std::vector<int> shown;            // the indices of `names` that match the filter
    int sel = 0;                       // the row of `shown`
    int scroll = 0;
    std::string filter;                // "/" in the library, or "/cs:words"
    bool filtering = false;
    size_t fcaret = 0, fanchor = 0;
    int focus = 0;                     // 0 the library, 1 the editor
    std::string cur;                   // the name of the script in the editor ("" = none)
    std::vector<std::string> lines{""};
    int cy = 0;                        // the line of the caret
    size_t cx = 0, anchor = 0;         // the caret inside it (bytes)
    mutable int top = 0;               // the first line shown
    mutable int rows = 10;             // the rows of the editor (set while drawing; PgUp / PgDn use it)
    bool from_settings = false;   // opened from Settings -> GENERAL: ESC goes back there
    bool dirty = false;
    bool discard_armed = false;        // a second ESC / ENTER discards the changes
    int prompt = 0;                    // 1 new script (a name), 2 rename, 3 delete (y / n)
    std::string name_buf;
    size_t ncaret = 0, nanchor = 0;
    std::vector<std::string> out;      // what the last runs answered
    std::string note;                  // a short message in the library's frame
};

} // namespace muisc
