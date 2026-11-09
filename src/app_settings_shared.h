// The few helpers of the settings code that app_settings.cpp and app_settings_screen.cpp share (the second file was split off the
// first so that they compile in parallel).
#pragma once
#include "app_internal.h"
#include <filesystem>
#include <string>

namespace muisc {

std::string* color_cell(Settings& s, int row, int col);
int color_row_of_tab(int t);
int tab_row_of_color(int c);
constexpr int kColorsLines = 25;   // the Colours tab is 25 lines long
int colors_line_of(int c);
int colors_sel_line(int settings_row);
int colors_first_line(int settings_row, int visible);
bool theme_matches(const Settings& s, const themes::Theme& t);
std::filesystem::path theme_dir_path(std::string dir);

} // namespace muisc
