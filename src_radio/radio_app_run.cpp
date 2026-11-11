#include "app_dirs.h"
#include "radio_app.h"
#include "console_log.h"
#include "string_util.h"
#if defined(_WIN32)
#include "win_compat.h"
#endif

RadioApp::RadioApp() {
    cfg = load_radio_settings(&cfg_source);
    // Still the colours of a built-in theme as an earlier version had them: the theme's fixed colours now (same in every terminal).
    if (cfg_source != "built-in defaults")
        for (const auto& [was, now] : themes::renewed_builtins()) {
            if (!themes::file_holds(radio_config_path(), was, true)) continue;
            theme_apply(cfg, now);
            save_radio_settings(cfg, nullptr);
            break;
        }
    set_emoji_replacement(true);   // station names may carry emoji: always drawn as "?" so the frame cannot shift

    ui.model.shuffle = cfg.playback_shuffle;
    // Where the stations / presets / station lists live: ~/.config/mousiki by default, or what the PATHS tab says.
    storage = resolve_storage_paths(cfg);
    set_storage_files(storage.stations_file.string(), storage.lists_file.string(), storage.presets_file.string());
    load_storage();
    ui.refilter();
    ui.refilter_menu();
    ui.refilter_presets();
    ui.refilter_lists_stations();
    ui.refilter_saved_lists();
    ui.refilter_main_lists();

    apply_audio();   // (the settings that live in the audio path are pushed to the engine at the start and after every change)
    // Timeshift buffer (pause / rewind): a ring file in the cache folder, sized by the GENERAL tab's "Timeshift buffer".
    const char* ts_home = std::getenv("HOME");
    if (!ts_home || !*ts_home) ts_home = std::getenv("USERPROFILE");
    timeshift_file = (muisc::app_dirs::cache() / "radio_timeshift.pcm").string();
    apply_timeshift();
    rhist.open(storage.history_dir);
    yt.set_download_dir(storage.download_dir);
    // (MOUSIKI_RADIO_SEED: a fixed seed for the tests, so that "shuffle" picks the same stations every time)
    rng.seed(std::getenv("MOUSIKI_RADIO_SEED") ? static_cast<unsigned>(muisc::atoi_safe(std::getenv("MOUSIKI_RADIO_SEED"))) : std::random_device{}());
}

// The headless modes (--dump ..., --selftest) print one frame, or run for a few seconds, and end the program: one method each (headless_*).
// --dump
int RadioApp::headless_dump(int argc, char** argv) {
    (void)argc; (void)argv;
        apply_test_size(ui.model);
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
}

// --dump-menu
int RadioApp::headless_dump_menu(int argc, char** argv) {
    (void)argc; (void)argv;
        // The PRESETS menu as plain text; an optional second argument is typed into its search box.
        apply_test_size(ui.model);
        // --dump-menu [text] [p|new|rename]: text goes into the station search; "p" puts it into the preset
        // search instead; "new" / "rename" show the name overlay with `text` typed into it.
        ui.model.menu.open = true;
        const std::string how = argc > 3 ? argv[3] : "";
        if (argc > 2 && how == "p") {
            ui.model.menu.psearch = argv[2]; ui.model.menu.target = SearchTarget::Presets; ui.refilter_presets();
        } else if (argc > 2 && (how == "new" || how == "rename")) {
            ui.model.menu.focus = MenuFocus::Presets;
            ui.model.menu.name.open = true;
            ui.model.menu.name.rename = how == "rename";
            ui.model.menu.name.target = ui.model.bank_active;
            ui.model.menu.name.text = argv[2];
        } else if (argc > 2) { ui.model.menu.search = argv[2]; ui.refilter_menu(); }
        ui.model.menu.search_edit.to_end(ui.model.menu.search);
        ui.model.menu.psearch_edit.to_end(ui.model.menu.psearch);
        ui.model.menu.name.edit.to_end(ui.model.menu.name.text);
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
}

// --dump-lists
int RadioApp::headless_dump_lists(int argc, char** argv) {
    (void)argc; (void)argv;
        // The STATION LISTS menu with sample data (never touches stationlists.txt): tab 1, tab 2 ("2") or the
        // unsaved-changes prompt ("prompt").
        apply_test_size(ui.model);
        const std::string how = argc > 2 ? argv[2] : "";
        ListMenuModel& lm = ui.model.lmenu;
        lm.open = true;
        lm.name = "Morning drive";
        lm.name_edit.to_end(lm.name);
        for (size_t i = 0; i < ui.stations.size() && i < 4; i += 2) lm.items.push_back(static_cast<int>(i));
        lm.dirty = how == "prompt";
        lm.confirm_exit = how == "prompt";
        lm.focus = ListFocus::Stations;
        lm.sort_az = true;
        StationList a; a.name = "Morning drive"; a.items = lm.items;
        StationList b; b.name = "Chill"; b.items = {1, 2};
        StationList c; c.name = "Test signals"; c.items = {4, 5, 6};
        ui.model.lists = {a, b, c};
        if (how == "2") lm.tab = 1;
        ui.refilter_lists_stations();
        ui.refilter_saved_lists();
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
}

