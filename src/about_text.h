#pragma once
// The text of the ABOUT APP tab of the player and the radio.
// It is deliberately hard coded, not read from config.txt: a copy in the config file would be
// rewritten and padded with blank lines on every save.
#include <string>
#include <vector>
#include "version.h"

namespace muisc {

inline const std::vector<std::string>& about_app_text() {
    // "v4.2.0" padded to the width of the column, so "Licence" keeps its place in any version.
    static const std::vector<std::string> lines = [] {
        std::string ver = std::string("v") + MOUSIKI_VERSION;
        if (ver.size() < 26) ver.append(26 - ver.size(), ' ');
        else ver += ' ';
        return std::vector<std::string>{
            "Mousiki",
            "",
            "A light weight terminal UI music, radio and audio book player for lovers of the terminal <3",
            "",
            "Mousiki Plus ",
            "Windows Port / Modification : Steffen Schwerdtfeger     Github   : StSchwerdtfeger",
            "Email                       : fanti.blub@gmail.com",
            "Version                     : " + ver + "Licence  : Apache licence 2.0",
            "",
            "Original v1.0 ",
            "Developer : ender                         Github   : itzender5820",
            "Email     : itz.ender5820@gmail.com",
            "Version   : original and final v1.0       Licence  : Apache licence 2.0",
        };
    }();
    return lines;
}

} // namespace muisc
