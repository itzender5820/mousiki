// The music player's settings: defaults, the config.txt round trip, the shared oscilloscope rows, the hotkey table.
#include "test_framework.h"
#include "settings.h"
#include "themes.h"
#include "app_internal.h"
#include "scripts_env.h"
#include "keyboard_layout.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>

using namespace muisc;

TEST(settings_defaults_per_oscilloscope_style) {
    mt::TempHome home;
    const Settings s = load_settings();
    // braille: the classic beam with rough mode; image: the CRT beam with dwell brightness, rough mode off
    CHECK(!s.osci_set[0].crt);
    CHECK(s.osci_set[0].rough);
    CHECK(s.osci_set[1].crt);
    CHECK(s.osci_set[1].dwell);
    CHECK(!s.osci_set[1].rough);
    CHECK_EQ(s.osci_set[0].trace, 1024);
    CHECK(s.black_background);
    CHECK(!s.share_profiles);
    CHECK(s.spectro_history);   // (the analysis of the spectrogram keeps running while a track plays: on by default)
}

TEST(settings_save_load_is_stable) {
    mt::TempHome home;
    Settings s = load_settings();
    s.osci_set[1].decay = 0.10f;
    s.osci_set[1].trace = 8192;
    s.osci_set[0].rough = false;
    s.osci_style = 1;
    s.osci_mode = 3;
    s.image_detail = 1;
    s.share_profiles = true;
    s.spectro_history = false;
    save_settings(s);
    const Settings back = load_settings();
    CHECK(!back.spectro_history);
    CHECK_NEAR(back.osci_set[1].decay, 0.10, 0.001);
    CHECK_EQ(back.osci_set[1].trace, 8192);
    CHECK(!back.osci_set[0].rough);
    CHECK_EQ(back.osci_style, 1);
    CHECK_EQ(back.osci_mode, 3);
    CHECK_EQ(back.image_detail, 1);
    CHECK(back.share_profiles);
    // a second round trip changes nothing (the first one may normalise, e.g. the lyrics flag; HKey lines come in map order)
    save_settings(back);
    const Settings again = load_settings();
    auto sorted_lines = [](const std::string& text) {
        std::vector<std::string> v; std::istringstream in(text); std::string l;
        while (std::getline(in, l)) v.push_back(l);
        std::sort(v.begin(), v.end());
        return v;
    };
    CHECK(sorted_lines(settings_to_text(again)) == sorted_lines(settings_to_text(back)));
}

TEST(settings_config_keys_are_case_insensitive_and_legacy_frame_rate_sets_both_styles) {
    VizPrefs v;
    CHECK(osci_config_key(v, "OsciDecay", "0.55"));
    CHECK(osci_config_key(v, "oscidecay", "0.60"));
    CHECK_NEAR(v.osci_set[0].decay, 0.60, 0.001);
    CHECK(osci_config_key(v, "FrameRate", "60"));               // up to v3.0.0: one value for both styles
    CHECK_EQ(v.osci_set[0].frame_rate, 60);
    CHECK_EQ(v.osci_set[1].frame_rate, 60);
    CHECK(osci_config_key(v, "OsciImageFrameRate", "120"));     // the per-style key that follows wins
    CHECK_EQ(v.osci_set[0].frame_rate, 60);
    CHECK_EQ(v.osci_set[1].frame_rate, 120);
    CHECK(!osci_config_key(v, "SomethingElse", "1"));
}

TEST(settings_osci_rows_step_and_clamp) {
    VizPrefs v;
    for (int i = 0; i < 200; ++i) osci_adjust(v, kOrDecay, +1);
    CHECK_NEAR(v.osci().decay, 0.99, 0.0001);                   // never above 0.99
    for (int i = 0; i < 200; ++i) osci_adjust(v, kOrDecay, -1);
    CHECK_NEAR(v.osci().decay, 0.0, 0.0001);
    for (int i = 0; i < 40; ++i) osci_adjust(v, kOrTrace, +1);
    CHECK_EQ(v.osci().trace, 8192);                             // 128 .. 1024 in steps, then 2048, 4096, 8192
    for (int i = 0; i < 40; ++i) osci_adjust(v, kOrTrace, -1);
    CHECK_EQ(v.osci().trace, 128);
    osci_adjust(v, kOrStyle, +1);
    CHECK_EQ(v.osci_style, 1);
    osci_reset(v);                                               // the style in use goes back to its defaults
    CHECK(v.osci().crt);
    CHECK(!v.osci().rough);
    CHECK(osci_visible_rows(v).size() > 8);
}

TEST(settings_osci_row_labels_and_values_exist) {
    VizPrefs v;
    for (int r = 0; r < kOsciRowCount; ++r) {
        if (r == kOrDisplay) continue;                           // the caller's row
        CHECK(!osci_row_label(r).empty());
        CHECK(!osci_row_value(v, r).empty());
    }
}

