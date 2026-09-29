#pragma once
#include <string>

namespace muisc {

struct ProcResult {
    std::string out;   // combined stdout (stderr redirected away unless merge_stderr=true)
    int exit_code = -1;
    bool ok() const { return exit_code == 0; }
};

// Runs `cmd` through the shell and captures stdout. Caller is responsible
// for shell-quoting any interpolated arguments (see shell_quote()).
ProcResult run_capture(const std::string& cmd, bool merge_stderr = false);

// Wraps a string in single quotes, safely escaping any embedded quotes,
// so it can be dropped into a shell command line.
std::string shell_quote(const std::string& s);

// Resolves the yt-dlp binary to actually invoke -- already shell-quoted,
// ready to drop straight into a command string (e.g. `ytdlp_binary() +
// " -4 --no-warnings ..."`).
//
// Prefers mousiki's own bundled standalone binary at
// $HOME/.local/share/mousiki/bin/yt-dlp (see setup.sh, which downloads
// it directly from yt-dlp's GitHub releases) over anything found via
// PATH. That standalone build has no Python dependency of its own at
// all -- it's a self-contained PyInstaller executable -- which is the
// actual point: users no longer need a working pip/pip3/pipx to get a
// functioning yt-dlp, which had been the single biggest source of setup
// friction.
//
// Falls back to the bare "yt-dlp" command (relying on PATH) when the
// bundled download doesn't apply -- chiefly Termux, which installs its
// own yt-dlp via `pkg install yt-dlp` instead (a standalone
// glibc-linked Linux PyInstaller binary won't run under Termux's bionic
// libc/proot environment anyway), or a manual yt-dlp install someone
// already has on PATH during development/testing.
std::string ytdlp_binary();

} // namespace muisc
