// wizard_: the `wizard_*` data of this feature, grouped (they were 3 loose members of App)
// Kept in its own header so that a change here only rebuilds the files that use `wizard_` (see app.h).
#pragma once
#include "app.h"

namespace muisc {

struct App::WizardState {
    // SHIFT+w (HKeyWizard): the Mousiki Plus Help Wizard (Mode::Wizard), a full-screen help book. src/wizard.{h,cpp}
    // holds the text, the page / part / contents state and the key handling; this class only draws it (the radio draws
    // the same pages the same way). It opens from the main screen and from every menu / overlay that does not take
    // typed text (wizard_allowed_over_mode()), and closing it returns to the mode it was opened from. The first
    // close also sets Settings' wizard_seen and writes config.txt, so the first-start greeting shows only once.
    wizard::State st;
    Mode return_mode = Mode::Browse;
    double angle = 0.0;       // the first page's disk, spun at 0.10 whatever is playing
};

} // namespace muisc