TEST(settings_default_hotkeys_do_not_collide) {
    Settings s;
    apply_default_hotkeys(s);
    std::map<std::string, std::string> owner;
    for (const auto& [action, key] : s.hotkeys) {
        if (key.empty()) continue;
        const auto [it, fresh] = owner.emplace(key, action);
        // The karaoke size keys work only inside the karaoke overlay, the mode keys outside it: each pair shares its keys.
        auto pair = [](const std::string& x, const std::string& y) {
            return (x == "HKeyModeNext" && y == "HKeyKaraokeBigger") || (x == "HKeyKaraokeBigger" && y == "HKeyModeNext") ||
                   (x == "HKeyModePrev" && y == "HKeyKaraokeSmaller") || (x == "HKeyKaraokeSmaller" && y == "HKeyModePrev") ||
                   shares_shift_b(x, y);   // SHIFT+b: the bookmark in the audio book mode, the bars overlay elsewhere
        };
        if (!fresh && pair(it->second, action)) continue;
        if (!fresh) ::mt::fail(__FILE__, __LINE__, "key '" + key + "' is bound to both " + it->second + " and " + action);
    }
    CHECK(owner.size() > 40);
}

TEST(settings_take_shared_keeps_the_grid_colour) {
    VizPrefs a, b;
    a.scope_grid_color = "99";
    b.scope_grid_color = "11";
    b.osci_set[1].decay = 0.33f;
    b.osci_style = 1;
    b.spectro.span = 45;
    a.take_shared_from(b);
    CHECK_EQ(a.scope_grid_color, std::string("99"));            // the colour belongs to the theme
    CHECK_NEAR(a.osci_set[1].decay, 0.33, 0.001);
    CHECK_EQ(a.osci_style, 1);
    CHECK_EQ(a.spectro.span, 45);
}

TEST(settings_update_check_and_media_keys_default_on_and_round_trip) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK(s.update_check);
    CHECK(s.media_keys);
    CHECK(read_update_check());   // no config.txt line yet: on
    CHECK(read_media_keys());
    s.update_check = false;
    s.media_keys = false;
    save_settings(s);
    const Settings back = load_settings();
    CHECK(!back.update_check);
    CHECK(!back.media_keys);
    CHECK(!read_update_check());
    CHECK(!read_media_keys());
}

TEST(settings_rate_cap_defaults_on_and_round_trips) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK(s.rate_cap_96);
    s.rate_cap_96 = false;
    save_settings(s);
    CHECK(!load_settings().rate_cap_96);
}

TEST(settings_terminal_size_defaults_and_round_trip) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK_EQ(s.terminal_cols, 120);
    CHECK_EQ(s.terminal_rows, 32);
    CHECK(!s.full_screen);
    CHECK(!s.use_terminal_settings);
    s.terminal_cols = 0;
    s.terminal_rows = 55;
    s.full_screen = true;
    s.use_terminal_settings = true;
    save_settings(s);
    const Settings back = load_settings();
    CHECK_EQ(back.terminal_cols, 0);
    CHECK_EQ(back.terminal_rows, 55);
    CHECK(back.full_screen);
    CHECK(back.use_terminal_settings);
}

TEST(settings_mode_keys_are_read_from_the_config) {
    mt::TempHome home;
    Settings s = load_settings();
    // the default depends on the keyboard layout of the machine (' ; on US-style layouts, * _ elsewhere): whatever the
    // layout is, the settings and the radio's reader agree, and the pair is one of the two documented ones
    const std::string next = default_size_mode_key(true), prev = default_size_mode_key(false);
    CHECK((next == "*" && prev == "_") || (next == "'" && prev == ";"));
    CHECK_EQ(s.hotkeys["HKeyModeNext"], next);
    CHECK_EQ(s.hotkeys["HKeyModePrev"], prev);
    CHECK_EQ(read_mode_config().key_next, static_cast<int>(static_cast<unsigned char>(next[0])));
    CHECK_EQ(read_mode_config().key_prev, static_cast<int>(static_cast<unsigned char>(prev[0])));
    s.hotkeys["HKeyModeNext"] = "J";
    s.hotkeys["HKeyModePrev"] = "";
    save_settings(s);
    CHECK_EQ(read_mode_config().key_next, static_cast<int>('J'));
    CHECK_EQ(read_mode_config().key_prev, 0);
}

// ---- the three modes: which are on, in which order, where Mousiki starts, the audio book path ----
TEST(settings_modes_default_order_and_all_on) {
    mt::TempHome home;
    const Settings s = load_settings();
    CHECK(s.use_mode[0] && s.use_mode[1] && s.use_mode[2]);
    CHECK_EQ(mode_order_text(s.mode_order), std::string("player,radio,audiobook"));
    CHECK_EQ(s.start_mode, std::string("player"));
    CHECK_EQ(s.next_mode(0, 1), 1);    // player -> radio -> audio book -> player
    CHECK_EQ(s.next_mode(1, 1), 2);
    CHECK_EQ(s.next_mode(2, 1), 0);
    CHECK_EQ(s.next_mode(0, -1), 2);   // SHIFT and - goes back
}

