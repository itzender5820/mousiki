// Remote control: `mousiki --cmd ...` talks to a running program through files in <config>/control/.
#include "test_framework.h"
#include "remote_control.h"

#include <fstream>
#include <algorithm>
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
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(120)); }   // poll() looks at most ten times a second
}

TEST(remote_command_reaches_the_program) {
    mt::TempHome home;
    remote::start("player");
    CHECK_EQ(client({"--cmd", "next"}), 0);
    CHECK_EQ(client({"--cmd", "volume", "60"}), 0);
    CHECK_EQ(client({"--cmd", "seek", "-10"}), 0);
    settle();
    const auto cmds = remote::poll();
    CHECK_EQ(cmds.size(), static_cast<size_t>(3));
    if (cmds.size() == 3) {
        CHECK_EQ(cmds[0].name, std::string("next"));
        CHECK_EQ(cmds[1].name, std::string("volume"));
        CHECK_EQ(cmds[1].arg, std::string("60"));
        CHECK_EQ(cmds[2].arg, std::string("-10"));              // in the order they were sent
        CHECK_NEAR(remote::number(cmds[2].arg, 0), -10.0, 0.001);
    }
    settle();
    CHECK(remote::poll().empty());                               // each command is delivered once
    remote::stop();
}

TEST(remote_bad_input_is_refused) {
    mt::TempHome home;
    remote::start("radio");
    CHECK_EQ(client({"--cmd", "format-disk"}), 0);              // a name that is no numeric command goes to the program as a command line ...
    CHECK_EQ(client({"--cmd", "seek", "abc"}), 2);              // only a short number as the value
    CHECK_EQ(client({"--cmd", "seek", "1234567890123456"}), 2);
    CHECK_EQ(client({"--cmd"}), 2);
    settle();
    const auto got = remote::poll();                            // ... where it is refused ("unknown command"): only that one arrived
    CHECK_EQ(got.size(), static_cast<size_t>(1));
    if (!got.empty()) CHECK_EQ(got[0].name, std::string("cli"));
    remote::stop();
}

TEST(remote_not_running) {
    mt::TempHome home;
    CHECK_EQ(client({"--cmd", "next"}), 1);
    CHECK_EQ(client({"--status"}), 1);
    remote::start("player");
    CHECK_EQ(client({"--cmd", "next"}), 0);
    remote::stop();                                              // quit: the marker is gone
    CHECK_EQ(client({"--cmd", "next"}), 1);
}

TEST(remote_stale_and_flooded_commands) {
    mt::TempHome home;
    std::filesystem::create_directories(remote::dir());
    { std::ofstream f(remote::dir() / "0000000000001-00000001.cmd"); f << "quit\n"; }   // left over from an earlier run
    remote::start("player");
    settle();
    CHECK(remote::poll().empty());                               // must not fire now
    for (int i = 0; i < 40; ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "%013d-%08d.cmd", 100 + i, i);
        std::ofstream f(remote::dir() / name);
        f << "next\n";
    }
    settle();
    CHECK(remote::poll().size() <= 16);                          // a flood is cut off
    settle();
    CHECK(remote::poll().empty());                               // ... and not queued up for later
    remote::stop();
}

TEST(remote_status_is_published) {
    mt::TempHome home;
    remote::start("player");
    remote::publish_status({{"mode", "player"}, {"state", "playing"}, {"title", "line\nbreak"}});
    std::ifstream f(remote::dir() / "status");
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(all.find("state=playing\n") != std::string::npos);
    CHECK(all.find("title=line break\n") != std::string::npos);  // one line per value
    remote::stop();
}

TEST(remote_command_names) {
    const auto names = remote::known_commands();
    for (const char* n : {"play-pause", "next", "prev", "volume-up", "volume-down", "volume", "mute", "seek", "quit"})
        CHECK(std::find(names.begin(), names.end(), std::string(n)) != names.end());
}

TEST(remote_help_and_version_need_no_running_program) {
    mt::TempHome home;
    CHECK_EQ(client({"--cmd", "--help"}), 0);
    CHECK_EQ(client({"--cmd", "help"}), 0);
    CHECK_EQ(client({"--remote-help"}), 0);
    CHECK_EQ(client({"--help"}), 0);
    CHECK_EQ(client({"--version"}), 0);
    CHECK_EQ(client({"--remote-info"}), 0);   // off: says how to switch it on
}
