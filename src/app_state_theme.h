// theme_: the `theme_*` data of this feature, grouped (they were 17 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `theme_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::ThemeState {
    std::string name_initial;     // the NAME the creator opened with ("" for a new theme, the theme in use for [e])
    std::string draft_text;       // the CREATE THEME draft as it was when the creator opened (to see whether anything changed)
    // --- colour themes (Colors tab, last row: THEME | CREATE THEME) ---------
    // ui: 0 = the plain Settings screen | 1 = pick a theme to apply | 2 = the CREATE THEME overlay |
    // 3 = pick a template from inside the creator. All of it lives inside Mode::Settings (text editing inside
    // the creator uses Mode::ColorEdit with ui still set).
    int ui = 0;
    bool peek = false, peek_busy = false;   // SHIFT+p in the theme picker: a still copy of the main screen in the highlighted theme
    std::vector<themes::Theme> list;
    int sel = 0;               // picker cursor
    mutable int page = 20;     // picker: rows on screen, set while drawing (SHIFT+up / down move by a page)
    Settings draft;            // creator: the colours being edited (a copy; settings_ is untouched until saved)
    themes::Theme base;        // creator: the template whose other keys (the radio's) the new theme keeps
    std::string name;          // creator: name of the new theme (25 characters at most)
    int row = -1;              // creator: -1 = the NAME / TEMPLATE row, else the Colors row
    int col = 0;               // creator: 0/1 -- name|button on the top row, FG|BG on a colour row
    // SHIFT+e / SHIFT+i on the Colors tab: ui 4 = export the current colours as a theme file, 5 = import theme files.
    std::string io_name;       // export: the theme's name
    std::string io_dir;        // export / import: the folder
    int io_row = 0;            // export: 0 name, 1 folder | import: 0 folder, 1 file list
    int io_sel = 0;            // import: the hovered file
    std::vector<std::filesystem::path> io_files;
    void theme_io_rescan();
};

} // namespace muisc