TEST(settings_modes_round_trip_and_one_mode_stays_on) {
    mt::TempHome home;
    Settings s = load_settings();
    s.mode_order[0] = 2; s.mode_order[1] = 0; s.mode_order[2] = 1;   // audio book, player, radio
    s.use_mode[1] = false;                                           // radio left out
    s.audiobook_path = "/some/books";
    s.start_mode = "audiobook";
    save_settings(s);
    const Settings back = load_settings();
    CHECK_EQ(mode_order_text(back.mode_order), std::string("audiobook,player,radio"));
    CHECK(back.use_mode[0] && !back.use_mode[1] && back.use_mode[2]);
    CHECK_EQ(back.audiobook_path, std::string("/some/books"));
    CHECK_EQ(back.start_mode, std::string("audiobook"));
    CHECK_EQ(back.next_mode(2, 1), 0);   // the radio is skipped
    CHECK_EQ(back.next_mode(0, 1), 2);
    // read_mode_config() reads the same file without loading everything (the radio and main() use it)
    const ModeConfig mc = read_mode_config();
    CHECK(!mc.use[1]);
    CHECK_EQ(next_mode_in(mc, 0, -1), 2);

    // everything switched off is repaired to "one mode on", and a start mode that is off moves to the first one that is on
    Settings off = back;
    off.use_mode[0] = off.use_mode[1] = off.use_mode[2] = false;
    off.start_mode = "radio";
    off.mode_normalize();
    CHECK(off.use_mode[0]);
    CHECK_EQ(off.start_mode, std::string("player"));
    CHECK_EQ(off.next_mode(0, 1), 0);    // nowhere else to go
}

TEST(settings_mode_order_is_always_a_permutation) {
    mt::TempHome home;
    Settings s = load_settings();
    s.mode_order[0] = 1; s.mode_order[1] = 1; s.mode_order[2] = 7;   // nonsense
    s.mode_normalize();
    CHECK_EQ(mode_order_text(s.mode_order), std::string("player,radio,audiobook"));
    CHECK_EQ(mode_index("music"), 0);
    CHECK_EQ(mode_index("Audiobook"), 2);
    CHECK_EQ(mode_index("nonsense"), -1);
}

// ---- one look per mode: the audio book mode keeps its colours, elements, visualizations and sound in audiobook_config.txt ----
TEST(book_profile_sync_keeps_the_look_and_takes_the_rest) {
    Settings player;
    Settings book = player;
    book.book_profile = true;
    book.border_color = "33";
    book.element_disk = false;
    book.normalize = false;
    book.play_mode = 3;
    player.header_color = "9";
    player.local_music_paths = {"/music"};
    player.audiobook_path = "/books";
    sync_shared(player, book);
    CHECK_EQ(book.local_music_paths.size(), size_t(1));      // paths follow
    CHECK_EQ(book.audiobook_path, std::string("/books"));
    CHECK_EQ(book.border_color, std::string("33"));            // the look stays
    CHECK(!book.element_disk);
    CHECK(!book.normalize);
    CHECK_EQ(book.play_mode, 3);
    CHECK(book.header_color != std::string("9"));              // a colour of the player does not come along
    CHECK(book.book_profile);
}

TEST(book_profile_is_saved_apart_from_the_players_file) {
    mt::TempHome home;
    Settings player = load_settings();
    player.border_color = "21";
    save_settings(player);
    Settings book = load_book_settings(player);
    CHECK(book.book_profile);
    CHECK_EQ(book.border_color, std::string("21"));            // the first time it looks like the player
    book.border_color = "46";
    book.header_color = "196";
    book.use_osci = false;
    book.eq_enabled = true;
    book.audiobook_path = "/some/books";                       // a shared value
    save_settings(book);
    // the player's file: its own look, the shared value
    const Settings again = load_settings();
    CHECK_EQ(again.border_color, std::string("21"));
    CHECK(again.header_color != std::string("196"));
    CHECK(again.use_osci);
    CHECK(!again.eq_enabled);
    CHECK_EQ(again.audiobook_path, std::string("/some/books"));
    // the book's file: its look (and nothing of the paths)
    const Settings b2 = load_book_settings(again);
    CHECK_EQ(b2.border_color, std::string("46"));
    CHECK_EQ(b2.header_color, std::string("196"));
    CHECK(!b2.use_osci);
    CHECK(b2.eq_enabled);
    std::ifstream in(book_config_path());
    std::stringstream ss;
    ss << in.rdbuf();
    CHECK(ss.str().find("AudioBookPath") == std::string::npos);
    CHECK(ss.str().find("HKey") == std::string::npos);
    CHECK(ss.str().find("ColorBorderTop=46") != std::string::npos);
}

// A built-in theme picked in the audio book mode must still be that theme after the profile was saved and read again
// (the settings screen names the theme by comparing its colours).
TEST(book_profile_keeps_a_picked_theme_exactly) {
    mt::TempHome home;
    Settings player = load_settings();
    save_settings(player);
    const std::vector<themes::Theme> all = themes::load_all();
    for (const themes::Theme& t : all) {
        Settings book = load_book_settings(player);
        theme_apply(book, t);
        save_settings(book);
        Settings back = load_book_settings(load_settings());
        Settings want = book;
        CHECK_EQ(settings_to_text(back).size() > 0, true);
        std::istringstream a(settings_to_text(want)), b(settings_to_text(back));
        std::string la, lb;
        while (std::getline(a, la) && std::getline(b, lb)) {
            if (la.compare(0, 5, "Color") == 0 && la != lb) { std::cerr << t.name << ": " << la << " != " << lb << "\n"; CHECK(false); }
        }
    }
}

