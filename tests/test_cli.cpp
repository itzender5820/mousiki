// The command line (CLI): the pure parts (cli_core.h), the settings bridge and the text commands of `mousiki --cmd / --query`.
#include "test_framework.h"
#include "cli_core.h"
#include "remote_control.h"
#include "settings.h"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>

using namespace muisc;

namespace {
int client(std::initializer_list<const char*> args) {
    std::vector<std::string> store{"mousiki"};
    for (const char* a : args) store.push_back(a);
    std::vector<char*> argv;
    for (auto& s : store) argv.push_back(s.data());
    return remote::client_main(static_cast<int>(argv.size()), argv.data());
}
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(120)); }
}

TEST(cli_chain_splits_at_semicolons_outside_quotes) {
    const auto v = cli::split_chain("volume 30; next ;; play \"a;b\" ");
    CHECK_EQ(v.size(), static_cast<size_t>(3));
    if (v.size() == 3) { CHECK_EQ(v[0], std::string("volume 30")); CHECK_EQ(v[1], std::string("next")); CHECK_EQ(v[2], std::string("play \"a;b\"")); }
    CHECK(cli::split_chain("  ; ;").empty());
}

TEST(cli_words_and_quotes) {
    const auto w = cli::split_words("play \"beta tune\" now \"\"");
    CHECK_EQ(w.size(), static_cast<size_t>(4));
    if (w.size() == 4) { CHECK_EQ(w[1], std::string("beta tune")); CHECK_EQ(w[3], std::string("")); }
    CHECK_EQ(cli::unquote("\"a b\""), std::string("a b"));
    CHECK_EQ(cli::unquote(" plain "), std::string("plain"));
}

TEST(cli_word_start_finds_the_word_where_it_stands) {
    // `if volume>50 volume 20`: the command is the word after the condition, not the first place its text occurs in the line
    const std::string l = "volume>50 volume 20";
    CHECK_EQ(cli::word_start(l, 0), static_cast<size_t>(0));
    CHECK_EQ(cli::word_start(l, 1), static_cast<size_t>(10));
    CHECK_EQ(l.substr(cli::word_start(l, 1)), std::string("volume 20"));
    const std::string m = "unmuted mute";                     // "mute" is also inside "unmuted"
    CHECK_EQ(m.substr(cli::word_start(m, 1)), std::string("mute"));
    const std::string q = "  a  \"b c\"   d";                 // spaces inside quotes do not separate; leading / repeated spaces do not count
    CHECK_EQ(cli::word_start(q, 0), static_cast<size_t>(2));
    CHECK_EQ(cli::word_start(q, 1), static_cast<size_t>(5));
    CHECK_EQ(cli::word_start(q, 2), static_cast<size_t>(13));
    CHECK_EQ(cli::word_start(q, 3), std::string::npos);
    CHECK_EQ(cli::word_start("", 0), std::string::npos);
    // it counts the words the way split_words does, also for an empty quoted word and an open quote
    for (const std::string& t : {std::string("a \"\" b"), std::string("x \"y z"), std::string("  \t p \t q ")}) {
        const auto w = cli::split_words(t);
        CHECK(cli::word_start(t, w.size()) == std::string::npos);
        CHECK(w.empty() || cli::word_start(t, w.size() - 1) != std::string::npos);
    }
}

TEST(cli_durations) {
    double s = 0;
    CHECK(cli::parse_duration("90", s) && s == 90.0);
    CHECK(cli::parse_duration("5s", s) && s == 5.0);
    CHECK(cli::parse_duration("10m", s) && s == 600.0);
    CHECK(cli::parse_duration("1h30m", s) && s == 5400.0);
    CHECK(cli::parse_duration("1.5m", s) && s == 90.0);
    CHECK(!cli::parse_duration("", s));
    CHECK(!cli::parse_duration("abc", s));
    CHECK(!cli::parse_duration("1h30", s));       // a number without a unit after one with a unit
    CHECK(!cli::parse_duration("0", s));
    CHECK(!cli::parse_duration("-5", s));
    CHECK(!cli::parse_duration("25h", s));         // more than a day
    CHECK_EQ(cli::duration_text(3725), std::string("1h02m05s"));
    CHECK_EQ(cli::duration_text(75), std::string("1m15s"));
    CHECK_EQ(cli::duration_text(9), std::string("9s"));
}