// --dump-stations
int RadioApp::headless_dump_stations(int argc, char** argv) {
    (void)argc; (void)argv;
        // The big STATIONS overlay: --dump-stations [search [add|edit|delete]]
        apply_test_size(ui.model);
        ui.model.stov.open = true;
        if (argc > 2) { ui.model.search = argv[2]; ui.refilter(); }
        const std::string how = argc > 3 ? argv[3] : "";
        if (how == "add") { ui.model.stov.add_open = true; ui.model.stov.add_url = "https://example.org/stream.mp3"; ui.model.stov.add_edit[0].to_end(ui.model.stov.add_url); }
        if (how == "edit" && !ui.stations.empty()) {
            StationsOverlay& so = ui.model.stov;
            const Station& st0 = ui.stations[0];
            so.edit_open = true; so.edit_idx = 0;
            const std::string v[StationsOverlay::kEditFields] = {st0.name, st0.preset_name, st0.url, st0.genre, st0.country, st0.codec_hint, std::to_string(st0.bitrate_hint)};
            for (int f = 0; f < StationsOverlay::kEditFields; ++f) { so.edit_text[f] = v[f]; so.edit_ed[f].to_end(v[f]); }
        }
        if (how == "delete" && !ui.stations.empty()) ui.model.stov.del_idx = 0;
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
}

// --dump-history
int RadioApp::headless_dump_history(int argc, char** argv) {
    (void)argc; (void)argv;
        // The LISTENING HISTORY: --dump-history [tab 0-2 [yt]]  (reads the real history folder; "yt" shows the overlay with sample results)
        apply_test_size(ui.model);
        HistoryModel& hm = ui.model.hmenu;
        hm.open = true; hm.hist = &rhist; hm.now = static_cast<long long>(std::time(nullptr));
        hm.tab = std::clamp(argc > 2 ? muisc::atoi_safe(argv[2]) : 0, 0, 2);
        if (argc > 3 && std::string(argv[3]) == "yt") {
            hm.yt_open = true; hm.yt_text = "Daft Punk One More Time"; hm.yt_edit.to_end(hm.yt_text); hm.yt_in_field = false;
            hm.yt_dir = storage.download_dir.string(); hm.yt.searched = true;
            const char* titles[4] = {"Daft Punk - One More Time (Official Video)", "One More Time", "Daft Punk - One More Time (Live)", "One More Time (Radio Edit)"};
            for (int i = 0; i < 4; ++i) {
                YtItem it; it.video_id = "id" + std::to_string(i); it.title = titles[i]; it.uploader = "Daft Punk"; it.duration_sec = 320.0 - i * 20;
                it.state = i == 1 ? 2 : i == 2 ? 1 : i == 3 ? 3 : 0; it.note = i == 1 ? "One_More_Time.opus" : "yt-dlp not found";
                hm.yt.items.push_back(it);
            }
            hm.yt_cursor = 1;
        }
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
}