TEST(scripts_dir_override_accepts_only_a_checked_absolute_folder) {
    namespace fs = std::filesystem;
    mt::TempHome home;
    const fs::path dir = fs::path(std::getenv("HOME")) / "scripts_override";
    fs::create_directories(dir);
    { std::ofstream(dir / "tool.py") << "print(1)\n"; }
    fs::create_directories(dir / "sub.py");
    auto set_env = [](const std::string& v) {
#if defined(_WIN32)
        _putenv_s("MOUSIKI_SCRIPTS_DIR", v.c_str());
#else
        if (v.empty()) unsetenv("MOUSIKI_SCRIPTS_DIR"); else setenv("MOUSIKI_SCRIPTS_DIR", v.c_str(), 1);
#endif
    };
    set_env(dir.string());
    CHECK(!scripts_file_from_env("tool.py").empty());                     // a regular file in an absolute folder
    CHECK(scripts_file_from_env("missing.py").empty());                   // not there
    CHECK(scripts_file_from_env("sub.py").empty());                       // a folder is not a script
    CHECK(scripts_file_from_env("../tool.py").empty());                   // the name is a plain file name
    CHECK(scripts_file_from_env("a/tool.py").empty());
    set_env("scripts_override");
    CHECK(scripts_file_from_env("tool.py").empty());                      // relative: refused
    set_env((dir / ".." / "scripts_override").string());
    CHECK(scripts_file_from_env("tool.py").empty());                      // ".." parts: refused
    set_env("");
    CHECK(scripts_file_from_env("tool.py").empty());                      // not set
}

TEST(settings_cover_brightness_colors_and_volume_default) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK_EQ(s.cover_brightness, 50);   // the defaults
    CHECK_EQ(s.cover_colors, 0);
    CHECK_EQ(s.volume_indicator, 4);    // the "disk" look
    s.cover_brightness = 35;
    s.cover_colors = 80;
    save_settings(s);
    Settings r = load_settings();
    CHECK_EQ(r.cover_brightness, 35);
    CHECK_EQ(r.cover_colors, 80);
    CHECK(set_config_value("CoverColors", "47%"));   // rounded to steps of 5, clamped
    CHECK_EQ(load_settings().cover_colors, 45);
    CHECK(set_config_value("CoverBrightness", "250"));
    CHECK_EQ(load_settings().cover_brightness, 100);
    s.cover_contrast = 135;   // a contrast beyond 100 % is written and read back
    save_settings(s);
    CHECK_EQ(load_settings().cover_contrast, 135);
}

TEST(settings_cover_look_keys_are_ctrl_t_and_ctrl_z) {
    Settings s;
    apply_default_hotkeys(s);
    CHECK_EQ(s.hotkeys["HKeyCoverPrev"], std::string("CTRL+T"));
    CHECK_EQ(s.hotkeys["HKeyCoverNext"], std::string("CTRL+Z"));
    CHECK_EQ(combo_key_name(kKeyCtrlT), std::string("CTRL+T"));
    CHECK_EQ(combo_key_name(kKeyCtrlZ), std::string("CTRL+Z"));
}

TEST(config_files_are_written_whole_or_not_at_all) {
    mt::TempHome home;
    const fs::path p = fs::path(std::getenv("HOME")) / "atomic_test.txt";
    CHECK(write_text_atomic(p, "A=1\n"));
    CHECK(write_text_atomic(p, "A=2\nB=3\n"));   // over an existing file
    std::ifstream in(p);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_EQ(all, std::string("A=2\nB=3\n"));
    fs::path tmp = p;
    tmp += ".tmp";
    CHECK(!fs::exists(tmp));                        // no temp file is left behind
}

TEST(settings_old_fetch_tags_binding_becomes_the_bookmark_and_the_bars_get_shift_b) {
    mt::TempHome home;
    {
        std::filesystem::create_directories(config_path().parent_path());
        std::ofstream f(config_path());
        f << "HKeyFetchTags=9\nHKeyBarsMenu=Y\n";   // an older config: the user had moved the tag fetch; the bars overlay was on Y
    }
    const Settings s = load_settings();
    CHECK_EQ(s.hotkeys.at("HKeyFetchTags"), std::string("9"));   // stays where the user put it
    CHECK_EQ(s.hotkeys.at("HKeyBookmark"), std::string("9")); // the user's own key is kept
    CHECK_EQ(s.hotkeys.at("HKeyBarsMenu"), std::string("B"));
    mt::TempHome home2;
    const Settings d = load_settings();                          // a fresh config: both commands hold SHIFT+b (one per mode)
    CHECK_EQ(d.hotkeys.at("HKeyBookmark"), std::string("B"));
    CHECK_EQ(d.hotkeys.at("HKeyBarsMenu"), std::string("B"));
}

TEST(settings_video_mode_defaults_off_and_round_trips) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK(!s.video_mode);
    CHECK(!s.video_view);
    CHECK(s.hotkeys["HKeyVideo"] == "&" || s.hotkeys["HKeyVideo"] == "Y");
    s.video_mode = true;
    s.element_lyrics = true;
    s.video_view = true;
    s.viz_default = 4;   // last used: the video view stays
    save_settings(s);
    Settings b = load_settings();
    CHECK(b.video_mode);
    CHECK(b.video_view);
    b.viz_default = 5;   // video
    b.video_view = false;
    save_settings(b);
    Settings c = load_settings();
    CHECK_EQ(c.viz_default, 5);
    CHECK(c.element_lyrics);
    CHECK(c.video_view);
    // without the Video mode the view falls back to the lyrics
    c.video_mode = false;
    save_settings(c);
    const Settings d = load_settings();
    CHECK(!d.video_view);
}