TEST(cli_conditions) {
    cli::State st;
    st.has_track = true; st.volume = 60; st.queue = 2;
    bool ok = false;
    CHECK(cli::eval_condition("playing", st, ok) && ok);
    CHECK(!cli::eval_condition("paused", st, ok) && ok);
    CHECK(cli::eval_condition("!paused", st, ok) && ok);
    CHECK(cli::eval_condition("not muted", st, ok) && ok);
    CHECK(cli::eval_condition("volume>50", st, ok) && ok);
    CHECK(!cli::eval_condition("volume<50", st, ok) && ok);
    CHECK(cli::eval_condition("volume=60", st, ok) && ok);
    CHECK(cli::eval_condition("queue-nonempty", st, ok) && ok);
    cli::eval_condition("banana", st, ok);
    CHECK(!ok);
    cli::eval_condition("volume>x", st, ok);
    CHECK(!ok);
    st.has_track = false;
    CHECK(cli::eval_condition("stopped", st, ok) && ok);
}

TEST(cli_script_lines) {
    bool soft = true;
    CHECK_EQ(cli::script_line("  # a comment", soft), std::string(""));
    CHECK_EQ(cli::script_line("   ", soft), std::string(""));
    CHECK_EQ(cli::script_line("next", soft), std::string("next"));
    CHECK(!soft);
    CHECK_EQ(cli::script_line("- next", soft), std::string("next"));
    CHECK(soft);
}

TEST(cli_names_and_script_files) {
    CHECK(cli::valid_name("night-mode_2.cli"));
    CHECK(!cli::valid_name(""));
    CHECK(!cli::valid_name(".hidden"));
    CHECK(!cli::valid_name("../etc/passwd"));
    CHECK(!cli::valid_name("a b"));
    CHECK(!cli::valid_name("a..b"));
    mt::TempHome home;
    std::string err;
    CHECK(cli::write_script("night", {"volume 25", "# quiet", "in 45m pause"}, err));
    CHECK(cli::write_script("morning", {"volume 60"}, err));
    CHECK(!cli::write_script("../bad", {"x"}, err));
    const auto names = cli::script_names();
    CHECK_EQ(names.size(), static_cast<size_t>(2));
    if (names.size() == 2) { CHECK_EQ(names[0], std::string("morning")); CHECK_EQ(names[1], std::string("night")); }
    std::vector<std::string> lines;
    CHECK(cli::read_script("night", lines, err));
    CHECK_EQ(lines.size(), static_cast<size_t>(3));
    CHECK(!cli::read_script("nope", lines, err));
    CHECK(!cli::read_script("../night", lines, err));
}

TEST(cli_aliases_round_trip) {
    mt::TempHome home;
    CHECK(cli::load_aliases().empty());
    std::map<std::string, std::string> a{{"night", "volume 25; wait 1s; pause"}, {"up", "volume-up 10"}};
    CHECK(cli::save_aliases(a));
    const auto b = cli::load_aliases();
    CHECK_EQ(b.size(), static_cast<size_t>(2));
    CHECK_EQ(b.count("night") ? b.at("night") : std::string(), std::string("volume 25; wait 1s; pause"));
}

