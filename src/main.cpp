// Entry point: handles the command-line modes (the self-test, the remote client calls, the --cmd
// options) and then starts the music player or the radio.
#include <algorithm>
#include <memory>
#include <clocale>
#include <iostream>
#include <string>
#include "app.h"
#include "bg_tasks.h"
#include "process_util.h"
#include <cstdlib>
#include "terminal_font.h"
#include "mode_switch.h"
#include "scope_window.h"
#include "remote_control.h"
#include "remote_server.h"
#include "update_check.h"
#include "media_session.h"
#include "app_internal.h"
#include "selftest.h"
#include "terminal_ui.h"

#if defined(_WIN32)
#include "win_compat.h"
#endif

int main(int argc, char** argv) {
    // `mousiki --selftest-fetch`: the fetch health check (lyrics sources, cover archive, update check, tools, pinned downloads).
    if (argc > 1 && std::string(argv[1]) == "--selftest-fetch") {
#if defined(_WIN32)
        muisc::win_bootstrap_env();
#endif
        return muisc::selftest_fetch_main(argc, argv);
    }
    // `mousiki --cmd next`, `mousiki --status`: remote control of a running Mousiki (src/remote_control.h). No terminal, no audio.
    if (muisc::remote::is_client_call(argc, argv)) {
#if defined(_WIN32)
        muisc::win_bootstrap_env();
#endif
        return muisc::remote::client_main(argc, argv);
    }
    // `mousiki --scope-window`: the oscilloscope window, started by the player / radio itself (src/scope_window.h).
    // It never touches the terminal.
    if (argc > 1 && std::string(argv[1]) == "--scope-window") {
#if defined(_WIN32)
        muisc::win_bootstrap_env();   // HOME, for its position file
#endif
        return muisc::scope_window_main();
    }
    if (argc > 1 && std::string(argv[1]) == "--spectro-window") {   // the spectrogram window, the same way
#if defined(_WIN32)
        muisc::win_bootstrap_env();
#endif
        return muisc::spectro_window_main();
    }
#if defined(_WIN32)
    // Order matters. Both of these have to happen before App's constructor
    // runs: it loads settings, which resolves $HOME, and it may log, which
    // writes UTF-8 through the console.
    //
    //  - win_bootstrap_env() points HOME at %USERPROFILE%, so the seven
    //    getenv("HOME") call sites across settings/cache/snapshot/log all
    //    land in C:\Users\you\ instead of silently falling back to "."
    //    and littering the launch directory with dot-folders.
    //  - win_console_init() switches the console to UTF-8 and turns on VT
    //    escape processing, without which the entire UI renders as literal
    //    escape-sequence text.
    muisc::win_bootstrap_env();
    if (!muisc::win_console_init()) {
        std::cerr << "mousiki: this console does not support ANSI escape sequences.\n"
                     "Run it in Windows Terminal (wt.exe) rather than the legacy\n"
                     "conhost window, or enable \"Use the new console host\" in\n"
                     "the properties of the window you are using.\n";
        return 1;
    }
#endif

    if (!std::setlocale(LC_ALL, "")) {
        std::setlocale(LC_ALL, "C.UTF-8");
    } else {
        const char* cur = std::setlocale(LC_CTYPE, nullptr);
        if (cur && std::string(cur) == "C") {
            std::setlocale(LC_ALL, "C.UTF-8");
        }
    }
    // Every std::stod() call in this codebase -- settings parsing
    // (app.cpp's settings-panel input, settings.cpp's config.txt loader),
    // ffprobe/JSON duration parsing (metadata_probe.cpp, online_source.cpp),
    // and LRC lyric timestamps (lyrics_fetcher.cpp) -- assumes a period
    // decimal separator, because that's what writes it: save_settings()
    // uses a plain ostream `<<`, which is locale-INDEPENDENT and always
    // writes a period, and ffprobe/most CLI tools/APIs emit numbers in a
    // fixed, non-localized format regardless of OS region. std::stod()
    // itself, though, goes through the C library's LC_NUMERIC category --
    // which the setlocale(LC_ALL, "") call above just set to the OS's
    // regional format. On a machine set to German (or French, or any other
    // comma-decimal locale), "0.5" typed into a settings field, or
    // "215.34" read back from ffprobe, gets silently misparsed as "0" /
    // "215" at the first '.', with no exception thrown -- a partial parse
    // still "succeeds" as far as std::stod is concerned. Pinning
    // LC_NUMERIC back to "C" here keeps LC_CTYPE (and whatever else the
    // block above cares about) on the OS locale while making every number
    // this app touches parse the same way on every machine.
    std::setlocale(LC_NUMERIC, "C");

    // The mode switch (SHIFT and +, the '*' character) ends the running mode and starts the other one. The radio is
    // created inside its own scope, so everything it held (audio device, threads, caches) is released before the
    // player comes back; the player is only suspended (see `app` below and src/mode_switch.h).
    int rc = 0;
    // The music player and the audio book player are two App instances (they live while another mode runs, suspended, so
    // coming back is instant); the radio itself is destroyed on every switch. Each of the two has its own Settings object: they
    // differ in their look (colours, theme, elements, visualizations ...: audio book mode in audiobook_config.txt) and are
    // brought in line in everything else (paths, hotkeys, sound ...) whenever the other one starts to run (sync_shared).
    muisc::Settings shared_settings = muisc::load_settings();
    // The terminal is asked for the wanted size / full screen once, here (a mode switch does not repeat it); see terminal_ui.h.
    muisc::terminal_probe_screen();
    if (!shared_settings.use_terminal_settings)
        muisc::terminal_apply_size(shared_settings.terminal_cols, shared_settings.terminal_rows, shared_settings.full_screen);
    // Mousiki's own font (Settings -> GENERAL -> TEXT AND TRANSLITERATION -> Font): for this window only, where the terminal allows it
    muisc::terminal_font::apply(shared_settings.terminal_default_font ? std::string(muisc::terminal_font::kTerminalDefault) : shared_settings.terminal_font);
    bool book_default_theme = false;
    muisc::Settings book_settings = muisc::load_book_settings(shared_settings, &book_default_theme);
    if (book_default_theme) muisc::apply_book_default_theme(book_settings);   // no look of its own yet: Rose Pine
    muisc::Settings* last_run = &shared_settings;   // the one that was edited last (the radio talks through config.txt)
    std::unique_ptr<muisc::App> app, book_app;
    try {
        muisc::update_check::start(muisc::locate_script("check_update.py"));   // a quiet look for a newer release (once a day, in the background)
        int mode = std::max(0, muisc::mode_index(muisc::read_start_mode()));   // 0 music player, 1 radio, 2 audio book player
        for (;;) {
            if (mode != 1) {
                muisc::set_emoji_replacement(false);   // the radio forces "?" for emoji; the player follows its setting
                std::unique_ptr<muisc::App>& a = (mode == 2) ? book_app : app;
                muisc::Settings* mine = (mode == 2) ? &book_settings : &shared_settings;
                muisc::sync_shared(*last_run, *mine);   // what the other mode changed meanwhile (paths, hotkeys ...)
                last_run = mine;
                if (!a) a = std::make_unique<muisc::App>(mine, mode == 2);
                rc = a->run();
            } else {
                char arg0[] = "mousiki";
                char* argv[] = {arg0, nullptr};
                rc = radio_main(1, argv);
                // the radio writes its choices into config.txt; the sound output is one of them (CHOOSE AUDIO DEVICE)
                shared_settings.audio_device = book_settings.audio_device = muisc::config_value("AudioDevice");
            }
            if (rc != kExitSwitchMode) break;
            mode = muisc::next_mode_in(muisc::read_mode_config(), mode, g_mode_switch_dir);   // (the settings screens save before they switch)
            rc = 0;
        }
        // quit from another mode: now the players really end (each saves, so it first takes over what the last one changed)
        if (app && app->suspended()) { muisc::sync_shared(*last_run, shared_settings); app->shutdown(); }
        if (book_app && book_app->suspended()) { muisc::sync_shared(*last_run, book_settings); book_app->shutdown(); }
        // Background tasks of the players (tags, lyrics, waveforms ...) get a moment to finish. One that is still busy after that (a
        // hung ffprobe, a slow network) must not find its App destroyed: that App is then left alive until the process ends.
        for (std::unique_ptr<muisc::App>* a : {&app, &book_app})
            if (*a && !(*a)->wait_background(3000)) (void)a->release();
        muisc::media::shutdown();               // the system media controls (MPRIS / Now Playing), if on
        muisc::remote::net::shutdown();         // the remote app's web server (if on)
        muisc::scope_window_close();            // the scope window (if open) closes with the program
        muisc::spectro_window_close();          // and the spectrogram window
        muisc::terminal_release_alt_screen();   // (only does something when a mode ended for a switch and the other one never started)
        muisc::terminal_font::restore();        // the terminal's own font again (Terminal.app window, iTerm2 profile, console window)
    } catch (const std::exception& e) {
        // Belt-and-braces on top of settings.cpp's own try/catch around
        // config.txt parsing: this catches anything else startup could
        // throw (a bad path, a filesystem error, whatever else), so a
        // hand-edited config -- or any other environment-specific issue --
        // prints a reason and exits cleanly instead of disappearing with
        // no output, which is what a plain uncaught exception does on
        // Windows (it fails fast with no console message at all).
        std::cerr << "mousiki: " << e.what() << "\n";
        rc = 1;
    } catch (...) {
        std::cerr << "mousiki: startup failed for an unrecognized reason.\n";
        rc = 1;
    }

#if defined(_WIN32)
    // App's TerminalIO destructor already left the alternate screen; this
    // puts the code pages and console modes back the way we found them.
    muisc::win_console_restore();
#endif
    // The helper tasks of the cover loading get a moment; one that is still busy (an ffmpeg on a slow drive) must not meet destroyed
    // objects while the program ends: then the process ends here, without running the destructors.
    if (!muisc::bg_wait_all(700)) muisc::kill_all_children();
    if (!muisc::bg_wait_all(800)) {
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(rc);
    }
    return rc;
}
