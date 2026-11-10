// rl_: the `rl_*` data of this feature, grouped (they were 11 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `rl_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::RlState {
    std::string title, artist, ft;
    std::string remix_text, other_text;
    // TypeSlowed/TypeUltraSlowed/TypeSpedup are mutually exclusive (a
    // radio group -- at most one true); TypeReverb/TypeRemix/TypeOther
    // are independent toggles, any combination.
    bool reverb = false, slowed = false, ultra_slowed = false;
    bool spedup = false, remix = false, other = false;
    RLField focus = RLField::Title;
    std::vector<LyricsVersion> versions;   // what list_lyrics_versions() found for the playing track
    int src_sel = 0;                       // the highlighted row of that list
    int src_scroll = 0;                    // first visible row of the list (it shows 3 at a time)
    bool confirm_delete = false;           // [d] asked "delete these lyrics?", waiting for y / n
    bool from_reader = false;          // opened over READ LYRICS: drawn on it, closing returns to it
    bool from_karaoke = false;         // opened over the karaoke screen: drawn on it, closing returns to it
    std::vector<RLField> rl_visible_fields() const;
    bool* rl_bool_ptr(RLField f);           // nullptr for non-checkbox fields
    std::string* rl_text_ptr(RLField f);
    void rl_fix_scroll();                      // keeps the highlighted row inside the 3 visible ones
};

} // namespace muisc