TEST(settings_viz_order_has_five_places_and_novideo_pick) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK_EQ(s.viz_order[4], 4);                 // video is the fifth place by default
    CHECK_EQ(s.novideo_viz, 4);                  // lyrics when there is no video
    CHECK(!s.viz_usable(4));                     // "Use Video" = the Video mode, off by default
    s.video_mode = true;
    CHECK(s.viz_usable(4));
    CHECK_EQ(s.viz_step(0, 1, false), 1);        // the video view is never a "default" step target
    const int o[5] = {4, 3, 2, 1, 0};
    for (int i = 0; i < 5; ++i) s.viz_order[i] = o[i];
    s.novideo_viz = 2;
    save_settings(s);
    const Settings b = load_settings();
    for (int i = 0; i < 5; ++i) CHECK_EQ(b.viz_order[i], o[i]);
    CHECK_EQ(b.novideo_viz, 2);
}

TEST(settings_old_viz_order_gets_video_after_lyrics) {
    mt::TempHome home;
    Settings s = load_settings();
    save_settings(s);
    // rewrite the order the way config files of the earlier versions have it (four names)
    const auto path = config_path();
    std::string text;
    { std::ifstream in(path); std::stringstream ss; ss << in.rdbuf(); text = ss.str(); }
    const size_t p = text.find("VizOrder=");
    CHECK(p != std::string::npos);
    const size_t e = text.find('\n', p);
    text.replace(p, e - p, "VizOrder=lyrics,sphere,spectro,osci");
    { std::ofstream out(path); out << text; }
    const Settings b = load_settings();
    const int want[5] = {3, 4, 0, 2, 1};
    for (int i = 0; i < 5; ++i) CHECK_EQ(b.viz_order[i], want[i]);
}

TEST(settings_video_overlay_options_round_trip) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK(s.video_lyrics);
    CHECK_EQ(s.video_cover, 0);
    CHECK_EQ(s.video_down_height, 720);
    CHECK(s.video_auto);   // (on by default: a track with a video shows it)
    CHECK_EQ(s.video_clap, 2);   // (Show Clap Loading: none by default)
    CHECK_EQ(s.video_size, 2);   // (Video Size: full by default)
    CHECK(s.video_round);        // (Rounded Corners: on by default)
    CHECK_EQ(s.video_chroma, 1); // (Video Chromatic Aberration: subtle by default)
    CHECK_EQ(s.video_dither, 5); // (Video Dither Pattern: interleaved noise by default)
    CHECK_EQ(s.video_grad_sat, 85); // (Video Gradient Saturation: 85 % by default)
    CHECK(!s.disk_border);       // (the ":  :" column: off by default)
    s.video_clap = 1;
    s.video_auto = false;
    s.video_lyrics = false;
    s.video_cover = 2;
    s.video_down_height = 1080;
    save_settings(s);
    const Settings b = load_settings();
    CHECK(!b.video_auto);
    CHECK_EQ(b.video_clap, 1);
    CHECK(!b.video_lyrics);
    CHECK_EQ(b.video_cover, 2);
    CHECK_EQ(b.video_down_height, 1080);
}

TEST(settings_v_is_the_full_screen_video_and_an_old_config_is_migrated) {
    {   // a fresh start: v opens the full screen video, normalization has no key of its own
        mt::TempHome home;
        const Settings s = load_settings();
        CHECK_EQ(s.hotkeys.at("HKeyVideoFull"), std::string("v"));
        CHECK(s.hotkeys.at("HKeyToggleNormalize").empty());
    }
    {   // an old config.txt (normalization on v, no video key, or the earlier ">" video key): the video gets v, normalization is unbound
        mt::TempHome home;
        Settings s = load_settings();
        s.hotkeys["HKeyToggleNormalize"] = "v";
        s.hotkeys.erase("HKeyVideoFull");
        save_settings(s);
        // (save writes every known hotkey: take the video key out of the file by hand)
        const std::filesystem::path cfg = config_path();
        std::ifstream in(cfg);
        std::stringstream ss; ss << in.rdbuf(); in.close();
        std::string txt = ss.str(), out;
        std::istringstream lines(txt);
        for (std::string l; std::getline(lines, l);) if (l.rfind("HKeyVideoFull", 0) != 0) out += l + "\n";
        std::ofstream(cfg) << out;
        const Settings back = load_settings();
        CHECK_EQ(back.hotkeys.at("HKeyVideoFull"), std::string("v"));
        CHECK(back.hotkeys.at("HKeyToggleNormalize").empty());
    }
}

