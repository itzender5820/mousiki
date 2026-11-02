// The console log: 1500 lines, the last session's log above this one's, [v] for the verbose level.
#include "test_framework.h"
#include "console_log.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace muisc;

#ifndef _WIN32
TEST(console_log_keeps_the_last_session_and_1500_lines) {
    namespace fs = std::filesystem;
    const fs::path home = fs::temp_directory_path() / "mousiki_console_log_test";
    fs::remove_all(home);
    fs::create_directories(home);
    setenv("HOME", home.string().c_str(), 1);
    auto& log = ConsoleLog::instance();
    log.init(LogVerbosity::Basic);
    for (int i = 0; i < 1700; ++i) log.log_basic("first " + std::to_string(i));
    CHECK_EQ(static_cast<int>(log.lines().size()), 1500);   // (no last session yet)
    log.init(LogVerbosity::Basic);                           // the next session
    log.log_basic("second one");
    const auto all = log.lines();
    CHECK(fs::exists(home / ".cache" / "mousiki" / "logs" / "console_previous.log"));
    bool has_first = false, has_second = false;
    for (const auto& l : all) { if (l.find("first 1699") != std::string::npos) has_first = true; if (l.find("second one") != std::string::npos) has_second = true; }
    CHECK(has_first);    // the end of the last session is there ...
    CHECK(has_second);   // ... above this session's lines
    CHECK(all.size() > 1500);
    // [v]
    CHECK(log.level() == LogVerbosity::Basic);
    CHECK(log.toggle_level() == LogVerbosity::Verbose);
    log.log_verbose("only in verbose");
    CHECK(log.toggle_level() == LogVerbosity::Basic);
    log.log_verbose("not logged");
    bool a = false, b = false;
    for (const auto& l : log.lines()) { if (l.find("only in verbose") != std::string::npos) a = true; if (l.find("not logged") != std::string::npos) b = true; }
    CHECK(a);
    CHECK(!b);
    fs::remove_all(home);
}
#endif