TEST(cli_settings_bridge_only_reaches_flags_and_numbers) {
    Settings s;
    const auto names = cli_setting_names();
    CHECK(!names.empty());
    for (const std::string& n : names) {
        CHECK(n.find("Remote") == std::string::npos);
        CHECK(n.find("Pin") == std::string::npos);
        CHECK(n.find("Path") == std::string::npos);
    }
    bool found = false;
    cli_setting_get(s, "RemoteAppPin", &found);
    CHECK(!found);
    cli_setting_get(s, "LocalMusicPath", &found);
    CHECK(!found);
    CHECK(!cli_setting_set(s, "LocalMusicPath", "/x").empty());
    CHECK(!cli_setting_set(s, "NoSuchSetting", "1").empty());
    std::string canon;
    CHECK_EQ(cli_setting_get(s, "videogradientsaturation", &found), std::string("85"));   // (case does not matter)
    CHECK(found);
    CHECK_EQ(cli_setting_set(s, "videogradientsaturation", "47", &canon), std::string(""));
    CHECK_EQ(canon, std::string("VideoGradientSaturation"));
    CHECK_EQ(s.video_grad_sat, 45);                                                          // (steps of 5)
    CHECK(!cli_setting_set(s, "VideoGradientSaturation", "150").empty());                    // out of range
    CHECK(!cli_setting_set(s, "VideoGradientSaturation", "abc").empty());
    CHECK_EQ(s.video_grad_sat, 45);
    CHECK_EQ(cli_setting_set(s, "VolumeGradient", "off"), std::string(""));
    CHECK(!s.volume_gradient);
    CHECK_EQ(cli_setting_set(s, "VolumeGradient", "toggle"), std::string(""));
    CHECK(s.volume_gradient);
    CHECK(!cli_setting_set(s, "VolumeGradient", "maybe").empty());
}

TEST(cli_text_commands_reach_the_program_with_an_id) {
    mt::TempHome home;
    remote::start("player");
    CHECK_EQ(client({"--cmd", "play", "beta tune"}), 0);                  // text: a command line
    CHECK_EQ(client({"--cli", "volume 30; next"}), 0);
    CHECK_EQ(client({"--cmd", "seek", "-10"}), 0);                        // a number: the old command
    settle();
    const auto cmds = remote::poll();
    CHECK_EQ(cmds.size(), static_cast<size_t>(3));
    if (cmds.size() == 3) {
        CHECK_EQ(cmds[0].name, std::string("cli"));
        CHECK_EQ(cmds[0].arg, std::string("play \"beta tune\""));
        CHECK(cmds[0].id.empty());                                        // no reply wanted
        CHECK_EQ(cmds[1].arg, std::string("volume 30; next"));
        CHECK_EQ(cmds[2].name, std::string("seek"));
    }
    remote::stop();
}

TEST(cli_query_waits_for_the_reply_file) {
    mt::TempHome home;
    remote::start("player");
    std::thread answer([] {   // the "program": takes the command and writes the reply
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            const auto cmds = remote::poll();
            for (const auto& c : cmds) if (c.name == "cli" && !c.id.empty()) { remote::reply(c.id, c.arg != "boom", c.arg == "boom" ? "error: boom" : "volume 42%"); return; }
        }
    });
    CHECK_EQ(client({"--query", "get", "volume"}), 0);
    answer.join();
    std::thread answer2([] {
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            const auto cmds = remote::poll();
            for (const auto& c : cmds) if (c.name == "cli" && !c.id.empty()) { remote::reply(c.id, false, "unknown command"); return; }
        }
    });
    CHECK_EQ(client({"--query", "boom"}), 3);                              // an error reply: exit code 3
    answer2.join();
    CHECK_EQ(client({"--query", "--timeout", "0.5", "nobody answers"}), 4);   // no answer
    remote::stop();
}

TEST(cli_status_json_and_format) {
    mt::TempHome home;
    remote::start("player");
    remote::publish_status({{"mode", "player"}, {"state", "playing"}, {"title", "A \"quoted\" song"}, {"volume", "60"}, {"queue_len", "2"}});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(client({"--status", "--json"}), 0);
    CHECK_EQ(client({"--status", "--format", "{state}: {title}"}), 0);
    CHECK_EQ(client({"--status", "volume"}), 0);
    CHECK_EQ(client({"--status", "nokey"}), 2);
    remote::stop();
}