// --dump-settings
int RadioApp::headless_dump_settings(int argc, char** argv) {
    (void)argc; (void)argv;
        // The RADIO SETTINGS screen: --dump-settings [row [col]] puts the cursor there ("edit" as 4th arg opens the cell).
        apply_test_size(ui.model);
        SettingsModel& sm = ui.model.settings;
        sm.open = true;
        const std::string a2 = argc > 2 ? argv[2] : "";
        if (a2 == "onoff" || a2 == "anim" || a2 == "paths" || a2 == "ref" || a2 == "about") {   // --dump-settings onoff|anim|paths|ref|about [row [edit]]
            const bool anim = a2 == "anim", paths = a2 == "paths", ref = a2 == "ref", about = a2 == "about";
            sm.tab = about ? 5 : ref ? 4 : paths ? 3 : anim ? 2 : 1;
            const int maxrow = about ? static_cast<int>(about_app_text().size()) : ref ? kKeyActionCount + 1 : paths ? kPathRowCount : anim ? rs_row_count(2) : kOnOffRowCount;
            sm.row = std::clamp(argc > 3 ? muisc::atoi_safe(argv[3]) : 0, 0, maxrow - 1);
            if (ref && argc > 4 && std::string(argv[4]) == "edit") { sm.editing = true; sm.buffer = "u"; sm.edit.to_end(sm.buffer); }
            if (paths && argc > 4 && std::string(argv[4]) == "edit") { sm.editing = true; sm.buffer = "/home/me/radio files"; sm.edit.to_end(sm.buffer); }
        } else {
            sm.row = std::clamp(argc > 2 ? muisc::atoi_safe(argv[2]) : 0, 0, color_tab_row_count() - 1);
            if (a2 == "pick" || a2 == "create") {   // --dump-settings pick|create: the THEME overlays
                sm.theme_list = themes::load_all();
                sm.theme_sel = 2; sm.theme_draft = cfg; sm.theme_base = sm.theme_list[0]; sm.theme_name = "My Theme";
                sm.theme_ui = a2 == "pick" ? 1 : 2; sm.theme_row = argc > 3 ? muisc::atoi_safe(argv[3]) : -1;
            } else sm.theme_list = themes::load_all();
            sm.col = argc > 3 ? muisc::atoi_safe(argv[3]) : 0;
            if (argc > 4 && std::string(argv[4]) == "edit") { sm.editing = true; sm.buffer = "21"; }
        }
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
}

// --dump-overlay
int RadioApp::headless_dump_overlay(int argc, char** argv) {
    (void)argc; (void)argv;
    // --dump-overlay [1|2]: the main screen with the SHIFT+o / SHIFT+v overlay
        apply_test_size(ui.model);
        ui.model.overlay = argc > 2 ? muisc::atoi_safe(argv[2]) : 1;
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
}

// --dump-cheat
int RadioApp::headless_dump_cheat(int argc, char** argv) {
    (void)argc; (void)argv;
        apply_test_size(ui.model);
        ui.model.cheat_open = true;
        ui.model.cheat_scroll = argc > 2 ? muisc::atoi_safe(argv[2]) : 0;
        auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
}

// --dump-browse
int RadioApp::headless_dump_browse(int argc, char** argv) {
    (void)argc; (void)argv;
        // The RADIO BROWSER menu with made-up results (no network needed): shows the layout.
        apply_test_size(ui.model);
        BrowseModel& bm = ui.model.browse;
        bm.open = true;
        bm.text[0] = "jazz"; bm.text[1] = "smooth, lounge"; bm.text[2] = "DE"; bm.text[5] = "128-320";
        for (int i = 0; i < kBrowseFields; ++i) bm.edit[i].to_end(bm.text[i]);
        bm.focus = argc > 2 && std::string(argv[2]) == "fields" ? BrowseFocus::Tags : BrowseFocus::Results;
        auto v = std::make_shared<std::vector<BrowseStation>>();
        auto add = [&](const char* name, const char* cc, const char* country, const char* tags, const char* codec, int br, bool on) {
            BrowseStation b; b.uuid = "960e57c5-0601-11e8-ae97-52543be04c81"; b.name = name; b.countrycode = cc; b.country = country;
            b.tags = tags; b.codec = codec; b.bitrate = br; b.online = on; b.language = "german,english"; b.state = "Berlin";
            b.url = "http://example.com/stream.pls"; b.url_resolved = std::string("https://stream.example.com/") + codec + "/" + std::to_string(br);
            b.homepage = "https://www.example.com/radio"; b.votes = 1234; b.clickcount = 87; b.lastchecktime = "2026-10-05 18:16:35";
            v->push_back(b);
        };
        add("Smooth Jazz Lounge Berlin", "DE", "Germany", "jazz,smooth jazz,lounge,easy listening", "MP3", 128, true);
        add("Jazz Radio - The Very Long Name Of A Station That Needs A Marquee To Be Read", "DE", "Germany", "jazz,swing", "AAC", 192, true);
        add("Nightflight Lounge", "DE", "Germany", "lounge,chillout", "MP3", 320, false);
        add("Radio Paradise", "US", "United States", "eclectic,rock", "AAC", 128, true);
        bm.results = v; bm.state = BrowseState::Done; bm.server = "de1.api.radio-browser.info";
        bm.cursor = 1;
        ui.model.t_sec = 3.0;
        RadioStatus fake = engine.status();   // pretend the 4th result is tuned in, to show the highlight and the "Tuned in" line
        fake.state = StreamState::Live; fake.listening_sec = 872; fake.tuned_name = "Radio Paradise";
        fake.tuned_url = "https://stream.example.com/AAC/128"; fake.info.artist = "Twisted Sister"; fake.info.title = "I Wanna Rock";
        auto frame = render_radio_frame(ui.model, fake, engine, cfg);
        print_plain(frame);
        return check_geometry(frame, ui.model) ? 0 : 2;
}