TEST(settings_video_resolution_buffer_and_effect_are_saved_and_read_back) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK_EQ(s.video_res, 0);
    CHECK_EQ(s.video_buffer, 32);
    CHECK(s.video_mp3);                       // (yt-dlp mp3 from mp4: on by default)
    s.video_res = 2; s.video_buffer = 128; s.video_fx = 7; s.video_mp3 = false;
    save_settings(s);
    const Settings back = load_settings();
    CHECK_EQ(back.video_res, 2);
    CHECK_EQ(back.video_buffer, 128);
    CHECK(!back.video_mp3);
    CHECK_EQ(back.video_fx, 7);
    s.video_contrast = 40; s.video_brightness = 65; s.video_colors = 25; s.video_chroma = 2; s.cover_chroma = 3; s.video_dither = 6; s.video_grad_sat = 45;
    save_settings(s);
    const Settings b2 = load_settings();
    CHECK_EQ(b2.video_dither, 6);   // (golden ratio)
    CHECK_EQ(b2.video_grad_sat, 45);
    CHECK_EQ(dither_from("nonsense"), 0);   // (anything unknown: auto)
    CHECK_EQ(dither_from("line screen"), 5);   // (gone: the default one)
    CHECK_EQ(dither_from("clustered dot"), 5);
    CHECK_EQ(dither_from("bayer 2x2"), 2);
    CHECK_EQ(dither_from("bayer 4x4"), 3);
    CHECK_EQ(dither_from("auto"), 0);
    CHECK_EQ(b2.video_contrast, 40);
    CHECK_EQ(b2.video_brightness, 65);
    CHECK_EQ(b2.video_colors, 25);
    CHECK_EQ(b2.video_chroma, 2);
    CHECK_EQ(b2.cover_chroma, 3);
    CHECK_EQ(video_res_from("high (1280 px)"), 1);
    CHECK_EQ(video_res_from("full (1920 px)"), 2);
    CHECK_EQ(video_res_from("standard (640 px)"), 0);
}