TEST(cli_script_rename_trash_and_search) {
    mt::TempHome home;
    std::string err;
    CHECK(cli::write_script("night", {"volume 25", "in 45m pause"}, err));
    CHECK(cli::write_script("morning", {"volume 60"}, err));
    CHECK(!cli::rename_script("night", "morning", err));          // an existing name is not overwritten
    CHECK(cli::rename_script("night", "evening", err));
    CHECK(cli::script_names().size() == 2);
    CHECK(!cli::rename_script("night", "x", err));                // gone
    CHECK(!cli::rename_script("evening", "../x", err));
    CHECK(cli::trash_script("evening", err));                     // moved, not erased
    CHECK_EQ(cli::script_names().size(), static_cast<size_t>(1));
    CHECK(std::filesystem::exists(cli::dir() / "deleted" / "evening.cli"));
    CHECK(cli::write_script("evening", {"x"}, err));
    CHECK(cli::trash_script("evening", err));                     // a second one with the same name does not overwrite the first
    CHECK(std::filesystem::exists(cli::dir() / "deleted" / "evening.1.cli"));
    CHECK(!cli::trash_script("../etc/passwd", err));
    CHECK(cli::script_matches("night", "volume 25\nin 45m pause", "PAUSE night"));   // every word, name or text, any case
    CHECK(!cli::script_matches("night", "volume 25", "pause"));
    CHECK(cli::script_matches("night", "", ""));
}

TEST(cli_radio_hooks_and_alias_definition) {
    const std::vector<std::string> lines = {"on station-change = volume 22", "on stream-error = echo x", "on track-change = volume 99", "on pause = echo p"};
    const std::vector<cli::Hook> r = cli::parse_hooks(lines, true), p = cli::parse_hooks(lines, false);
    CHECK_EQ(r.size(), static_cast<size_t>(3));   // the radio: station-change, stream-error, pause
    CHECK_EQ(p.size(), static_cast<size_t>(2));   // the player: track-change, pause
    CHECK_EQ(r[0].event, std::string("station-change"));
    CHECK(cli::is_alias_definition("alias night = volume 25; pause"));
    CHECK(cli::is_alias_definition("  ALIAS a = b"));
    CHECK(!cli::is_alias_definition("alias"));
    CHECK(!cli::is_alias_definition("alias night"));
    CHECK(!cli::is_alias_definition("echo alias a = b"));
}

TEST(cli_hooks_parse_expand_and_limit) {
    const std::vector<cli::Hook> h = cli::parse_hooks({"# my hooks", "on track-change = echo {title}; log {artist}", "ON Pause volume 30", "on nonsense = x", "on resume", "bogus line", "on idle =  "});
    CHECK_EQ(h.size(), static_cast<size_t>(2));
    CHECK_EQ(h[0].event, std::string("track-change"));
    CHECK_EQ(h[0].chain, std::string("echo {title}; log {artist}"));
    CHECK_EQ(h[1].event, std::string("pause"));
    CHECK_EQ(h[1].chain, std::string("volume 30"));
    // a title can never start a second command or carry control characters
    const std::map<std::string, std::string> v = {{"title", "Tonight, Tonight; quit\n"}, {"artist", "The Smashing Pumpkins"}};
    CHECK_EQ(cli::expand_hook("echo {title} by {artist} {nope}", v), std::string("echo Tonight, Tonight, quit by The Smashing Pumpkins {nope}"));
    cli::HookLimiter lim;
    const auto t0 = std::chrono::steady_clock::now();
    int allowed = 0;
    for (int i = 0; i < 20; ++i) allowed += lim.allow(t0 + std::chrono::milliseconds(i * 100)) ? 1 : 0;
    CHECK_EQ(allowed, 8);                                                    // 8 runs in 10 s, then it is cut off
    CHECK(lim.allow(t0 + std::chrono::seconds(30)));                         // and goes on once the window has passed
}
