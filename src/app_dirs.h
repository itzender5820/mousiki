#pragma once
// Where Mousiki keeps its files: the config folder (config.txt, scripts, stations ...) and the cache folder (logs, covers, history ...).
// Windows and macOS: $HOME/.config/mousiki and $HOME/.cache/mousiki, as always (HOME is set by win_bootstrap_env() on Windows).
// Linux and other Unix systems: $XDG_CONFIG_HOME/mousiki and $XDG_CACHE_HOME/mousiki when the variable holds an absolute path -- unless
// that folder does not exist yet while the old one ($HOME/.config/mousiki, $HOME/.cache/mousiki) does: then the old one stays in use, so
// nobody with the variable set loses their settings after an update (move the folder to switch over). Not cached: HOME and the XDG
// variables are read at every call (the tests change them while they run); the folder check happens only when a XDG variable is set.
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "path_utf8.h"

namespace muisc::app_dirs {

inline fs::path home() {
    const char* h = std::getenv("HOME");
#if defined(_WIN32)
    if (!h || !*h) h = std::getenv("USERPROFILE");
#endif
    return h && *h ? path_from_utf8(h) : fs::path(".");
}

inline fs::path pick(const char* xdg_var, const char* dot_dir) {
    const fs::path legacy = home() / dot_dir / "mousiki";
#if !defined(_WIN32) && !defined(__APPLE__)
    if (const char* x = std::getenv(xdg_var); x && *x) {
        const fs::path base = path_from_utf8(x);
        if (base.is_absolute()) {
            const fs::path cand = base / "mousiki";
            std::error_code ec;
            if (fs::exists(cand, ec) || !fs::exists(legacy, ec)) return cand;
        }
    }
#else
    (void)xdg_var;
#endif
    return legacy;
}

inline fs::path config() { return pick("XDG_CONFIG_HOME", ".config"); }
inline fs::path cache() { return pick("XDG_CACHE_HOME", ".cache"); }

}  // namespace muisc::app_dirs
