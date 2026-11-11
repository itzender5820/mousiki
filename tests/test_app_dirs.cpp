// Where the config and cache folders are: HOME/.config|.cache/mousiki, or the XDG variables on Linux (the old folder stays in use until
// the XDG one exists).
#include "test_framework.h"
#include "app_dirs.h"

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace muisc;

namespace {
void set_env(const char* k, const std::string& v) {
#if defined(_WIN32)
    _putenv_s(k, v.c_str());
#else
    if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1);
#endif
}
struct XdgGuard {   // restores the two variables
    std::string c, k;
    bool hc = false, hk = false;
    XdgGuard() {
        if (const char* v = std::getenv("XDG_CONFIG_HOME")) { c = v; hc = true; }
        if (const char* v = std::getenv("XDG_CACHE_HOME")) { k = v; hk = true; }
    }
    ~XdgGuard() { set_env("XDG_CONFIG_HOME", hc ? c : ""); set_env("XDG_CACHE_HOME", hk ? k : ""); }
};
}  // namespace

TEST(app_dirs_default_is_home_dot_config_and_cache) {
    XdgGuard g;
    mt::TempHome home;
    set_env("XDG_CONFIG_HOME", "");
    set_env("XDG_CACHE_HOME", "");
    CHECK_EQ(path_utf8(app_dirs::config()), path_utf8(home.path() / ".config" / "mousiki"));
    CHECK_EQ(path_utf8(app_dirs::cache()), path_utf8(home.path() / ".cache" / "mousiki"));
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST(app_dirs_xdg_variables_on_linux) {
    XdgGuard g;
    mt::TempHome home;
    const fs::path xc = home.path() / "xdgc", xk = home.path() / "xdgk";
    set_env("XDG_CONFIG_HOME", path_utf8(xc));
    set_env("XDG_CACHE_HOME", path_utf8(xk));
    // nothing exists: the XDG folders
    CHECK_EQ(path_utf8(app_dirs::config()), path_utf8(xc / "mousiki"));
    CHECK_EQ(path_utf8(app_dirs::cache()), path_utf8(xk / "mousiki"));
    // the old folder exists, the XDG one does not: nobody loses their settings
    fs::create_directories(home.path() / ".config" / "mousiki");
    fs::create_directories(home.path() / ".cache" / "mousiki");
    CHECK_EQ(path_utf8(app_dirs::config()), path_utf8(home.path() / ".config" / "mousiki"));
    CHECK_EQ(path_utf8(app_dirs::cache()), path_utf8(home.path() / ".cache" / "mousiki"));
    // both exist: the XDG folder wins
    fs::create_directories(xc / "mousiki");
    fs::create_directories(xk / "mousiki");
    CHECK_EQ(path_utf8(app_dirs::config()), path_utf8(xc / "mousiki"));
    CHECK_EQ(path_utf8(app_dirs::cache()), path_utf8(xk / "mousiki"));
    // a relative path is not an XDG path (the specification says to ignore it)
    set_env("XDG_CONFIG_HOME", "relative/dir");
    CHECK_EQ(path_utf8(app_dirs::config()), path_utf8(home.path() / ".config" / "mousiki"));
}
#endif