// --rb-search
int RadioApp::headless_rb_search(int argc, char** argv) {
    (void)argc; (void)argv;
        // One real search, results as plain text. key=value arguments: name tags country state language bitrate.
        BrowseQuery q;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            const size_t eq = a.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = a.substr(0, eq), v = a.substr(eq + 1);
            if (k == "name") q.name = v; else if (k == "tags" || k == "tag") q.tags = v; else if (k == "country") q.country = v;
            else if (k == "state") q.state = v; else if (k == "language") q.language = v; else if (k == "bitrate") q.bitrate = v;
        }
        std::vector<std::pair<std::string, std::string>> args;
        std::string err;
        if (!build_browse_args(q, args, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        std::cout << "query:";
        for (const auto& kv : args) std::cout << " " << kv.first << "=" << kv.second;
        std::cout << "\n";
        RadioBrowser rb;
        rb.search(q);
        rb.wait();
        const BrowseSnapshot snap = rb.snapshot();
        if (snap.state != BrowseState::Done) { std::cout << "FAILED: " << snap.message << "\n"; return 3; }
        const size_t n = snap.results ? snap.results->size() : 0;
        std::cout << n << " stations from " << snap.server << "\n";
        for (size_t i = 0; i < n && i < 15; ++i) {
            const BrowseStation& b = (*snap.results)[i];
            std::cout << "  " << b.name << " | " << b.countrycode << " | " << b.bitrate << "k " << b.codec << " | " << b.tags << " | " << b.stream_url() << "\n";
        }
        return 0;
}

// --selftest
int RadioApp::headless_selftest(int argc, char** argv) {
    (void)argc; (void)argv;
        const double secs = argc > 2 ? std::atof(argv[2]) : 6.0;
#if !defined(_WIN32)
        setenv("MOUSIKI_RADIO_NULL", "1", 1);
#endif
        std::string err;
        if (!engine.open_device(&err)) std::fprintf(stderr, "device: %s\n", err.c_str());
        int idx = 0;
        for (size_t i = 0; i < ui.stations.size(); ++i) if (ui.stations[i].synthetic()) { idx = static_cast<int>(i); break; }
        engine.tune(ui.stations[static_cast<size_t>(idx)], idx);
        apply_test_size(ui.model);
        const auto t0 = Clock::now();
        auto last = t0;
        std::vector<std::string> frame;
        StreamState seen_live = StreamState::Idle;
        while (std::chrono::duration<double>(Clock::now() - t0).count() < secs) {
            const auto now = Clock::now();
            ui.model.dt = std::chrono::duration<double>(now - last).count();
            ui.model.t_sec = std::chrono::duration<double>(now - t0).count();
            last = now;
            RadioStatus st = engine.status();
            if (st.state == StreamState::Live) seen_live = StreamState::Live;
            frame = render_radio_frame(ui.model, st, engine, cfg);
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        print_plain(frame);
        RadioStatus st = engine.status();
        std::fprintf(stderr, "state=%d buffer=%.2fs listening=%.1fs reconnects=%d reached_live=%s\n",
                     static_cast<int>(st.state), st.buffer_sec, st.listening_sec, st.reconnects,
                     seen_live == StreamState::Live ? "yes" : "no");
        const bool geo = check_geometry(frame, ui.model);
        engine.stop();
        return (geo && seen_live == StreamState::Live) ? 0 : 2;
}

// The headless modes (--dump ..., --selftest): they print one frame, or run for a few seconds, and end the program. -1: `mode` is none of them.
// Runs the headless mode `mode`, if it is one: its exit code; -1 when `mode` is none of them (then the screen is shown).
int RadioApp::headless(int argc, char** argv, const std::string& mode) {
    if (mode == "--dump") return headless_dump(argc, argv);
    if (mode == "--dump-menu") return headless_dump_menu(argc, argv);
    if (mode == "--dump-lists") return headless_dump_lists(argc, argv);
    if (mode == "--dump-stations") return headless_dump_stations(argc, argv);
    if (mode == "--dump-history") return headless_dump_history(argc, argv);
    if (mode == "--dump-settings") return headless_dump_settings(argc, argv);
    if (mode == "--dump-overlay") return headless_dump_overlay(argc, argv);
    if (mode == "--dump-cheat") return headless_dump_cheat(argc, argv);
    if (mode == "--dump-browse") return headless_dump_browse(argc, argv);
    if (mode == "--rb-search") return headless_rb_search(argc, argv);
    if (mode == "--selftest") return headless_selftest(argc, argv);
    return -1;
}

void RadioApp::interactive_setup() {
    {
        std::string err;
        engine.set_output_device(cfg.audio_device);   // the chosen sound output (CHOOSE AUDIO DEVICE; "" = the default)
        engine.open_device(&err); // on failure the UI shows the reason and stays usable
    }
    engine.set_volume(70);
    scope_win_was = scope_window_running();
    last_full_frame = Clock::now();   // the window may have been opened in the player already
    spectro_win_was = spectro_window_running();
    record_serial_seen = engine.status().record_serial;
    sleep_unit = std::getenv("MOUSIKI_RADIO_SLEEP_UNIT") ? std::max(0.1, std::atof(std::getenv("MOUSIKI_RADIO_SLEEP_UNIT"))) : 60.0;
}

// What happens once per frame before the keys are read.
void RadioApp::frame_tick() {
    muisc::remote::tick("radio");
    for (const muisc::remote::Command& rc : muisc::remote::poll()) {
        if (rc.name != "wizard") { remote_apply(rc); continue; }
        if (!ui.model.wizard_open) wizard_open_now();   // SHIFT+w from the remote: open / close the help wizard
        else { ui.model.wizard_open = false; out_clear_next = true; wizard_seen_save(); }
    }
    cli_tick();   // timers and waiting scripts of the command line
    capture_status();   // a new status line is one more message in the console
    // the remote app's web server follows the Remote App switch (cheap when nothing changed)
    muisc::remote::net::configure({cfg.remote_app, cfg.remote_port, cfg.remote_pin, cfg.remote_hosts, cfg.remote_cli});
    {   // ... and its phone screen, when a phone asks for it (at most ~14 a second)
        static std::chrono::steady_clock::time_point phone_built{};
        int pc = 0, pr = 0;
        const auto pnow = std::chrono::steady_clock::now();
        if (muisc::remote::net::screen_wanted(pc, pr) && pnow - phone_built >= std::chrono::milliseconds(70)) {
            phone_built = pnow;
            std::vector<muisc::remote::net::Zone> zones;
            const std::vector<std::string> lines = build_phone_screen(ui.model, engine.status(), cfg, pc, pr, zones);
            muisc::remote::net::set_screen("radio", pc, pr, lines, zones, cfg.list_fg);
        }
    }
    {   // "a newer version is out": once, as a small overlay once the main screen is showing
        muisc::update_check::Notice n;
        if (!update_pending && muisc::update_check::take(n)) {
            ui.model.update_latest = n.latest; ui.model.update_current = n.current;
            ui.model.update_url = muisc::update_check::release_url(n);
            update_pending = true;
        }
        const UiModel& um = ui.model;
        const bool main_screen = um.overlay == 0 && !um.cheat_open && !um.wizard_open && !um.scope_big && !um.spectro_full &&
                                 !um.menu.open && !um.browse.open && !um.settings.open && !um.lmenu.open && !um.hmenu.open && !um.stov.open;
        if (update_pending && main_screen) { update_pending = false; ui.model.overlay = 8; }
    }
    {   // what `mousiki --status` prints
        const RadioStatus rs = engine.status();
        const char* st = rs.state == StreamState::Idle ? "stopped" : rs.state == StreamState::Failed ? "failed"
                       : rs.timeshift_paused ? "paused" : rs.muted ? "muted" : "playing";
        muisc::remote::publish_status({{"mode", "radio"}, {"state", st}, {"station", rs.tuned_name.empty() ? rs.info.station : rs.tuned_name},
                                       {"artist", rs.info.artist}, {"title", rs.info.title}, {"volume", std::to_string(rs.volume)},
                                       {"muted", rs.muted ? "yes" : "no"}, {"behind_live_sec", std::to_string(static_cast<int>(rs.behind_sec))}});
    }
    // search results arrive on a worker thread: copy the newest snapshot into the model, once per frame
    {
        const BrowseSnapshot snap = browser.snapshot();
        BrowseModel& bm = ui.model.browse;
        bm.results = snap.results; bm.state = snap.state; bm.message = snap.message; bm.server = snap.server;
        const unsigned st_now = static_cast<unsigned>(snap.state);
        if (snap.generation != browse_gen_seen) { browse_gen_seen = snap.generation; bm.cursor = 0; }
        if (st_now != browse_state_seen) {
            browse_state_seen = st_now;
            if (snap.state == BrowseState::Done && bm.open && browse_jump_to_results && snap.results && !snap.results->empty())
                browse_focus(BrowseFocus::Results);
            if (snap.state != BrowseState::Searching) browse_jump_to_results = false;
        }
        const int nres = snap.results ? static_cast<int>(snap.results->size()) : 0;
        bm.cursor = std::clamp(bm.cursor, 0, std::max(0, nres - 1));
    }
    if (sleep_minutes > 0) {                                           // sleep timer: fade, then stop
        const double left = std::chrono::duration<double>(sleep_deadline - Clock::now()).count();
        ui.model.sleep_running = sleep_minutes; ui.model.sleep_left = left;
        if (left <= 0.0) {
            engine.stop(); sleep_cancel();
            ui.model.notice = "sleep timer: stream stopped"; notice_until = ui.model.t_sec + 10;
        } else if (cfg.sleep_fade && left < sleep_fade_len) {
            const double t = left / sleep_fade_len;
            engine.set_fade(static_cast<float>(t * t));
        } else engine.set_fade(1.0f);
    }
    apply_timeshift();   // the buffer size changed in the settings (or was put back by discarding them)
    engine.set_scope_raw(cfg.osci().music);   // oscilloscope music mode: the scopes get the unprocessed stream
    set_marquee_enabled(cfg.marquee);   // GENERAL "Marquee scrolling"
    radio_enforce_visuals(cfg);   // GENERAL "Use oscilloscope / spectrogram": a switched-off one never stays
    {   // the spectrogram window (SHIFT+8): the same
        const bool running_sp = spectro_window_running();
        if (running_sp && (scope_win_tick % 6) == 3) {
            SpectroWinConfig c;
            fill_spectro_window_config(cfg, engine.status(), c);
            spectro_window_set_config(c);
        }
        if (spectro_win_was && !running_sp) {
            const std::string e = spectro_window_take_error();
            ui.model.notice = e.empty() ? "spectrogram window closed" : e;
            notice_until = ui.model.t_sec + (e.empty() ? 3 : 10);
        }
        spectro_win_was = running_sp;
    }
    {   // the scope window (SHIFT+9): its settings / title, and a window closed from its own side
        const bool running = scope_window_running();
        if (running && (++scope_win_tick % 6) == 0) {
            ScopeWinConfig c;
            fill_scope_window_config(cfg, engine.status(), c);
            scope_window_set_config(c);
        }
        if (scope_win_was && !running) {
            const std::string e = scope_window_take_error();
            ui.model.notice = e.empty() ? "scope window closed" : e;
            notice_until = ui.model.t_sec + (e.empty() ? 3 : 10);
        }
        scope_win_was = running;
    }
    if (!ui.model.scope_notice.empty() && ui.model.t_sec > ui.model.scope_notice_until) ui.model.scope_notice.clear();
    {   // history store, YouTube overlay and recording notices: once per frame
        const RadioStatus rs = engine.status();
        off_air_now = rs.state == StreamState::Idle || rs.state == StreamState::Failed || rs.timeshift_paused;
        const long long now_unix = static_cast<long long>(std::time(nullptr));
        rhist.tick(rs, ui.model.dt, now_unix);
        HistoryModel& hm = ui.model.hmenu;
        hm.hist = &rhist; hm.now = now_unix;
        ui.model.hmenu.hist = &rhist;
        if (hm.yt_open) {
            hm.yt = yt.snapshot();
            if (yt_jump && !hm.yt.searching) {
                yt_jump = false;
                if (!hm.yt.items.empty()) { hm.yt_in_field = false; set_text_entry(false); }
            }
            hm.yt_cursor = std::clamp(hm.yt_cursor, 0, std::max(0, static_cast<int>(hm.yt.items.size()) - 1));
        }
        if (rs.record_serial != record_serial_seen) {
            record_serial_seen = rs.record_serial;
            ui.model.notice = rs.record_note; notice_until = ui.model.t_sec + 10;
        }
        if (!ui.model.notice.empty() && ui.model.t_sec > notice_until) ui.model.notice.clear();
    }
}

int RadioApp::interactive() {
    interactive_setup();
    TerminalIO term;
    // Terminal graphics for the oscilloscope's image style: ask the terminal once, now that it is in raw mode.
    gfx = gfx_probe(cfg.gfx_protocol);
    ui.model.gfx_proto = gfx;
    next_frame = Clock::now();
    last_input = next_frame;
    reduce_motion = player_reduce_motion();   // ReduceMotionWhenPaused= in the player's config.txt, read once
    t0 = Clock::now();
    last = t0;
    // First start of the radio after an install: greet with the help wizard,
    // the same way the music player does. Closing it writes WizardSeen=true.
    if (wizard::kEnabled && !cfg.wizard_seen) wizard_open_now();
    if (!muisc::ConsoleLog::instance().initialized()) muisc::ConsoleLog::instance().init(muisc::LogVerbosity::Basic);   // (the console overlay shows this log)
    muisc::remote::start("radio");
    while (running) {
        frame_tick();
        int k;
        const auto input_before = last_input;

        int menus_inject = 0;   // the key of the entry chosen in the MENUS index: handled next, as if it had just been typed
        while ((k = (menus_inject ? std::exchange(menus_inject, 0) : term.poll_key())) != 0) {
            last_input = Clock::now();
            const bool arrow = last_key_was_arrow();
            const int nvis = static_cast<int>(ui.model.visible.size());
            KeyCtx kc{k, arrow, nvis, menus_inject};
            if (key_wizard(kc) ||
                key_cheat(kc) ||
                key_cliov(kc) ||
                key_console(kc) ||
                key_adev(kc) ||
                key_settings(kc) ||
                key_history(kc) ||
                key_stations_dialogs(kc) ||
                key_overlays_a(kc) ||
                key_overlays_b(kc) ||
                key_browse(kc) ||
                key_lists_menu(kc) ||
                key_presets_menu(kc) ||
                key_search(kc) ||
                key_stations_overlay(kc))
                continue;
            key_main(kc);
        }

        if (last_input != input_before) false_wakes = 0;   // a real key: waiting for input works
        else if (woke_for_input) ++false_wakes;
        woke_for_input = false;

        const auto now = Clock::now();
        ui.model.dt = std::chrono::duration<double>(now - last).count();
        ui.model.t_sec = std::chrono::duration<double>(now - t0).count();
        last = now;
        // The wizard's welcome disk: a fixed 0.10, the same speed the player
        // spins it at, and turning whether or not a station is playing.
        if (ui.model.wizard_open)
            ui.model.wizard_angle = std::fmod(ui.model.wizard_angle + (2.0 * 3.14159265358979323846 / 48.0) / 0.035 * 0.10 * ui.model.dt,
                                              2.0 * 3.14159265358979323846);

        const int rows = term.rows(), cols = term.cols();
        std::string out;
        if (rows != last_rows || cols != last_cols || out_clear_next) { out += "\x1b[2J"; last_rows = rows; last_cols = cols; out_clear_next = false; }
        if (cols < kUiCols || rows < kUiRows) {
            last_too_small = true;
            const std::string msg = "Terminal too small: radio mode needs " + std::to_string(kUiCols) + "x" + std::to_string(kUiRows)
                                  + " (this one is " + std::to_string(cols) + "x" + std::to_string(rows) + ")";
            out += "\x1b[H\x1b[2J" + msg;
        } else {
            if (last_too_small) { out += "\x1b[2J"; last_too_small = false; }
            out += "\x1b[H";
            ui.model.cols = cols;
            ui.model.rows = rows;
            {   // cell size in pixels (it changes with the font and the zoom), and whether this frame sends a new picture
                gfx_cell_pixels(cfg.cell_pixels, ui.model.cell_w, ui.model.cell_h);
                ++gfx_tick;
                // how many pictures per second the terminal is sent: Kitty up to 60, Sixel 30 (full) / 60 (half, third)
                const int cap = gfx_picture_cap(gfx, cfg.image_scale);
                const unsigned long every = static_cast<unsigned long>(std::max(1, (cfg.osci().frame_rate + cap / 2) / cap));
                ui.model.gfx_due = (gfx_tick % every) == 0;
            }
            terminal_black_background(cfg.black_background);   // Settings -> GENERAL -> MODES (only writes when it changes)
            terminal_default_text(cfg.list_fg);                 // text without a colour of its own: the list text colour
            auto frame = render_radio_frame(ui.model, engine.status(), engine, cfg);
            // Lines are joined with "\r\n"; the LAST line gets no newline, so a frame that fills the
            // whole terminal can never scroll. No per-line "\x1b[K" either: a line that fills the last
            // column leaves the cursor in the pending-wrap state there, and "erase to end of line"
            // would wipe that last character.
            {   // only the lines that changed (at 120 / 165 fps the whole screen every frame would be megabytes per second);
                // the whole frame after a clear, when the layout changed, and every 2 s
                const bool cleared = out.find("\x1b[2J") != std::string::npos;
                const bool heal = now - last_full_frame > std::chrono::seconds(2);
                std::string body;
                for (size_t i = 0; i < frame.size(); ++i) { body += frame[i]; if (i + 1 < frame.size()) body += "\r\n"; }
                still_frames = (body == last_body && !ui.model.gfx.active && !ui.model.gfx2.active && ui.model.spectro_area[0] < 0)
                                   ? std::min(still_frames + 1, 1000) : 0;
                last_body = body;
                std::string d;
                if (diff_frame_lines(body, prev_lines, d) && !cleared && !heal) out += d;
                else { out += body; last_full_frame = now; }
            }
            // Sixel: the text skips the picture's cells, so the picture on screen stays and is only sent when there is a new
            // one -- or when it is not on screen yet / the screen was cleared (then the current one is sent at once).
            const int sp_proto = gfx == GfxProto::Kitty ? 1 : gfx == GfxProto::Sixel ? 2 : 0;
            const bool sp_img = ui.model.spectro_area[0] >= 0 && sp_proto != 0;
            if (!ui.model.spectro_used) spectro().set_visible(false);
            if (out.find("\x1b[2J") != std::string::npos) spectro_gfx.invalidate();
            const bool sixel_on = (gfx == GfxProto::Sixel && ui.model.gfx.active) || (sp_img && sp_proto == 2);
            if (sixel_on && (!gfx_shown || out.find("\x1b[2J") != std::string::npos)) { ui.model.gfx.fresh = true; ui.model.gfx2.fresh = true; }
            gfx_fill_holes(out, sixel_on);
            // the image goes after the text (Sixel replaces the cells; a Kitty image sits under the text and is replaced in place)
            if (ui.model.gfx.active && gfx != GfxProto::None) { out += gfx_emit(gfx, ui.model.gfx); gfx_shown = ui.model.gfx.crop < ui.model.gfx.cols; }
            else if (gfx_shown) { out += gfx_clear(gfx); gfx_shown = false; }
            if (gfx == GfxProto::None && gfx_shown) gfx_shown = false;
            if (ui.model.gfx.active && ui.model.gfx2.active && gfx != GfxProto::None) {
                if (ui.model.gfx2.fresh || !gfx2_shown) { out += gfx_emit(gfx, ui.model.gfx2); gfx2_shown = true; }
            } else if (gfx2_shown) {
                out += gfx_clear(gfx, 1); gfx2_shown = false;
                // Sixel cannot delete a picture: the overlay's pictures stay until their cells are written over, so the next
                // frame is a full one and the main picture is sent again
                if (gfx == GfxProto::Sixel) { prev_lines.clear(); gfx_shown = false; }
            }
            if (sp_img) {
                const int* a = ui.model.spectro_area;
                const int* sk = ui.model.spectro_skip;
                out += spectro_gfx.emit(sp_proto, cfg.spectro, spectro(), a[0], a[1], a[2], a[3], std::max(1, ui.model.cell_w),
                                        std::max(1, ui.model.cell_h), true, sk[0], sk[1], sk[2], sk[3]);
            } else if (spectro_gfx.shown()) out = spectro_gfx.clear(sp_proto) + out;   // before the new screen's text
        }
        write_frame(out);
        // frame pacing: one frame every 1 / frame_rate seconds (the render time counts), never a burst to catch up
        int fps = std::clamp(cfg.osci().frame_rate, 30, 165);
        // Reduce Motion When Paused, as in the player: off air (nothing tuned / paused), no key for 3 s, the wizard closed: 10 frames a
        // second; when 20 of them in a row were the same, 4. A key ends the wait at once (and brings the full rate back).
        const bool quiet = reduce_motion && off_air_now && !ui.model.wizard_open && Clock::now() - last_input > std::chrono::seconds(3);
        if (quiet) fps = std::min(fps, still_frames >= 20 ? 4 : 10);
        next_frame += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
        const auto after = Clock::now();
        if (next_frame < after) next_frame = after;
        if (quiet && false_wakes < 3) {   // (the input signalling although there is no key: sleep, as before)
            while (Clock::now() < next_frame) {
                const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - Clock::now()).count()) + 1;
                if (term.wait_input(std::min(left, 50))) { woke_for_input = true; break; }
                false_wakes = 0;
            }
        } else std::this_thread::sleep_until(next_frame);
    }
    muisc::remote::stop();
    set_text_entry(false);
    if (ui.model.settings.dirty) settings_save();
    engine.stop();
    rhist.tick(engine.status(), 0.0, static_cast<long long>(std::time(nullptr)));   // closes the live line
    rhist.close_live();
    if (gfx_shown) { write_frame(gfx_clear(gfx)); gfx_shown = false; }
    if (gfx2_shown) { write_frame(gfx_clear(gfx, 1)); gfx2_shown = false; }
    if (spectro_gfx.shown()) write_frame(spectro_gfx.clear(gfx == GfxProto::Kitty ? 1 : 2));   // nothing of it may outlive the radio
    if (switch_mode) terminal_hold_alt_screen();   // the player takes the screen over: no flash of the shell in between
    return switch_mode ? kExitSwitchMode : 0;
}
