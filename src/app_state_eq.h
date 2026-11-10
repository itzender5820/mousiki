// eq_: the `eq_*` data of this feature, grouped (they were 6 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `eq_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::EqState {
    // Shift+E (HKeyEqualizer): the equaliser overlay (Mode::Equalizer), a
    // floating panel over the live playback UI. Ten vertical sliders
    // (31 Hz .. 16 kHz, +/-12 dB), a preset line and an on/off state. Left/
    // Right pick a band, Up/Down change it by 1 dB, ,/. (or TAB) step through
    // the presets (the built-in ones first, then the user's own), E or SPACE
    // switches the EQ on/off, R resets to Flat, ESC / Shift+E close and save.
    // S saves the current curve as a custom preset (a name prompt takes over
    // the row under the sliders until ENTER / ESC), DEL or X deletes the
    // selected custom preset after a second press. Custom presets live in
    // settings_.eq_custom_presets and are written to config.txt right away.
    // Touching a band or choosing a preset also turns the EQ on, so what you
    // change is always what you hear. The values live in settings_.eq_enabled
    // / eq_gains and reach the audio thread through Player::set_equalizer().
    int band = 0;                  // selected band 0..kEqBands-1
    int last_preset = 0;           // unified preset index (built-ins, then custom): where the cycle continues from while the gains are Custom
    bool naming = false;           // the "Save as:" prompt is open
    std::string name_buf;          // its text (edit_caret_/edit_anchor_ belong to it while open)
    std::string status;            // one-line feedback under the sliders, cleared by the next key
    bool delete_armed = false;     // DEL / X pressed once on a custom preset: the next press deletes it
};

} // namespace muisc
