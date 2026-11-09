// mousiki_radio -- radio mode (standalone executable, and the radio mode of the main program).
//
//   mousiki_radio                 interactive
//   mousiki_radio --dump          render one idle frame as plain text and exit
//   mousiki_radio --dump-menu [q] render the PRESETS menu (optionally with search text q) and exit
//   mousiki_radio --dump-settings [row [col [edit]]] render the RADIO SETTINGS screen and exit
//   mousiki_radio --dump-overlay [1|2] render the main screen with the oscilloscope (1) / loudness (2) overlay and exit
//   mousiki_radio --dump-cheat [n] render the CHEATSHEET (scrolled down n lines) and exit
//   mousiki_radio --dump-browse   render the RADIO BROWSER menu with sample results and exit
//   mousiki_radio --dump-lists [1|2|prompt] render the STATION LISTS menu (tab 1 / tab 2 / unsaved-changes prompt) with sample data and exit
//   mousiki_radio --rb-search [name=.. tags=.. country=.. state=.. language=.. bitrate=..]
//                                 one real Radio Browser search from the command line (connectivity test)
//   mousiki_radio --selftest [s]  headless: tune the offline test signal for s seconds
//                                 (default 6) using the null audio device, then print
//                                 the frame and check its geometry. Needs ffmpeg.
//
// The program is the class RadioApp (radio_app.h): radio_app_run.cpp has the loop of frames and keys, the headless modes and the set-up;
// radio_app_core.cpp (tuning, the sleep timer, the windows), radio_app_stations.cpp (the stations overlay, the browser, the station lists)
// and radio_app_settings.cpp (the settings screen, themes, audio device) the rest of the work; radio_app_keys*.cpp one method per overlay /
// menu for the keys. Nothing in radio/ depends on App or Player.
#include "radio_app.h"
#if defined(_WIN32)
#include "win_compat.h"
#endif

int radio_main(int argc, char** argv) {
    {   const muisc::ModeConfig mk = muisc::read_mode_config(); g_mode_key_next = mk.key_next; g_mode_key_prev = mk.key_prev; }
    // (Windows console set-up and the locale are done once by main() / radio_standalone.cpp.)
    const std::string mode = argc > 1 ? argv[1] : "";
    RadioApp app;
    const int rc = app.headless(argc, argv, mode);
    if (rc >= 0) return rc;
    return app.interactive();
}