// An upgrade from an older version: the config.txt of 4.x has keys that are gone or renamed, values that no longer exist, a BOM and CRLF line ends.
// None of it may stop the program, reset the other settings or come back after a save.
TEST(settings_an_old_config_with_gone_keys_and_values_loads_quietly) {
    mt::TempHome home;
    fs::create_directories(config_path().parent_path());
    {
        std::ofstream f(config_path(), std::ios::binary);
        f << "\xEF\xBB\xBF" "# written by Mousiki 4.9.0\r\n"
             "Volume=37\r\n"
             "VideoWindow=true\r\n"            // a key that no version of 5.0 knows
             "VideoWindowSize=big\r\n"
             "SomeOldKey=1\r\n"
             "no equals sign in this line\r\n"
             "=\r\n"
             "VideoFx=bloom\r\n"               // an effect that is not offered any more
             "VideoSize=banana\r\n"            // a value that never existed
             "VideoContrast=abc\r\n"
             "VideoBrightness=abc\r\n"
             "VideoMode=true\r\n"
             "UpdateCheck=false\r\n";
    }
    Settings s = load_settings();
    CHECK(s.video_fx == 0);                    // bloom reads as Off
    CHECK(s.video_mode);                       // the lines around the odd ones were still read
    CHECK(!s.update_check);
    const Settings d = Settings();
    CHECK_EQ(s.video_size, d.video_size);      // a value that makes no sense: the default
    CHECK_EQ(s.video_contrast, d.video_contrast);
    CHECK_EQ(s.video_brightness, d.video_brightness);
    save_settings(s);
    std::ifstream in(config_path(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string saved = ss.str();
    CHECK(saved.find("VideoFx=off") != std::string::npos || saved.find("VideoFx=Off") != std::string::npos || saved.find("VideoFx=0") != std::string::npos);
    const Settings again = load_settings();
    CHECK_EQ(again.video_fx, s.video_fx);
    CHECK_EQ(again.video_mode, s.video_mode);
}

TEST(settings_old_dither_effect_reads_as_dither_bw) {
    mt::TempHome home;
    fs::create_directories(config_path().parent_path());
    { std::ofstream f(config_path()); f << "VideoFx=dither\n"; }
    const Settings s = load_settings();
    CHECK_EQ(s.video_fx, 21);                  // Dither B/W
}

// The Video Effect Contrast: reduced by default, saved by name, an unknown name (and the "auto" of the first version) reads as the default, a track's own is separate (test_cover_art).
TEST(settings_video_effect_contrast_round_trips) {
    mt::TempHome home;
    CHECK_EQ(load_settings().video_fxc, 1);                  // no file: reduced
    Settings s = load_settings();
    s.video_fxc = 2;
    save_settings(s);
    std::string text;
    { std::ifstream f(config_path()); std::stringstream ss; ss << f.rdbuf(); text = ss.str(); }
    CHECK(text.find("VideoEffectContrast=low") != std::string::npos);
    CHECK_EQ(load_settings().video_fxc, 2);
    { std::ofstream f(config_path()); f << "VideoEffectContrast=full\n"; }
    CHECK_EQ(load_settings().video_fxc, 0);
    { std::ofstream f(config_path()); f << "VideoEffectContrast=reduced\n"; }
    CHECK_EQ(load_settings().video_fxc, 1);
    { std::ofstream f(config_path()); f << "VideoEffectContrast=gibberish\n"; }
    CHECK_EQ(load_settings().video_fxc, 1);                  // anything unknown: the default
    { std::ofstream f(config_path()); f << "VideoEffectContrast=auto\n"; }
    CHECK_EQ(load_settings().video_fxc, 1);                  // the first version's "auto": the default
}

// "clean" is the default Base Layer: a fresh install has it, and a config.txt from before (scanlines was the default then, and the file kept it as
// if it had been chosen) is read as clean once -- the file is written again with the VideoBaseDefault marker, after which a choice stays.
TEST(settings_base_layer_is_clean_by_default_and_an_old_saved_default_is_read_as_clean_once) {
    mt::TempHome home;
    CHECK_EQ(load_settings().video_base, 4);                 // no file: clean
    fs::create_directories(config_path().parent_path());
    { std::ofstream f(config_path()); f << "VideoFx=crt\nVideoBase=scanlines\n"; }
    const Settings a = load_settings();
    CHECK_EQ(a.video_base, 4);                               // the old saved default: clean
    std::string text;
    { std::ifstream f(config_path()); std::stringstream ss; ss << f.rdbuf(); text = ss.str(); }
    CHECK(text.find("VideoBaseDefault=clean") != std::string::npos);
    CHECK(text.find("VideoBase=clean") != std::string::npos);
    { std::ofstream f(config_path()); f << "VideoBaseDefault=clean\nVideoBase=aperture grille\n"; }   // chosen (or typed) after that: it stays
    CHECK_EQ(load_settings().video_base, 1);
    Settings b = load_settings();
    b.video_base = 0;
    save_settings(b);
    CHECK_EQ(load_settings().video_base, 0);                 // scanlines picked on purpose stays scanlines
}

// The safety net of every change to the settings code: for each `Key=value` line of the default config.txt, a copy of the file with that
// one value changed is read and written again; the changed value has to come back (a flag flipped, a number one up or one down -- one of the
// two is inside every range). It finds a setting that is written but not read, read but not written, or clamped where it should not be.
// Settings that depend on another setting are listed below with the reason.
TEST(settings_every_simple_key_round_trips_with_a_changed_value) {
    mt::TempHome home;
    save_settings(load_settings());
    std::string base;
    { std::ifstream in(config_path(), std::ios::binary); std::ostringstream ss; ss << in.rdbuf(); base = ss.str(); }
    CHECK(base.size() > 1000);
    std::vector<std::string> lines;
    { std::istringstream in(base); std::string l; while (std::getline(in, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); lines.push_back(l); } }
    // keys whose value is set by other settings on purpose (the written value is not the one read):
    //   VideoView: only on while the video mode and the lyrics are on;  SpectroLastStyle0/1: the view in use shows its own style.
    //   VideoFrameRate: two values only, 25 and 60 (settings_video_frame_rate_is_25_or_60 below)
    const std::set<std::string> skip = {"VideoView", "SpectroLastStyle0", "SpectroLastStyle1", "VideoFrameRate"};
    int bools = 0, ints = 0, colours = 0, other = 0;
    std::vector<std::string> problems;
    auto round_trip = [&](size_t idx, const std::string& key, const std::string& value) {
        std::string text;
        for (size_t i = 0; i < lines.size(); ++i) text += (i == idx ? key + "=" + value : lines[i]) + "\n";
        { std::ofstream o(config_path(), std::ios::binary | std::ios::trunc); o << text; }
        const std::string out = settings_to_text(load_settings());
        std::istringstream in(out); std::string l;
        const std::string want = key + "=" + value;
        while (std::getline(in, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); if (l == want) return true; }
        return false;
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (l.empty() || l[0] == '#' || l[0] == '[' || l.rfind("HKey", 0) == 0) continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string key = l.substr(0, eq), value = l.substr(eq + 1);
        if (skip.count(key)) continue;
        if (value == "true" || value == "false") {
            ++bools;
            if (!round_trip(i, key, value == "true" ? "false" : "true")) problems.push_back(key + " (flag: " + value + " -> " + (value == "true" ? "false" : "true") + ")");
            continue;
        }
        char* end = nullptr;
        const long n = std::strtol(value.c_str(), &end, 10);
        if (!value.empty() && end && *end == 0) {
            ++ints;
            // (some numbers take steps or a list of values: 5, 10, 25 up or down, or twice / half)
            bool ok = false;
            for (const long cand : {n + 1, n - 1, n + 5, n - 5, n + 10, n - 10, n + 25, n - 25, n * 2, n / 2, n * 2 + 1}) {
                if (cand == n) continue;
                if (round_trip(i, key, std::to_string(cand))) { ok = true; break; }
            }
            if (!ok) problems.push_back(key + " (number: " + value + ")");
            continue;
        }
        if (key.rfind("Color", 0) == 0 && key != "ColorCodes") {   // a colour: a new value has to come back as the file has it (the colours are written as 0..255 palette numbers or #rrggbb)
            ++colours;
            std::string other_value = value == "#102030" ? "#304050" : "#102030";
            if (!round_trip(i, key, other_value)) problems.push_back(key + " (colour: " + value + " -> " + other_value + ")");
            continue;
        }
        ++other;
    }
    std::printf("  [settings round trip: %d flags, %d numbers, %d colours checked, %d other values not checked]\n", bools, ints, colours, other);
    for (const std::string& p : problems) std::printf("  PROBLEM: %s\n", p.c_str());
    CHECK(problems.empty());
    CHECK(bools > 50);
    CHECK(ints > 50);
}
// The table of the simple settings: the names of older versions are still read, a number stays inside its range, a flag takes yes / true / 1.
TEST(settings_table_reads_old_names_clamps_numbers_and_takes_flag_spellings) {
    mt::TempHome home;
    save_settings(load_settings());   // (makes the folder of config.txt)
    auto read = [&](const std::string& text) {
        { std::ofstream o(config_path(), std::ios::binary | std::ios::trunc); o << "# test\n" << text; }   // (a comment line first)
        return load_settings();
    };
    CHECK_EQ(read("video_down_height=1000\n").video_down_height, 1000);          // the key of an older version
    CHECK_EQ(read("VideoDownHeight=99999\n").video_down_height, 2160);           // above the range
    CHECK_EQ(read("VideoDownHeight=5\n").video_down_height, 144);                // below it
    CHECK_EQ(read("VideoDownHeight=abc\n").video_down_height, Settings().video_down_height);   // not a number: the default stays
    CHECK_EQ(read("VideoLayout=7\n").video_layout, 3);
    CHECK(read("Eliment_vizfield=yes\n").element_vizfield);
    // the five element switches take their name, and the two older spellings (Eliment_* / Element_*); a value that is true / false either way
    {
        struct E { const char* names[3]; bool Settings::*field; };
        const E es[] = {{{"ElimentDisk", "Eliment_disk", "Element_disk"}, &Settings::element_disk},
                        {{"ElimentQueue", "Eliment_queue", "Element_queue"}, &Settings::element_queue},
                        {{"ElimentWaveForm", "Eliment_waveform_progress_bar", "Element_waveform"}, &Settings::element_waveform},
                        {{"ElimentLyrics", "Eliment_lyrics", "Element_lyrics"}, &Settings::element_lyrics},
                        {{"Visualizer", "Eliment_visualizer", "Element_visualizer"}, &Settings::element_visualizer}};
        for (const E& e : es)
            for (const char* n : e.names)
                for (const bool v : {true, false}) {
                    const Settings r = read(std::string(n) + (v ? "=true\n" : "=false\n"));
                    CHECK_EQ(r.*(e.field), v);
                }
        CHECK(!read("Element_disk=0\n").element_disk);
        CHECK(read("Element_disk=yes\n").element_disk);
    }
    CHECK(read("VideoAuto=1\n").video_auto);
    CHECK(!read("VolumeGradient=false\n").volume_gradient);
    CHECK(read("VolumeGradient=true\n").volume_gradient);
    CHECK_EQ(read("hifi_viscosity=50\n").hifi_viscosity, 10);
    // percent values: steps of 5, a trailing %, quotes; a brightness of the video that does not start with a digit is ignored
    CHECK_EQ(read("CoverBrightness=47%\n").cover_brightness, 45);
    CHECK_EQ(read("CoverBrightness=48\n").cover_brightness, 50);
    CHECK_EQ(read("CoverContrast=\"33\"\n").cover_contrast, 35);
    CHECK_EQ(read("CoverContrast=150\n").cover_contrast, 150);   // up to 150 %, the other cover percents stop at 100
    CHECK_EQ(read("CoverContrast=400\n").cover_contrast, 150);
    CHECK_EQ(read("CoverContrast=148%\n").cover_contrast, 150);
    CHECK_EQ(read("CoverColors=250\n").cover_colors, 100);
    CHECK_EQ(Settings().cover_grad_sat, 30);   // the default: a mild gradient on the cover looks
    CHECK_EQ(read("CoverGradientSaturation=0\n").cover_grad_sat, 0);
    CHECK_EQ(read("CoverGradientSaturation=47%\n").cover_grad_sat, 45);
    CHECK_EQ(read("CoverGradientSaturation=250\n").cover_grad_sat, 100);
    CHECK(Settings().bulk_save && !Settings().bulk_video);   // BULK ADD: saved as before, no video
    CHECK(!read("BulkAddSave=false\n").bulk_save);
    CHECK(read("BulkAddVideo=true\n").bulk_video);
    CHECK(read("BulkAddSave=true\nBulkAddVideo=false\n").bulk_save);
    CHECK_EQ(read("VideoContrast=33\n").video_contrast, 35);
    CHECK_EQ(read("video_colors=12\n").video_colors, 10);
    CHECK_EQ(read("VideoBrightness=abc\n").video_brightness, Settings().video_brightness);
    CHECK_EQ(read("VideoBrightness=64\n").video_brightness, 65);
    // the box characters: written between quotes, read with or without them, older names too
    CHECK(read("Vertical=\"|\";\n").box_vertical == "|");
    CHECK(read("upper_left_corner=+\n").box_upper_left == "+");
    CHECK(read("ListSeparator=\" / \"\n").list_separator == " / ");
    CHECK(read("legend_cmd_color=77\n").legend_cmd_color == "77");
    CHECK(read("ColorLegendCommand=88\n").legend_cmd_color == "88");
}

// Video Frame Rate (VideoFrameRate=, batch 278): 25 (the default, as every video was decoded before) or 60 (the video's own rate, up to 60);
// anything else is read as the nearer of the two kinds (60 and more = 60, less = 25).
TEST(settings_video_frame_rate_is_25_or_60) {
    mt::TempHome home;
    CHECK_EQ(load_settings().video_fps, 25);
    save_settings(load_settings());   // (the config folder and file exist from here on)
    auto with = [&](const std::string& v) {
        Settings s = load_settings();
        std::string text = settings_to_text(s);
        const size_t at = text.find("VideoFrameRate=");
        CHECK(at != std::string::npos);
        const size_t nl = text.find('\n', at);
        text = text.substr(0, at) + "VideoFrameRate=" + v + text.substr(nl);
        { std::ofstream o(config_path(), std::ios::binary | std::ios::trunc); o << text; }
        return load_settings().video_fps;
    };
    CHECK_EQ(with("60"), 60);
    CHECK(settings_to_text(load_settings()).find("VideoFrameRate=60\n") != std::string::npos);
    CHECK_EQ(with("25"), 25);
    CHECK_EQ(with("120"), 60);
    CHECK_EQ(with("30"), 25);
    CHECK_EQ(with("nonsense"), 25);
}
